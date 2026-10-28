#pragma once
// Генерация тестовых файлов: WAV с синусом и сборка байтов чанков/кадров.
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace tests {

inline void PutLe32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
inline void PutLe16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v));
    b.push_back(static_cast<uint8_t>(v >> 8));
}
inline void PutBe32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 3; i >= 0; --i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
inline void PutStr(std::vector<uint8_t>& b, const std::string& s) { b.insert(b.end(), s.begin(), s.end()); }

// WAV 16 бит стерео с синусом; extraChunk (если не пуст) добавляется после data
inline std::vector<uint8_t> MakeWav(double seconds, uint32_t rate = 44100, double hz = 440,
                                   const std::vector<uint8_t>& extraChunk = {}) {
    const uint32_t frames = static_cast<uint32_t>(seconds * rate);
    std::vector<uint8_t> data;
    for (uint32_t i = 0; i < frames; ++i) {
        const auto s = static_cast<int16_t>(std::sin(2 * 3.14159265358979 * hz * i / rate) * 12000);
        PutLe16(data, static_cast<uint16_t>(s));
        PutLe16(data, static_cast<uint16_t>(s));
    }
    std::vector<uint8_t> b;
    PutStr(b, "RIFF");
    PutLe32(b, static_cast<uint32_t>(4 + 24 + 8 + data.size() + extraChunk.size()));
    PutStr(b, "WAVE");
    PutStr(b, "fmt ");
    PutLe32(b, 16);
    PutLe16(b, 1);  // PCM
    PutLe16(b, 2);
    PutLe32(b, rate);
    PutLe32(b, rate * 4);
    PutLe16(b, 4);
    PutLe16(b, 16);
    PutStr(b, "data");
    PutLe32(b, static_cast<uint32_t>(data.size()));
    b.insert(b.end(), data.begin(), data.end());
    b.insert(b.end(), extraChunk.begin(), extraChunk.end());
    return b;
}

inline bool WriteBytes(const std::filesystem::path& p, const std::vector<uint8_t>& b) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return static_cast<bool>(f);
}

inline std::filesystem::path TempDir(const std::string& name) {
    auto d = std::filesystem::temp_directory_path() / ("pm_tests_" + name);
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    return d;
}

}  // namespace tests
