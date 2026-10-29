#include "core/tags.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <string_view>

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

// Число из текста тега: "-6.53 dB", "+1.2", "0.98"; не зависит от локали
bool ParseTagFloat(std::string v, float& out) {
    v = text::Trim(std::move(v));
    if (!v.empty() && v[0] == '+') v.erase(0, 1);
    const char* b = v.data();
    const auto r = std::from_chars(b, b + v.size(), out);
    return r.ec == std::errc{};
}

// Поля, общие для ID3v2 TXXX и Vorbis Comment (ключ в нижнем регистре)
void ApplyExtraField(const std::string& key, const std::string& value, TagInfo& out) {
    float f = 0;
    if (key == "replaygain_track_gain" && ParseTagFloat(value, f)) {
        out.hasReplayGain = true;
        out.replayGainDb = f;
    } else if (key == "replaygain_track_peak" && ParseTagFloat(value, f) && f > 0) {
        out.replayPeak = f;
    } else if ((key == "tracknumber" || key == "trck") && out.track == 0) {
        out.track = std::atoi(value.c_str());  // "3/12" -> 3
    }
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
        else if (id == "TRCK" || id == "TRK") ApplyExtraField("trck", DecodeId3Text(enc, body + 1, bodyLen - 1), out);
        else if (id == "TXXX" || id == "TXX") {
            // Описание (до нуля в своей кодировке), затем значение
            const size_t descLen = Id3StringLength(enc, body + 1, bodyLen - 1);
            const size_t valueAt = 1 + descLen + ((enc == 1 || enc == 2) ? 2 : 1);
            if (valueAt <= bodyLen)
                ApplyExtraField(text::AsciiLower(DecodeId3Text(enc, body + 1, descLen)),
                                DecodeId3Text(enc, body + valueAt, bodyLen - valueAt), out);
        }
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
// Блок PICTURE (он же METADATA_BLOCK_PICTURE в Vorbis Comment после base64)
void ParseFlacPicture(const uint8_t* b, size_t size, TagInfo& out) {
    size_t p = 4;  // тип картинки
    if (p + 4 > size) return;
    const uint32_t mimeLen = Be32(b + p);
    p += 4;
    if (mimeLen > size - p) return;
    const std::string mime(reinterpret_cast<const char*>(b + p), mimeLen);
    p += mimeLen;
    if (p + 4 > size) return;
    const uint32_t descLen = Be32(b + p);
    p += 4;
    if (descLen > size - p || size - p - descLen < 20) return;
    p += descLen + 16;  // ширина, высота, глубина, палитра
    const uint32_t dataLen = Be32(b + p);
    p += 4;
    if (dataLen > size - p || dataLen > kMaxCover) return;
    out.cover.assign(b + p, b + p + dataLen);
    out.coverMime = text::AsciiLower(mime);
}

// base64 без переносов строк; неверные символы пропускаются
std::vector<uint8_t> Base64Decode(std::string_view in) {
    std::vector<uint8_t> out;
    out.reserve(in.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=') break;
        else continue;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>(acc >> bits));
        }
    }
    return out;
}

// Vorbis Comment (FLAC, Ogg Vorbis, Opus): числа little endian, поля «КЛЮЧ=значение»
void ParseVorbisComment(const uint8_t* b, size_t size, TagInfo& out, bool wantCover) {
    size_t p = 0;
    auto u32 = [&](uint32_t& v) {
        if (p + 4 > size) return false;
        v = Le32(b + p);
        p += 4;
        return true;
    };
    uint32_t vendor = 0, count = 0;
    if (!u32(vendor) || vendor > size - p) return;
    p += vendor;
    if (!u32(count)) return;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t n = 0;
        if (!u32(n) || n > size - p) break;
        const std::string_view kv(reinterpret_cast<const char*>(b + p), n);
        p += n;
        const size_t eq = kv.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string key = text::AsciiLower(std::string(kv.substr(0, eq)));
        const std::string_view value = kv.substr(eq + 1);
        if (key == "title") SetIfEmpty(out.title, std::string(value));
        else if (key == "artist") SetIfEmpty(out.artist, std::string(value));
        else if (key == "album") SetIfEmpty(out.album, std::string(value));
        else if (key == "metadata_block_picture") {
            if (wantCover && out.cover.empty()) {
                const auto pic = Base64Decode(value);
                ParseFlacPicture(pic.data(), pic.size(), out);
            }
        } else ApplyExtraField(key, std::string(value), out);
    }
}

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
        } else if (type == 4 && len <= kMaxTag) {  // VORBIS_COMMENT
            const auto b = ReadBytes(s, pos, len);
            ParseVorbisComment(b.data(), b.size(), out, wantCover);
        } else if (type == 6 && wantCover && out.cover.empty() && len <= kMaxCover + 1024) {  // PICTURE
            const auto b = ReadBytes(s, pos, len);
            ParseFlacPicture(b.data(), b.size(), out);
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

// ---------- MP4 (M4A, AAC, ALAC) ----------
// Дерево атомов: [размер 4][тип 4][данные]. Размер 1 - следом 64-битный, 0 - до конца родителя.
struct Atom {
    std::string type;
    uint64_t body = 0;  // смещение данных
    uint64_t size = 0;  // длина данных
};

// Атомы внутри [from, to) буфера в памяти
std::vector<Atom> Mp4Children(const std::vector<uint8_t>& b, uint64_t from, uint64_t to) {
    std::vector<Atom> out;
    uint64_t pos = from;
    while (pos + 8 <= to && out.size() < 4096) {
        uint64_t len = Be32(&b[pos]);
        uint64_t head = 8;
        if (len == 1) {
            if (pos + 16 > to) break;
            len = (uint64_t(Be32(&b[pos + 8])) << 32) | Be32(&b[pos + 12]);
            head = 16;
        } else if (len == 0) {
            len = to - pos;
        }
        if (len < head || len > to - pos) break;
        out.push_back({std::string(reinterpret_cast<const char*>(&b[pos + 4]), 4), pos + head, len - head});
        pos += len;
    }
    return out;
}

// Копия, а не указатель: списки детей обычно временные
std::optional<Atom> FindAtom(const std::vector<Atom>& v, const char* type) {
    for (const Atom& a : v)
        if (a.type == type) return a;
    return std::nullopt;
}

// Значение элемента ilst: атом data = [тип 4][локаль 4][данные]
bool Mp4Data(const std::vector<uint8_t>& b, const Atom& item, uint32_t& type, uint64_t& at, uint64_t& len) {
    const auto d = FindAtom(Mp4Children(b, item.body, item.body + item.size), "data");
    if (!d || d->size < 8) return false;
    type = Be32(&b[d->body]) & 0xFFFFFF;
    at = d->body + 8;
    len = d->size - 8;
    return true;
}

void ParseIlst(const std::vector<uint8_t>& b, const Atom& ilst, TagInfo& out, bool wantCover) {
    for (const Atom& item : Mp4Children(b, ilst.body, ilst.body + ilst.size)) {
        uint32_t type = 0;
        uint64_t at = 0, len = 0;
        if (item.type == "----") {
            // Свободное поле iTunes: mean (com.apple.iTunes), name (replaygain_track_gain), data
            const auto parts = Mp4Children(b, item.body, item.body + item.size);
            const auto name = FindAtom(parts, "name");
            if (!name || name->size < 4 || !Mp4Data(b, item, type, at, len)) continue;
            const std::string key = text::AsciiLower(
                std::string(reinterpret_cast<const char*>(&b[name->body + 4]), name->size - 4));
            ApplyExtraField(key, std::string(reinterpret_cast<const char*>(&b[at]), len), out);
            continue;
        }
        if (!Mp4Data(b, item, type, at, len)) continue;
        const std::string value(reinterpret_cast<const char*>(&b[at]), len);
        if (item.type == "\xA9nam") SetIfEmpty(out.title, value);
        else if (item.type == "\xA9" "ART") SetIfEmpty(out.artist, value);
        else if (item.type == "\xA9" "alb") SetIfEmpty(out.album, value);
        else if (item.type == "trkn" && len >= 4 && out.track == 0) out.track = (b[at + 2] << 8) | b[at + 3];
        else if (item.type == "covr" && wantCover && out.cover.empty() && len > 0 && len <= kMaxCover) {
            out.cover.assign(b.begin() + static_cast<long>(at), b.begin() + static_cast<long>(at + len));
            // 13 - JPEG, 14 - PNG; иначе по сигнатуре
            const bool png = type == 14 || (len >= 4 && b[at] == 0x89 && b[at + 1] == 'P');
            out.coverMime = png ? "image/png" : "image/jpeg";
        }
    }
}

bool ReadMp4(ByteSource& s, TagInfo& out, bool wantCover) {
    const auto h = ReadBytes(s, 0, 8);
    if (h.size() < 8 || std::memcmp(&h[4], "ftyp", 4) != 0) return false;
    // Ищем moov среди атомов верхнего уровня (он бывает и после mdat)
    uint64_t pos = 0;
    for (int guard = 0; guard < 1024 && pos + 8 <= s.Size(); ++guard) {
        const auto a = ReadBytes(s, pos, 16);
        if (a.size() < 8) break;
        uint64_t len = Be32(a.data());
        uint64_t head = 8;
        if (len == 1 && a.size() == 16) {
            len = (uint64_t(Be32(&a[8])) << 32) | Be32(&a[12]);
            head = 16;
        } else if (len == 0) {
            len = s.Size() - pos;
        }
        if (len < head) break;
        if (std::memcmp(&a[4], "moov", 4) == 0) {
            if (len - head > kMaxTag) return true;
            const auto moov = ReadBytes(s, pos + head, static_cast<size_t>(len - head));
            const auto top = Mp4Children(moov, 0, moov.size());
            if (const auto mvhd = FindAtom(top, "mvhd"); mvhd && mvhd->size >= 32) {
                const uint8_t* m = &moov[mvhd->body];
                // Версия 1: 64-битные времена и длительность
                const uint32_t scale = m[0] == 1 ? Be32(m + 20) : Be32(m + 12);
                const uint64_t dur = m[0] == 1 ? (uint64_t(Be32(m + 24)) << 32) | Be32(m + 28) : Be32(m + 16);
                if (scale) out.duration = double(dur) / scale;
            }
            if (const auto udta = FindAtom(top, "udta")) {
                const auto meta = FindAtom(Mp4Children(moov, udta->body, udta->body + udta->size), "meta");
                if (meta && meta->size >= 8) {
                    // meta - «полный» атом с 4 байтами версии; у старых QuickTime-файлов их нет
                    const uint64_t skip = std::memcmp(&moov[meta->body + 4], "hdlr", 4) == 0 ? 0 : 4;
                    const auto items = Mp4Children(moov, meta->body + skip, meta->body + meta->size);
                    if (const auto ilst = FindAtom(items, "ilst")) ParseIlst(moov, *ilst, out, wantCover);
                }
            }
            return true;
        }
        if (len > s.Size() - pos) break;
        pos += len;
    }
    return true;
}

// ---------- Ogg (Vorbis, Opus) ----------
// Страница: "OggS", версия, тип, позиция 8, поток 4, номер 4, CRC 4, число сегментов, таблица сегментов.
struct OggPage {
    uint64_t granule = 0;
    uint32_t serial = 0;
    uint64_t dataAt = 0;
    std::vector<uint8_t> lacing;
    uint64_t next = 0;
};

bool ReadOggPage(ByteSource& s, uint64_t at, OggPage& pg) {
    const auto h = ReadBytes(s, at, 27);
    if (h.size() < 27 || std::memcmp(h.data(), "OggS", 4) != 0) return false;
    pg.granule = (uint64_t(Le32(&h[10])) << 32) | Le32(&h[6]);
    pg.serial = Le32(&h[14]);
    pg.lacing = ReadBytes(s, at + 27, h[26]);
    if (pg.lacing.size() < h[26]) return false;
    pg.dataAt = at + 27 + h[26];
    uint64_t body = 0;
    for (uint8_t l : pg.lacing) body += l;
    pg.next = pg.dataAt + body;
    return true;
}

// Первые пакеты первого логического потока (заголовки кодека)
std::vector<std::vector<uint8_t>> OggHeaderPackets(ByteSource& s, size_t want, uint32_t& serial) {
    std::vector<std::vector<uint8_t>> packets;
    std::vector<uint8_t> cur;
    uint64_t at = 0;
    bool first = true;
    for (int guard = 0; guard < 4096 && packets.size() < want; ++guard) {
        OggPage pg;
        if (!ReadOggPage(s, at, pg)) break;
        at = pg.next;
        if (first) serial = pg.serial, first = false;
        if (pg.serial != serial) continue;
        uint64_t off = pg.dataAt;
        for (uint8_t l : pg.lacing) {
            const auto part = ReadBytes(s, off, l);
            off += l;
            if (cur.size() + part.size() > kMaxTag) return packets;
            cur.insert(cur.end(), part.begin(), part.end());
            if (l < 255) {  // сегмент короче 255 закрывает пакет
                packets.push_back(std::move(cur));
                cur.clear();
                if (packets.size() >= want) break;
            }
        }
    }
    return packets;
}

// Позиция последней страницы потока: ищем "OggS" с конца файла
uint64_t OggLastGranule(ByteSource& s, uint32_t serial) {
    const uint64_t size = s.Size();
    const uint64_t from = size > 65536 ? size - 65536 : 0;
    const auto tail = ReadBytes(s, from, static_cast<size_t>(size - from));
    for (size_t i = tail.size() >= 27 ? tail.size() - 27 : 0;; --i) {
        if (i + 27 <= tail.size() && std::memcmp(&tail[i], "OggS", 4) == 0 && Le32(&tail[i + 14]) == serial) {
            const uint64_t g = (uint64_t(Le32(&tail[i + 10])) << 32) | Le32(&tail[i + 6]);
            if (g != ~0ull) return g;  // -1 - на странице нет конца пакета
        }
        if (i == 0) break;
    }
    return 0;
}

bool ReadOgg(ByteSource& s, TagInfo& out, bool wantCover) {
    const auto magic = ReadBytes(s, 0, 4);
    if (magic.size() < 4 || std::memcmp(magic.data(), "OggS", 4) != 0) return false;
    uint32_t serial = 0;
    const auto packets = OggHeaderPackets(s, 2, serial);
    if (packets.size() < 2) return true;
    const auto& id = packets[0];
    const auto& tags = packets[1];
    if (id.size() >= 19 && std::memcmp(id.data(), "OpusHead", 8) == 0) {
        // Opus: позиция всегда в 48 кГц, первые preSkip сэмплов отбрасываются
        const uint32_t preSkip = id[10] | (id[11] << 8);
        if (tags.size() >= 8 && std::memcmp(tags.data(), "OpusTags", 8) == 0)
            ParseVorbisComment(tags.data() + 8, tags.size() - 8, out, wantCover);
        const uint64_t g = OggLastGranule(s, serial);
        if (g > preSkip) out.duration = double(g - preSkip) / 48000.0;
    } else if (id.size() >= 16 && std::memcmp(id.data(), "\x01vorbis", 7) == 0) {
        const uint32_t rate = Le32(&id[12]);
        if (tags.size() >= 7 && std::memcmp(tags.data(), "\x03vorbis", 7) == 0)
            ParseVorbisComment(tags.data() + 7, tags.size() - 7, out, wantCover);
        const uint64_t g = OggLastGranule(s, serial);
        if (rate && g) out.duration = double(g) / rate;
    }
    return true;
}

}  // namespace

float ReplayGainFactor(const TagInfo& t) {
    if (!t.hasReplayGain) return 1.f;
    float k = std::pow(10.f, t.replayGainDb / 20.f);
    if (t.replayPeak > 0 && k * t.replayPeak > 1.f) k = 1.f / t.replayPeak;  // без перегрузки
    return std::clamp(k, 0.1f, 4.f);
}

bool ReadFolderCover(const std::filesystem::path& audio, std::vector<uint8_t>& out, std::string& mime) {
    std::error_code ec;
    const std::filesystem::path dir = audio.parent_path();
    if (dir.empty()) return false;
    // Имена по убыванию предпочтения
    static const char* const kNames[] = {"cover", "folder", "front", "album"};
    std::filesystem::path best;
    int bestRank = 100;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string stem = text::AsciiLower(text::PathToUtf8(it->path().stem()));
        const std::string ext = text::AsciiLower(text::PathToUtf8(it->path().extension()));
        if (ext != ".jpg" && ext != ".jpeg" && ext != ".png") continue;
        for (int i = 0; i < 4; ++i)
            if (stem == kNames[i] && i < bestRank) {
                bestRank = i;
                best = it->path();
            }
    }
    if (best.empty()) return false;
    const uint64_t size = std::filesystem::file_size(best, ec);
    if (ec || size == 0 || size > kMaxCover) return false;
    FileSource src(best);
    if (!src.Ok()) return false;
    out = ReadBytes(src, 0, static_cast<size_t>(size));
    mime = text::AsciiLower(text::PathToUtf8(best.extension())) == ".png" ? "image/png" : "image/jpeg";
    return out.size() == size;
}

bool IsSupportedAudio(const std::filesystem::path& path) {
    const std::string ext = text::AsciiLower(text::PathToUtf8(path.extension()));
    if (ext == ".mp3" || ext == ".flac" || ext == ".wav") return true;
#ifdef _WIN32
    // Через Media Foundation (core/decoder_mf.cpp); Ogg и Opus - с «Расширениями для веб-мультимедиа»
    return ext == ".m4a" || ext == ".m4b" || ext == ".aac" || ext == ".ogg" || ext == ".oga" ||
           ext == ".opus";
#else
    return false;
#endif
}

bool ReadTags(ByteSource& s, TagInfo& out, bool wantCover) {
    out = TagInfo{};
    if (ReadWav(s, out, wantCover)) return true;
    if (ReadMp4(s, out, wantCover)) return true;
    if (ReadOgg(s, out, wantCover)) return true;
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
