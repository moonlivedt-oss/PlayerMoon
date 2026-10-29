#pragma once
// Теги и обложки: ID3v1, ID3v2.2/2.3/2.4 (TIT2/TPE1/TALB/APIC), FLAC (VORBIS_COMMENT,
// PICTURE, длительность из STREAMINFO), WAV (LIST/INFO, чанк id3) и оценка длительности MP3
// (заголовок Xing/Info/VBRI или постоянный битрейт), MP4/M4A (ilst: ©nam, ©ART, ©alb, trkn, covr,
// ReplayGain в «----», длительность из mvhd), Ogg Vorbis и Opus (Vorbis Comment, METADATA_BLOCK_PICTURE,
// длительность по последней странице). Без сторонних библиотек.
// Читает только нужные куски файла, поэтому годится для сканирования папок.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pm {

struct TagInfo {
    std::string title;
    std::string artist;
    std::string album;
    double duration = 0;  // секунды, 0 - неизвестна
    int track = 0;        // номер трека в альбоме (TRCK, TRACKNUMBER), 0 - нет
    // Выравнивание громкости ReplayGain (TXXX в ID3v2, REPLAYGAIN_* в Vorbis Comment)
    bool hasReplayGain = false;
    float replayGainDb = 0;  // REPLAYGAIN_TRACK_GAIN
    float replayPeak = 0;    // REPLAYGAIN_TRACK_PEAK, 0 - нет
    std::vector<uint8_t> cover;
    std::string coverMime;  // image/jpeg, image/png
};

// Источник байт: файл или память (для тестов)
class ByteSource {
public:
    virtual ~ByteSource() = default;
    virtual uint64_t Size() const = 0;
    // Читает до n байт со смещения off, возвращает сколько прочитано
    virtual size_t ReadAt(uint64_t off, void* dst, size_t n) = 0;
};

// wantCover = false - обложка не читается (быстрое сканирование плейлиста)
bool ReadTags(const std::filesystem::path& path, TagInfo& out, bool wantCover);
bool ReadTags(ByteSource& src, TagInfo& out, bool wantCover);

// Множитель громкости по ReplayGain: 10^(дБ/20), без перегрузки по пику, в пределах [0.1, 4]
float ReplayGainFactor(const TagInfo& t);

// Обложка из папки трека, если в тегах её нет: cover, folder, front, album (.jpg, .jpeg, .png),
// без учёта регистра. false - картинки нет.
bool ReadFolderCover(const std::filesystem::path& audio, std::vector<uint8_t>& out, std::string& mime);

// Поддерживаемые расширения без учёта регистра. M4A/M4B/AAC/OGG/OGA/OPUS - только в Windows
// (декодер Media Foundation), теги их читаются везде.
bool IsSupportedAudio(const std::filesystem::path& path);

}  // namespace pm
