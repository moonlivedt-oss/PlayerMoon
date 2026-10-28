#pragma once
// Теги и обложки: ID3v1, ID3v2.2/2.3/2.4 (TIT2/TPE1/TALB/APIC), FLAC (VORBIS_COMMENT,
// PICTURE, длительность из STREAMINFO), WAV (LIST/INFO, чанк id3) и оценка длительности MP3
// (заголовок Xing/Info/VBRI или постоянный битрейт). Без сторонних библиотек.
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

// Поддерживаемые сейчас расширения (нижний регистр, с точкой)
bool IsSupportedAudio(const std::filesystem::path& path);

}  // namespace pm
