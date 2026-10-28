// Поддельный yt-dlp для проверки плагина «Загрузка музыки» без сети (запуск под Wine).
// Понимает --version, --list-extractors, -U и загрузку: печатает строки PMITEM/PMPROG/PMFILE
// в том же формате, что настоящий yt-dlp с шаблонами из plugins/downloader/ytdlp.cpp, и
// «скачивает» короткий WAV. Ссылка со словом fail - ошибка, со словом list - два трека.

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

std::string Utf8(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

void Out(const std::string& s) {
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void WriteWav(const std::wstring& path, double hz) {
    const uint32_t rate = 44100, frames = rate * 3;
    std::vector<int16_t> pcm(frames * 2);
    for (uint32_t i = 0; i < frames; ++i) pcm[i * 2] = pcm[i * 2 + 1] = static_cast<int16_t>(6000 * std::sin(6.2831853 * hz * i / rate));
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    const uint32_t data = frames * 4, riff = 36 + data, fmt = 16, byteRate = rate * 4;
    const uint16_t pcmFmt = 1, ch = 2, align = 4, bits = 16;
    std::fwrite("RIFF", 1, 4, f);
    std::fwrite(&riff, 4, 1, f);
    std::fwrite("WAVEfmt ", 1, 8, f);
    std::fwrite(&fmt, 4, 1, f);
    std::fwrite(&pcmFmt, 2, 1, f);
    std::fwrite(&ch, 2, 1, f);
    std::fwrite(&rate, 4, 1, f);
    std::fwrite(&byteRate, 4, 1, f);
    std::fwrite(&align, 2, 1, f);
    std::fwrite(&bits, 2, 1, f);
    std::fwrite("data", 1, 4, f);
    std::fwrite(&data, 4, 1, f);
    std::fwrite(pcm.data(), 2, pcm.size(), f);
    std::fclose(f);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring outTemplate, url;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--version") {
            Out("2026.08.19");
            return 0;
        }
        if (a == L"--list-extractors") {
            Out("youtube");
            Out("youtube:tab");
            Out("YandexMusicTrack");
            return 0;
        }
        if (a == L"-U") {
            Out("Latest version: 2026.08.19, Current version: 2026.08.19");
            return 0;
        }
        if (a == L"-o" && i + 1 < argc) outTemplate = argv[++i];
        if (a == L"--" && i + 1 < argc) url = argv[++i];
    }
    const size_t cut = outTemplate.find(L"/%(");
    const std::wstring dir = cut == std::wstring::npos ? L"." : outTemplate.substr(0, cut);
    Out("[youtube] Extracting URL: " + Utf8(url));
    if (url.find(L"fail") != std::wstring::npos) {
        Out("ERROR: [youtube] fail: Video unavailable");
        return 1;
    }
    const int items = url.find(L"list") != std::wstring::npos ? 2 : 1;
    for (int n = 1; n <= items; ++n) {
        const std::string title = items > 1 ? "Трек из плейлиста " + std::to_string(n) : "Тестовый трек";
        Out("PMITEM|" + std::string(items > 1 ? std::to_string(n) : "NA") + "|" +
            std::string(items > 1 ? std::to_string(items) : "NA") + "|" + title + "|Тестовый артист");
        for (int p = 0; p <= 100; p += 5) {
            char line[128];
            std::snprintf(line, sizeof(line), "PMPROG|\x1b[0;94m%5.1f%%\x1b[0m|%.2fMiB/s|00:%02d", static_cast<double>(p),
                          1.5 + p / 100.0, (100 - p) / 20);
            Out(line);
            Sleep(120);
        }
        Out("[ExtractAudio] Destination: x.mp3");
        Sleep(300);
        const std::wstring file = dir + L"\\Тестовый артист - " +
                                  (items > 1 ? L"Трек из плейлиста " + std::to_wstring(n) : L"Тестовый трек") + L".wav";
        WriteWav(file, 330.0 + 110 * n);
        Out("PMFILE|" + Utf8(file));
    }
    return 0;
}
