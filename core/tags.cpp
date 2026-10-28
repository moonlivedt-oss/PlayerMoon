#include "core/tags.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "core/text.h"

namespace pm {

namespace {

constexpr size_t kMaxCover = 16u << 20;  // обложки больше 16 МБ не читаем
constexpr size_t kMaxTag = 64u << 20;    // и теги больше 64 МБ тоже

class FileSource : public ByteSource {
public:
    explicit FileSource(const std::filesystem::path& p) : f_(p, std::ios::binary) {
        if (f_) {
            f_.seekg(0, std::ios::end);
            size_ = static_cast<uint64_t>(f_.tellg());
        }
    }
    bool Ok() const { return static_cast<bool>(f_); }
    uint64_t Size() const override { return size_; }
    size_t ReadAt(uint64_t off, void* dst, size_t n) override {
        if (off >= size_) return 0;
        n = static_cast<size_t>(std::min<uint64_t>(n, size_ - off));
        f_.clear();
        f_.seekg(static_cast<std::streamoff>(off));
        f_.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
        return static_cast<size_t>(f_.gcount());
    }

private:
    std::ifstream f_;
    uint64_t size_ = 0;
};

std::vector<uint8_t> ReadBytes(ByteSource& s, uint64_t off, size_t n) {
    std::vector<uint8_t> buf(n);
    buf.resize(s.ReadAt(off, buf.data(), n));
    return buf;
}

uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint32_t Be24(const uint8_t* p) { return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2]; }
uint32_t Le32(const uint8_t* p) { return p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
// «Синхробезопасное» число ID3v2: 7 значащих бит в байте
uint32_t Syncsafe(const uint8_t* p) {
    return (uint32_t(p[0] & 0x7F) << 21) | (uint32_t(p[1] & 0x7F) << 14) | (uint32_t(p[2] & 0x7F) << 7) | (p[3] & 0x7F);
}

// Текст ID3 в одной из 4 кодировок -> UTF-8
std::string DecodeId3Text(int enc, const uint8_t* p, size_t n) {
    switch (enc) {
        case 0: return text::Latin1ToUtf8(p, n);
        case 1:
            if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) return text::Utf16ToUtf8(p + 2, n - 2, true);
            if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) return text::Utf16ToUtf8(p + 2, n - 2, false);
            return text::Utf16ToUtf8(p, n, false);
        case 2: return text::Utf16ToUtf8(p, n, true);
        default: {
            std::string s(reinterpret_cast<const char*>(p), n);
            const size_t z = s.find('\0');
            if (z != std::string::npos) s.resize(z);
            return s;
        }
    }
}

// Длина строки ID3 до терминатора (1 или 2 нулевых байта в зависимости от кодировки)
size_t Id3StringLength(int enc, const uint8_t* p, size_t n) {
    if (enc == 1 || enc == 2) {
        for (size_t i = 0; i + 1 < n; i += 2)
            if (p[i] == 0 && p[i + 1] == 0) return i;
        return n;
    }
    const void* z = std::memchr(p, 0, n);
    return z ? static_cast<size_t>(static_cast<const uint8_t*>(z) - p) : n;
}

void SetIfEmpty(std::string& field, std::string value) {
    value = text::Trim(std::move(value));
    if (field.empty() && !value.empty()) field = std::move(value);
}

// Обратное «рассинхронизирование» ID3v2: FF 00 -> FF
std::vector<uint8_t> RemoveUnsync(const uint8_t* p, size_t n) {
    std::vector<uint8_t> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(p[i]);
        if (p[i] == 0xFF && i + 1 < n && p[i + 1] == 0x00) ++i;
    }
    return out;
}

// Разбор тела ID3v2 (без 10-байтового заголовка)
void ParseId3v2Body(const std::vector<uint8_t>& tag, int major, uint8_t flags, TagInfo& out, bool wantCover) {
    std::vector<uint8_t> unsynced;
    const uint8_t* data = tag.data();
    size_t size = tag.size();
    if ((flags & 0x80) && major < 4) {  // в 2.4 рассинхронизация пофреймовая
        unsynced = RemoveUnsync(data, size);
        data = unsynced.data();
        size = unsynced.size();
    }
    size_t pos = 0;
    if ((flags & 0x40) && major >= 3 && size >= 4) {  // расширенный заголовок пропускаем
        const uint32_t ext = major == 4 ? Syncsafe(data) : Be32(data) + 4;
        pos = std::min<size_t>(size, ext);
    }
    const bool v22 = major == 2;
    const size_t headerLen = v22 ? 6 : 10;
    while (pos + headerLen <= size) {
        const uint8_t* h = data + pos;
        if (h[0] == 0) break;  // дальше заполнитель
        std::string id(reinterpret_cast<const char*>(h), v22 ? 3 : 4);
        uint32_t len = v22 ? Be24(h + 3) : (major == 4 ? Syncsafe(h + 4) : Be32(h + 4));
        const uint16_t frameFlags = v22 ? 0 : static_cast<uint16_t>((h[8] << 8) | h[9]);
        pos += headerLen;
        if (len > size - pos) break;
        const uint8_t* body = data + pos;
        size_t rawLen = len;
        pos += len;
        // Сжатые и зашифрованные кадры не читаем (биты в 2.3 и 2.4 разные)
        const bool packed = major == 4 ? (frameFlags & 0x000C) != 0 : (frameFlags & 0x00C0) != 0;
        if (packed) continue;
        if (major == 4 && (frameFlags & 0x0001) && rawLen >= 4) {  // 2.4: 4 байта длины перед данными
            body += 4;
            rawLen -= 4;
        }
        std::vector<uint8_t> fixed;
        if (major == 4 && (frameFlags & 0x0002)) {  // рассинхронизация кадра в 2.4
            fixed = RemoveUnsync(body, rawLen);
            body = fixed.data();
        }
        const size_t bodyLen = fixed.empty() ? rawLen : fixed.size();
        if (bodyLen < 1) continue;
        const int enc = body[0];
        if (id == "TIT2" || id == "TT2") SetIfEmpty(out.title, DecodeId3Text(enc, body + 1, bodyLen - 1));
        else if (id == "TPE1" || id == "TP1") SetIfEmpty(out.artist, DecodeId3Text(enc, body + 1, bodyLen - 1));
        else if (id == "TALB" || id == "TAL") SetIfEmpty(out.album, DecodeId3Text(enc, body + 1, bodyLen - 1));
        else if (wantCover && out.cover.empty() && (id == "APIC" || id == "PIC")) {
            size_t p = 1;
            std::string mime;
            if (v22) {  // PIC: формат из 3 букв вместо MIME
                if (bodyLen < 5) continue;
                const std::string fmt = text::AsciiLower(std::string(reinterpret_cast<const char*>(body + 1), 3));
                mime = fmt == "png" ? "image/png" : "image/jpeg";
                p = 4;
            } else {
                const void* z = std::memchr(body + 1, 0, bodyLen - 1);
                if (!z) continue;
                mime.assign(reinterpret_cast<const char*>(body + 1), static_cast<const uint8_t*>(z) - (body + 1));
                p = static_cast<size_t>(static_cast<const uint8_t*>(z) - body) + 1;
            }
            if (p + 1 > bodyLen) continue;
            ++p;  // тип картинки
            const size_t descLen = Id3StringLength(enc, body + p, bodyLen - p);
            p += descLen + ((enc == 1 || enc == 2) ? 2 : 1);
            if (p >= bodyLen || bodyLen - p > kMaxCover) continue;
            out.cover.assign(body + p, body + bodyLen);
            out.coverMime = text::AsciiLower(mime);
            if (out.coverMime == "jpg") out.coverMime = "image/jpeg";
        }
    }
}

// ID3v2 в начале файла: возвращает размер тега с заголовком (0, если нет)
uint64_t ReadId3v2(ByteSource& s, uint64_t at, TagInfo& out, bool wantCover) {
    const auto h = ReadBytes(s, at, 10);
    if (h.size() < 10 || std::memcmp(h.data(), "ID3", 3) != 0) return 0;
    const int major = h[3];
    const uint32_t size = Syncsafe(&h[6]);
    const uint64_t total = 10 + size + ((h[5] & 0x10) ? 10 : 0);  // + футер
    if (major < 2 || major > 4 || size > kMaxTag) return total;
    const auto body = ReadBytes(s, at + 10, size);
    ParseId3v2Body(body, major, h[5], out, wantCover);
    return total;
}

void ReadId3v1(ByteSource& s, TagInfo& out) {
    if (s.Size() < 128) return;
    const auto t = ReadBytes(s, s.Size() - 128, 128);
    if (t.size() < 128 || std::memcmp(t.data(), "TAG", 3) != 0) return;
    SetIfEmpty(out.title, text::Latin1ToUtf8(&t[3], 30));
    SetIfEmpty(out.artist, text::Latin1ToUtf8(&t[33], 30));
    SetIfEmpty(out.album, text::Latin1ToUtf8(&t[63], 30));
}

// ---------- MP3: длительность ----------
int Mp3Bitrate(int version, int layer, int index) {
    // version: 3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5; layer: 1 = III, 2 = II
    static const int kV1L3[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    static const int kV1L2[] = {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384};
    static const int kV2L23[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
    if (index <= 0 || index >= 15) return 0;
    if (version == 3) return layer == 1 ? kV1L3[index] : layer == 2 ? kV1L2[index] : 0;
    return (layer == 1 || layer == 2) ? kV2L23[index] : 0;
}

int Mp3SampleRate(int version, int index) {
    static const int kRates[] = {44100, 48000, 32000};
    if (index < 0 || index > 2) return 0;
    const int base = kRates[index];
    return version == 3 ? base : version == 2 ? base / 2 : version == 0 ? base / 4 : 0;
}

double Mp3Duration(ByteSource& s, uint64_t audioStart, uint64_t audioEnd) {
    const auto buf = ReadBytes(s, audioStart, 64 * 1024);
    for (size_t i = 0; i + 4 <= buf.size(); ++i) {
        if (buf[i] != 0xFF || (buf[i + 1] & 0xE0) != 0xE0) continue;
        const int version = (buf[i + 1] >> 3) & 3;
        const int layer = (buf[i + 1] >> 1) & 3;
        const int brIndex = buf[i + 2] >> 4;
        const int srIndex = (buf[i + 2] >> 2) & 3;
        const int channelMode = buf[i + 3] >> 6;
        if (version == 1 || layer == 0 || layer == 3) continue;  // зарезервировано или Layer I
        const int bitrate = Mp3Bitrate(version, layer, brIndex);
        const int rate = Mp3SampleRate(version, srIndex);
        if (!bitrate || !rate) continue;
        const int samplesPerFrame = (layer == 1 && version != 3) ? 576 : 1152;
        // Xing/Info сразу после побочной информации, VBRI - через 32 байта после заголовка
        const size_t side = version == 3 ? (channelMode == 3 ? 17 : 32) : (channelMode == 3 ? 9 : 17);
        const size_t xing = i + 4 + side;
        if (xing + 12 <= buf.size() &&
            (std::memcmp(&buf[xing], "Xing", 4) == 0 || std::memcmp(&buf[xing], "Info", 4) == 0)) {
            const uint32_t flags = Be32(&buf[xing + 4]);
            if (flags & 1) return double(Be32(&buf[xing + 8])) * samplesPerFrame / rate;
        }
        const size_t vbri = i + 4 + 32;
        if (vbri + 18 <= buf.size() && std::memcmp(&buf[vbri], "VBRI", 4) == 0)
            return double(Be32(&buf[vbri + 14])) * samplesPerFrame / rate;
        // Постоянный битрейт: размер аудиоданных / битрейт
        const uint64_t bytes = audioEnd > audioStart + i ? audioEnd - audioStart - i : 0;
        return double(bytes) * 8.0 / (bitrate * 1000.0);
    }
    return 0;
}

// ---------- FLAC ----------
bool ReadFlac(ByteSource& s, uint64_t at, TagInfo& out, bool wantCover) {
    const auto magic = ReadBytes(s, at, 4);
    if (magic.size() < 4 || std::memcmp(magic.data(), "fLaC", 4) != 0) return false;
    uint64_t pos = at + 4;
    for (int guard = 0; guard < 1024; ++guard) {
        const auto h = ReadBytes(s, pos, 4);
        if (h.size() < 4) break;
        const bool last = h[0] & 0x80;
        const int type = h[0] & 0x7F;
        const uint32_t len = Be24(&h[1]);
        pos += 4;
        if (type == 0 && len >= 18) {  // STREAMINFO: частота и число сэмплов
            const auto b = ReadBytes(s, pos, 18);
            if (b.size() == 18) {
                const uint32_t rate = (uint32_t(b[10]) << 12) | (uint32_t(b[11]) << 4) | (b[12] >> 4);
                const uint64_t samples = (uint64_t(b[13] & 0x0F) << 32) | Be32(&b[14]);
                if (rate) out.duration = double(samples) / rate;
            }
        } else if (type == 4 && len <= kMaxTag) {  // VORBIS_COMMENT, числа little endian
            const auto b = ReadBytes(s, pos, len);
            size_t p = 0;
            auto u32 = [&](uint32_t& v) {
                if (p + 4 > b.size()) return false;
                v = Le32(&b[p]);
                p += 4;
                return true;
            };
            uint32_t vendor = 0, count = 0;
            if (u32(vendor) && vendor <= b.size() - p) {
                p += vendor;
                if (u32(count)) {
                    for (uint32_t i = 0; i < count; ++i) {
                        uint32_t n = 0;
                        if (!u32(n) || n > b.size() - p) break;
                        const std::string kv(reinterpret_cast<const char*>(&b[p]), n);
                        p += n;
                        const size_t eq = kv.find('=');
                        if (eq == std::string::npos) continue;
                        const std::string key = text::AsciiLower(kv.substr(0, eq));
                        if (key == "title") SetIfEmpty(out.title, kv.substr(eq + 1));
                        else if (key == "artist") SetIfEmpty(out.artist, kv.substr(eq + 1));
                        else if (key == "album") SetIfEmpty(out.album, kv.substr(eq + 1));
                    }
                }
            }
        } else if (type == 6 && wantCover && out.cover.empty() && len <= kMaxCover + 1024) {  // PICTURE
            const auto b = ReadBytes(s, pos, len);
            size_t p = 4;  // тип картинки
            if (p + 4 <= b.size()) {
                const uint32_t mimeLen = Be32(&b[p]);
                p += 4;
                if (mimeLen <= b.size() - p) {
                    const std::string mime(reinterpret_cast<const char*>(&b[p]), mimeLen);
                    p += mimeLen;
                    if (p + 4 <= b.size()) {
                        const uint32_t descLen = Be32(&b[p]);
                        p += 4;
                        if (descLen <= b.size() - p && b.size() - p - descLen >= 20) {
                            p += descLen + 16;  // ширина, высота, глубина, палитра
                            const uint32_t dataLen = Be32(&b[p]);
                            p += 4;
                            if (dataLen <= b.size() - p) {
                                out.cover.assign(b.begin() + static_cast<long>(p),
                                                 b.begin() + static_cast<long>(p + dataLen));
                                out.coverMime = text::AsciiLower(mime);
                            }
                        }
                    }
                }
            }
        }
        pos += len;
        if (last) break;
    }
    return true;
}

// ---------- WAV ----------
bool ReadWav(ByteSource& s, TagInfo& out, bool wantCover) {
    const auto h = ReadBytes(s, 0, 12);
    if (h.size() < 12 || std::memcmp(h.data(), "RIFF", 4) != 0 || std::memcmp(&h[8], "WAVE", 4) != 0) return false;
    uint64_t pos = 12;
    uint32_t byteRate = 0;
    uint64_t dataSize = 0;
    for (int guard = 0; guard < 4096 && pos + 8 <= s.Size(); ++guard) {
        const auto ch = ReadBytes(s, pos, 8);
        if (ch.size() < 8) break;
        const std::string id(reinterpret_cast<const char*>(ch.data()), 4);
        const uint32_t len = Le32(&ch[4]);
        const uint64_t body = pos + 8;
        if (id == "fmt " && len >= 16) {
            const auto f = ReadBytes(s, body, 16);
            if (f.size() == 16) byteRate = Le32(&f[8]);
        } else if (id == "data") {
            dataSize = std::min<uint64_t>(len, s.Size() - body);
        } else if (id == "LIST" && len >= 4 && len <= kMaxTag) {
            const auto l = ReadBytes(s, body, len);
            if (l.size() >= 4 && std::memcmp(l.data(), "INFO", 4) == 0) {
                size_t p = 4;
                while (p + 8 <= l.size()) {
                    const std::string sub(reinterpret_cast<const char*>(&l[p]), 4);
                    const uint32_t n = Le32(&l[p + 4]);
                    p += 8;
                    if (n > l.size() - p) break;
                    const std::string value(reinterpret_cast<const char*>(&l[p]), n);
                    if (sub == "INAM") SetIfEmpty(out.title, value);
                    else if (sub == "IART") SetIfEmpty(out.artist, value);
                    else if (sub == "IPRD") SetIfEmpty(out.album, value);
                    p += n + (n & 1);
                }
            }
        } else if (id == "id3 " || id == "ID3 ") {
            ReadId3v2(s, body, out, wantCover);
        }
        pos = body + len + (len & 1);  // чанки выровнены по 2 байта
    }
    if (byteRate && dataSize) out.duration = double(dataSize) / byteRate;
    return true;
}

}  // namespace

bool IsSupportedAudio(const std::filesystem::path& path) {
    const std::string ext = text::AsciiLower(text::PathToUtf8(path.extension()));
    return ext == ".mp3" || ext == ".flac" || ext == ".wav";
}

bool ReadTags(ByteSource& s, TagInfo& out, bool wantCover) {
    out = TagInfo{};
    if (ReadWav(s, out, wantCover)) return true;
    const uint64_t id3 = ReadId3v2(s, 0, out, wantCover);
    if (ReadFlac(s, id3, out, wantCover)) return true;
    // Иначе считаем MP3
    ReadId3v1(s, out);
    uint64_t end = s.Size();
    if (end >= 128) {
        const auto t = ReadBytes(s, end - 128, 3);
        if (t.size() == 3 && std::memcmp(t.data(), "TAG", 3) == 0) end -= 128;
    }
    out.duration = Mp3Duration(s, id3, end);
    return true;
}

bool ReadTags(const std::filesystem::path& path, TagInfo& out, bool wantCover) {
    FileSource src(path);
    if (!src.Ok()) return false;
    return ReadTags(src, out, wantCover);
}

}  // namespace pm
