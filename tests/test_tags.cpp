// Теги на синтетических файлах: ID3v2.3 и 2.4 с обложкой, ID3v1, FLAC, WAV, длительность MP3
#include <cmath>
#include <cstring>

#include "core/tags.h"
#include "tests/check.h"
#include "tests/wav_util.h"

namespace {

using tests::PutBe32;
using tests::PutStr;

class MemSource : public pm::ByteSource {
public:
    explicit MemSource(std::vector<uint8_t> b) : b_(std::move(b)) {}
    uint64_t Size() const override { return b_.size(); }
    size_t ReadAt(uint64_t off, void* dst, size_t n) override {
        if (off >= b_.size()) return 0;
        n = std::min<size_t>(n, b_.size() - static_cast<size_t>(off));
        std::memcpy(dst, b_.data() + off, n);
        return n;
    }

private:
    std::vector<uint8_t> b_;
};

void PutSyncsafe(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>((v >> 21) & 0x7F));
    b.push_back(static_cast<uint8_t>((v >> 14) & 0x7F));
    b.push_back(static_cast<uint8_t>((v >> 7) & 0x7F));
    b.push_back(static_cast<uint8_t>(v & 0x7F));
}

void Frame(std::vector<uint8_t>& tag, const std::string& id, const std::vector<uint8_t>& body, int major) {
    PutStr(tag, id);
    if (major == 4) PutSyncsafe(tag, static_cast<uint32_t>(body.size()));
    else PutBe32(tag, static_cast<uint32_t>(body.size()));
    tag.push_back(0);
    tag.push_back(0);
    tag.insert(tag.end(), body.begin(), body.end());
}

std::vector<uint8_t> TextBody(int enc, const std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> b{static_cast<uint8_t>(enc)};
    b.insert(b.end(), bytes.begin(), bytes.end());
    return b;
}

std::vector<uint8_t> Utf8(const std::string& s) { return {s.begin(), s.end()}; }

// UTF-16LE с BOM (кодировка 1 в ID3)
std::vector<uint8_t> Utf16Bom(const std::u16string& s) {
    std::vector<uint8_t> b{0xFF, 0xFE};
    for (char16_t c : s) {
        b.push_back(static_cast<uint8_t>(c));
        b.push_back(static_cast<uint8_t>(c >> 8));
    }
    return b;
}

const std::vector<uint8_t> kPng = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 1, 2, 3, 4};

std::vector<uint8_t> MakeId3(int major) {
    std::vector<uint8_t> frames;
    Frame(frames, "TIT2", TextBody(1, Utf16Bom(u"Лунный свет")), major);
    Frame(frames, "TPE1", TextBody(3, Utf8("Северный Ветер")), major);
    Frame(frames, "TALB", TextBody(0, {'A', 'l', 'b', 0xE9}), major);  // Latin-1: é
    Frame(frames, "TRCK", TextBody(0, Utf8("7/12")), major);
    // ReplayGain: TXXX с описанием и значением
    std::vector<uint8_t> rg{3};
    PutStr(rg, "REPLAYGAIN_TRACK_GAIN");
    rg.push_back(0);
    PutStr(rg, "-6.02 dB");
    Frame(frames, "TXXX", rg, major);
    std::vector<uint8_t> apic{3};  // UTF-8
    PutStr(apic, "image/png");
    apic.push_back(0);
    apic.push_back(3);  // передняя обложка
    PutStr(apic, "desc");
    apic.push_back(0);
    apic.insert(apic.end(), kPng.begin(), kPng.end());
    Frame(frames, "APIC", apic, major);
    std::vector<uint8_t> tag;
    PutStr(tag, "ID3");
    tag.push_back(static_cast<uint8_t>(major));
    tag.push_back(0);
    tag.push_back(0);
    PutSyncsafe(tag, static_cast<uint32_t>(frames.size() + 16));  // с заполнителем
    tag.insert(tag.end(), frames.begin(), frames.end());
    tag.insert(tag.end(), 16, 0);
    return tag;
}

// Кадр MPEG1 Layer III 128 кбит/с 44.1 кГц стерео с заголовком Xing на frames кадров
std::vector<uint8_t> MakeXingFrame(uint32_t frames) {
    std::vector<uint8_t> f{0xFF, 0xFB, 0x90, 0x00};
    f.insert(f.end(), 32, 0);  // побочная информация
    PutStr(f, "Xing");
    PutBe32(f, 1);  // есть число кадров
    PutBe32(f, frames);
    f.resize(417, 0);
    return f;
}

}  // namespace

TEST(Id3v23And24WithCover) {
    for (int major : {3, 4}) {
        std::vector<uint8_t> file = MakeId3(major);
        const auto frame = MakeXingFrame(1000);
        file.insert(file.end(), frame.begin(), frame.end());
        MemSource src(file);
        pm::TagInfo t;
        CHECK(pm::ReadTags(src, t, true));
        CHECK(t.title == "Лунный свет");
        CHECK(t.artist == "Северный Ветер");
        CHECK(t.album == "Alb\xC3\xA9");
        CHECK(t.coverMime == "image/png");
        CHECK(t.cover == kPng);
        CHECK(t.track == 7);
        CHECK(t.hasReplayGain && std::abs(t.replayGainDb + 6.02f) < 1e-4f);
        CHECK(std::abs(pm::ReplayGainFactor(t) - 0.5f) < 0.001f);  // -6.02 дБ = половина
        // 1000 кадров * 1152 сэмпла / 44100 = 26.12 с
        CHECK_NEAR(t.duration, 1000.0 * 1152 / 44100, 0.01);
        // Без обложки - быстрее и без картинки
        pm::TagInfo noCover;
        CHECK(pm::ReadTags(src, noCover, false));
        CHECK(noCover.cover.empty() && noCover.title == "Лунный свет");
    }
}

TEST(Id3v1AndCbrDuration) {
    // Постоянный битрейт 128 кбит/с без Xing: длительность по размеру
    std::vector<uint8_t> file;
    for (int i = 0; i < 100; ++i) {
        std::vector<uint8_t> f{0xFF, 0xFB, 0x90, 0x00};
        f.resize(417, 0x11);
        file.insert(file.end(), f.begin(), f.end());
    }
    std::vector<uint8_t> v1(128, 0);
    std::memcpy(v1.data(), "TAG", 3);
    std::memcpy(v1.data() + 3, "Old Title", 9);
    std::memcpy(v1.data() + 33, "Old Artist", 10);
    file.insert(file.end(), v1.begin(), v1.end());
    MemSource src(file);
    pm::TagInfo t;
    CHECK(pm::ReadTags(src, t, false));
    CHECK(t.title == "Old Title" && t.artist == "Old Artist");
    CHECK_NEAR(t.duration, 100 * 417 * 8.0 / 128000, 0.01);
}

TEST(FlacTagsAndPicture) {
    std::vector<uint8_t> f;
    PutStr(f, "fLaC");
    // STREAMINFO: 44100 Гц, 441000 сэмплов = 10 с
    std::vector<uint8_t> si(34, 0);
    const uint32_t rate = 44100;
    const uint64_t samples = 441000;
    si[10] = static_cast<uint8_t>(rate >> 12);
    si[11] = static_cast<uint8_t>(rate >> 4);
    si[12] = static_cast<uint8_t>(((rate & 0xF) << 4) | (1 << 1));
    si[13] = static_cast<uint8_t>((15 << 4) | ((samples >> 32) & 0xF));
    si[14] = static_cast<uint8_t>(samples >> 24);
    si[15] = static_cast<uint8_t>(samples >> 16);
    si[16] = static_cast<uint8_t>(samples >> 8);
    si[17] = static_cast<uint8_t>(samples);
    f.push_back(0);
    f.push_back(0);
    f.push_back(0);
    f.push_back(34);
    f.insert(f.end(), si.begin(), si.end());
    // VORBIS_COMMENT (числа little endian)
    std::vector<uint8_t> vc;
    tests::PutLe32(vc, 3);
    PutStr(vc, "pm!");
    const std::vector<std::string> comments = {"TITLE=Город спит", "artist=Кварц", "ALBUM=Ночные окна",
                                                "REPLAYGAIN_TRACK_GAIN=+3.5 dB", "replaygain_track_peak=0.9",
                                                "TRACKNUMBER=2"};
    tests::PutLe32(vc, static_cast<uint32_t>(comments.size()));
    for (const auto& c : comments) {
        tests::PutLe32(vc, static_cast<uint32_t>(c.size()));
        PutStr(vc, c);
    }
    f.push_back(4);
    f.push_back(0);
    f.push_back(static_cast<uint8_t>(vc.size() >> 8));
    f.push_back(static_cast<uint8_t>(vc.size()));
    f.insert(f.end(), vc.begin(), vc.end());
    // PICTURE, последний блок
    std::vector<uint8_t> pic;
    PutBe32(pic, 3);
    PutBe32(pic, 10);
    PutStr(pic, "image/jpeg");
    PutBe32(pic, 0);
    for (int i = 0; i < 4; ++i) PutBe32(pic, 0);
    const std::vector<uint8_t> jpg = {0xFF, 0xD8, 0xFF, 0xE0, 9, 9};
    PutBe32(pic, static_cast<uint32_t>(jpg.size()));
    pic.insert(pic.end(), jpg.begin(), jpg.end());
    f.push_back(0x80 | 6);
    f.push_back(0);
    f.push_back(static_cast<uint8_t>(pic.size() >> 8));
    f.push_back(static_cast<uint8_t>(pic.size()));
    f.insert(f.end(), pic.begin(), pic.end());
    MemSource src(f);
    pm::TagInfo t;
    CHECK(pm::ReadTags(src, t, true));
    CHECK(t.title == "Город спит" && t.artist == "Кварц" && t.album == "Ночные окна");
    CHECK_NEAR(t.duration, 10.0, 1e-9);
    CHECK(t.coverMime == "image/jpeg" && t.cover == jpg);
    CHECK(t.track == 2 && t.hasReplayGain && std::abs(t.replayGainDb - 3.5f) < 1e-4f);
    // +3.5 дБ = 1.496, но пик 0.9: без перегрузки множитель не больше 1/0.9
    CHECK(std::abs(pm::ReplayGainFactor(t) - 1.f / 0.9f) < 0.001f);
    pm::TagInfo none;
    CHECK(pm::ReplayGainFactor(none) == 1.f);
}

TEST(FolderCover) {
    const auto dir = tests::TempDir("folder_cover");
    tests::WriteBytes(dir / "01.wav", tests::MakeWav(0.1));
    std::vector<uint8_t> out;
    std::string mime;
    CHECK(!pm::ReadFolderCover(dir / "01.wav", out, mime));
    tests::WriteBytes(dir / "Front.PNG", kPng);
    tests::WriteBytes(dir / "readme.jpg", {1, 2});
    CHECK(pm::ReadFolderCover(dir / "01.wav", out, mime));
    CHECK(out == kPng && mime == "image/png");
    // cover.jpg важнее front.png
    const std::vector<uint8_t> jpg = {0xFF, 0xD8, 0xFF, 7};
    tests::WriteBytes(dir / "Cover.jpg", jpg);
    CHECK(pm::ReadFolderCover(dir / "01.wav", out, mime));
    CHECK(out == jpg && mime == "image/jpeg");
}

TEST(WavInfoAndDuration) {
    std::vector<uint8_t> list;
    PutStr(list, "LIST");
    std::vector<uint8_t> body;
    PutStr(body, "INFO");
    auto sub = [&](const char* id, const std::string& v) {
        PutStr(body, id);
        tests::PutLe32(body, static_cast<uint32_t>(v.size() + 1));
        PutStr(body, v);
        body.push_back(0);
        if ((v.size() + 1) & 1) body.push_back(0);
    };
    sub("INAM", "Тёплый шум");
    sub("IART", "Ольга Ветрова");
    tests::PutLe32(list, static_cast<uint32_t>(body.size()));
    list.insert(list.end(), body.begin(), body.end());
    MemSource src(tests::MakeWav(0.5, 44100, 440, list));
    pm::TagInfo t;
    CHECK(pm::ReadTags(src, t, false));
    CHECK(t.title == "Тёплый шум" && t.artist == "Ольга Ветрова");
    CHECK_NEAR(t.duration, 0.5, 1e-3);
}

TEST(BrokenFilesDoNotCrash) {
    // Обрезанные и мусорные данные: функция просто ничего не находит
    std::vector<uint8_t> id3 = MakeId3(3);
    id3.resize(30);
    MemSource a(id3);
    pm::TagInfo t;
    CHECK(pm::ReadTags(a, t, true));
    std::vector<uint8_t> junk(5000);
    for (size_t i = 0; i < junk.size(); ++i) junk[i] = static_cast<uint8_t>(i * 131 + 7);
    MemSource b(junk);
    CHECK(pm::ReadTags(b, t, true));
    MemSource c(std::vector<uint8_t>{'f', 'L', 'a', 'C', 0x86, 0xFF, 0xFF, 0xFF});
    CHECK(pm::ReadTags(c, t, true));
}

namespace {

// Атом MP4: [размер][тип][данные]
std::vector<uint8_t> Atom(const std::string& type, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> a;
    PutBe32(a, static_cast<uint32_t>(body.size() + 8));
    PutStr(a, type);
    a.insert(a.end(), body.begin(), body.end());
    return a;
}

std::vector<uint8_t> Cat(std::initializer_list<std::vector<uint8_t>> parts) {
    std::vector<uint8_t> out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

// Элемент ilst со значением в атоме data
std::vector<uint8_t> Item(const std::string& type, uint32_t dataType, const std::vector<uint8_t>& value) {
    std::vector<uint8_t> d;
    PutBe32(d, dataType);
    PutBe32(d, 0);  // локаль
    d.insert(d.end(), value.begin(), value.end());
    return Atom(type, Atom("data", d));
}

std::string Base64(const std::vector<uint8_t>& b) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < b.size(); i += 3) {
        const uint32_t v = (uint32_t(b[i]) << 16) | (i + 1 < b.size() ? uint32_t(b[i + 1]) << 8 : 0) |
                           (i + 2 < b.size() ? b[i + 2] : 0);
        out += k[(v >> 18) & 63];
        out += k[(v >> 12) & 63];
        out += i + 1 < b.size() ? k[(v >> 6) & 63] : '=';
        out += i + 2 < b.size() ? k[v & 63] : '=';
    }
    return out;
}

// Страницы Ogg для одного пакета (сегменты по 255 байт, пакет может занять несколько страниц)
void OggPackets(std::vector<uint8_t>& f, const std::vector<std::vector<uint8_t>>& packets, uint64_t granule,
                uint32_t serial, uint32_t& seq) {
    std::vector<uint8_t> lacing, body;
    auto flush = [&] {
        PutStr(f, "OggS");
        f.push_back(0);
        f.push_back(0);
        tests::PutLe32(f, static_cast<uint32_t>(granule));
        tests::PutLe32(f, static_cast<uint32_t>(granule >> 32));
        tests::PutLe32(f, serial);
        tests::PutLe32(f, seq++);
        tests::PutLe32(f, 0);  // CRC не проверяется
        f.push_back(static_cast<uint8_t>(lacing.size()));
        f.insert(f.end(), lacing.begin(), lacing.end());
        f.insert(f.end(), body.begin(), body.end());
        lacing.clear();
        body.clear();
    };
    for (const auto& p : packets) {
        size_t at = 0;
        for (;;) {
            const size_t n = std::min<size_t>(p.size() - at, 255);
            lacing.push_back(static_cast<uint8_t>(n));
            body.insert(body.end(), p.begin() + static_cast<long>(at), p.begin() + static_cast<long>(at + n));
            at += n;
            if (lacing.size() == 255) flush();
            if (n < 255) break;
        }
    }
    if (!lacing.empty()) flush();
}

std::vector<uint8_t> VorbisComment(const std::vector<std::string>& comments) {
    std::vector<uint8_t> vc;
    tests::PutLe32(vc, 3);
    PutStr(vc, "pm!");
    tests::PutLe32(vc, static_cast<uint32_t>(comments.size()));
    for (const auto& c : comments) {
        tests::PutLe32(vc, static_cast<uint32_t>(c.size()));
        PutStr(vc, c);
    }
    return vc;
}

}  // namespace

TEST(Mp4TagsCoverAndDuration) {
    std::vector<uint8_t> mvhd(100, 0);  // версия 0: масштаб 1000, длительность 215.5 с
    mvhd[12] = 0, mvhd[13] = 0, mvhd[14] = 0x03, mvhd[15] = 0xE8;
    const uint32_t dur = 215500;
    for (int i = 0; i < 4; ++i) mvhd[16 + i] = static_cast<uint8_t>(dur >> (24 - 8 * i));
    std::vector<uint8_t> mean{0, 0, 0, 0}, name{0, 0, 0, 0};
    PutStr(mean, "com.apple.iTunes");
    PutStr(name, "replaygain_track_gain");
    std::vector<uint8_t> rgData{0, 0, 0, 1, 0, 0, 0, 0};
    PutStr(rgData, "-4.25 dB");
    const auto ilst = Atom("ilst", Cat({Item("\xA9nam", 1, Utf8("Синий час")), Item("\xA9" "ART", 1, Utf8("Кварц")),
                                        Item("\xA9" "alb", 1, Utf8("Окна")), Item("trkn", 0, {0, 0, 0, 5, 0, 12, 0, 0}),
                                        Item("covr", 14, kPng),
                                        Atom("----", Cat({Atom("mean", mean), Atom("name", name),
                                                          Atom("data", rgData)}))}));
    std::vector<uint8_t> metaBody{0, 0, 0, 0};  // версия и флаги
    const auto hdlr = Atom("hdlr", std::vector<uint8_t>(25, 0));
    metaBody.insert(metaBody.end(), hdlr.begin(), hdlr.end());
    metaBody.insert(metaBody.end(), ilst.begin(), ilst.end());
    const auto moov = Atom("moov", Cat({Atom("mvhd", mvhd), Atom("udta", Atom("meta", metaBody))}));
    std::vector<uint8_t> ftyp;
    PutStr(ftyp, "M4A ");
    PutBe32(ftyp, 0);
    // moov после mdat, как у файлов без «быстрого старта»
    const auto file = Cat({Atom("ftyp", ftyp), Atom("mdat", std::vector<uint8_t>(3000, 0x55)), moov});
    MemSource src(file);
    pm::TagInfo t;
    CHECK(pm::ReadTags(src, t, true));
    CHECK(t.title == "Синий час" && t.artist == "Кварц" && t.album == "Окна");
    CHECK(t.track == 5);
    CHECK_NEAR(t.duration, 215.5, 1e-9);
    CHECK(t.cover == kPng && t.coverMime == "image/png");
    CHECK(t.hasReplayGain && std::abs(t.replayGainDb + 4.25f) < 1e-4f);
    // Без обложки - те же теги
    pm::TagInfo quick;
    CHECK(pm::ReadTags(src, quick, false) && quick.cover.empty() && quick.title == "Синий час");
    // Обрезанный файл не роняет разбор
    for (size_t cut : {size_t(8), size_t(20), file.size() - 40, file.size() - 1}) {
        MemSource part(std::vector<uint8_t>(file.begin(), file.begin() + static_cast<long>(cut)));
        pm::TagInfo x;
        CHECK(pm::ReadTags(part, x, true));
    }
}

TEST(OggOpusTagsAndDuration) {
    std::vector<uint8_t> head;
    PutStr(head, "OpusHead");
    head.push_back(1);  // версия
    head.push_back(2);  // каналы
    tests::PutLe16(head, 312);  // preSkip
    tests::PutLe32(head, 44100);
    tests::PutLe16(head, 0);
    head.push_back(0);
    // Обложка METADATA_BLOCK_PICTURE (base64 блока PICTURE): комментарий больше страницы
    std::vector<uint8_t> pic;
    PutBe32(pic, 3);
    PutBe32(pic, 9);
    PutStr(pic, "image/png");
    PutBe32(pic, 0);
    for (int i = 0; i < 4; ++i) PutBe32(pic, 0);
    std::vector<uint8_t> big = kPng;
    big.resize(70000, 0xAB);
    PutBe32(pic, static_cast<uint32_t>(big.size()));
    pic.insert(pic.end(), big.begin(), big.end());
    std::vector<uint8_t> tags;
    PutStr(tags, "OpusTags");
    const auto vc = VorbisComment({"TITLE=Тихая гавань", "ARTIST=Море", "album=Берег", "TRACKNUMBER=3",
                                   "METADATA_BLOCK_PICTURE=" + Base64(pic)});
    tags.insert(tags.end(), vc.begin(), vc.end());
    std::vector<uint8_t> f;
    uint32_t seq = 0;
    OggPackets(f, {head}, 0, 77, seq);
    OggPackets(f, {tags}, 0, 77, seq);
    // Аудио: 10 с при 48 кГц плюс preSkip
    OggPackets(f, {std::vector<uint8_t>(100, 1)}, 240000, 77, seq);
    OggPackets(f, {std::vector<uint8_t>(100, 1)}, 480000 + 312, 77, seq);
    MemSource src(f);
    pm::TagInfo t;
    CHECK(pm::ReadTags(src, t, true));
    CHECK(t.title == "Тихая гавань" && t.artist == "Море" && t.album == "Берег" && t.track == 3);
    CHECK_NEAR(t.duration, 10.0, 1e-9);
    CHECK(t.cover == big && t.coverMime == "image/png");
    pm::TagInfo quick;
    CHECK(pm::ReadTags(src, quick, false) && quick.cover.empty() && quick.title == "Тихая гавань");
}

TEST(OggVorbisTagsAndDuration) {
    std::vector<uint8_t> id;
    id.push_back(1);
    PutStr(id, "vorbis");
    tests::PutLe32(id, 0);
    id.push_back(2);
    tests::PutLe32(id, 44100);
    id.resize(30, 0);
    std::vector<uint8_t> tags{3};
    PutStr(tags, "vorbis");
    const auto vc = VorbisComment({"title=Снег", "ARTIST=Зима", "REPLAYGAIN_TRACK_GAIN=-2 dB"});
    tags.insert(tags.end(), vc.begin(), vc.end());
    tags.push_back(1);  // бит кадрирования
    std::vector<uint8_t> f;
    uint32_t seq = 0;
    OggPackets(f, {id}, 0, 5, seq);
    // Комментарий и setup на одной странице
    OggPackets(f, {tags, std::vector<uint8_t>(40, 5)}, 0, 5, seq);
    OggPackets(f, {std::vector<uint8_t>(300, 1)}, 44100 * 3, 5, seq);
    MemSource src(f);
    pm::TagInfo t;
    CHECK(pm::ReadTags(src, t, true));
    CHECK(t.title == "Снег" && t.artist == "Зима" && t.hasReplayGain);
    CHECK_NEAR(t.duration, 3.0, 1e-9);
    // Обрезанные страницы не роняют разбор
    for (size_t cut : {size_t(4), size_t(30), size_t(60), f.size() - 10}) {
        MemSource part(std::vector<uint8_t>(f.begin(), f.begin() + static_cast<long>(cut)));
        pm::TagInfo x;
        CHECK(pm::ReadTags(part, x, true));
    }
}

TEST(SupportedExtensions) {
    CHECK(pm::IsSupportedAudio("a/b.MP3") && pm::IsSupportedAudio("x.flac") && pm::IsSupportedAudio("x.wav"));
    CHECK(!pm::IsSupportedAudio("x.txt") && !pm::IsSupportedAudio("cover.jpg"));
#ifdef _WIN32
    CHECK(pm::IsSupportedAudio("x.M4A") && pm::IsSupportedAudio("x.opus") && pm::IsSupportedAudio("x.ogg"));
#else
    CHECK(!pm::IsSupportedAudio("x.m4a"));
#endif
}
