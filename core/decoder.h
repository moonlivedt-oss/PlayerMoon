#pragma once
// Декодер: файл -> float PCM в формате движка (чередующиеся каналы).
// Сейчас одна реализация на miniaudio (MP3, FLAC, WAV). Чтобы позже добавить
// M4A/AAC/OGG через Media Foundation, достаточно новой реализации IDecoder
// и строки в OpenDecoder(); движок и плеер от вида декодера не зависят.
//
// Владение: декодер создаёт поток UI, потом передаёт его аудиопотоку
// (см. Engine::SetTrack). Одновременно декодером пользуется только один поток.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace pm {

class IDecoder {
public:
    virtual ~IDecoder() = default;
    // Читает до frames кадров в out (frames * channels чисел). Возвращает прочитанное; меньше - конец.
    virtual uint64_t Read(float* out, uint64_t frames) = 0;
    virtual bool Seek(uint64_t frame) = 0;
    // Длина в кадрах формата движка; 0 - неизвестна
    virtual uint64_t LengthFrames() const = 0;
};

// Открыть файл с выводом в channels каналов и частоту sampleRate (декодер сам пересчитывает частоту)
std::unique_ptr<IDecoder> OpenDecoder(const std::filesystem::path& path, uint32_t channels, uint32_t sampleRate,
                                      std::string* error = nullptr);

}  // namespace pm
