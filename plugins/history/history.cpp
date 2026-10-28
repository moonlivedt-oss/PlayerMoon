// Пример плагина PlayerMoon: журнал прослушиваний.
// Каждый начатый трек дописывается строкой в history.txt в папке данных плагина:
//   2026-10-28 21:04 | Исполнитель - Название
// Пункт меню «Открыть журнал прослушиваний» открывает файл в программе по умолчанию.

#include <cstdio>
#include <ctime>
#include <string>

#include "plugins/sdk/playermoon_plugin.h"

#ifdef _WIN32
#include <windows.h>

#include <shellapi.h>
#endif

namespace {

constexpr int kMenuOpen = 1;

struct History {
    const PmHost* host = nullptr;
    std::string file;  // UTF-8
};

#ifdef _WIN32
std::wstring Wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
#endif

FILE* OpenAppend(const std::string& path) {
#ifdef _WIN32
    return _wfopen(Wide(path).c_str(), L"ab");
#else
    return std::fopen(path.c_str(), "ab");
#endif
}

void OnTrackChanged(void* self, const PmTrack* track) {
    auto* h = static_cast<History*>(self);
    if (!track) return;
    FILE* f = OpenAppend(h->file);
    if (!f) {
        h->host->log(h->host->ctx, "history: не удалось открыть history.txt");
        return;
    }
    char when[32];
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &local);
    const std::string artist = track->artist[0] ? std::string(track->artist) + " - " : std::string();
    std::fprintf(f, "%s | %s%s\n", when, artist.c_str(), track->title);
    std::fclose(f);
}

void OnMenuItem(void* self, int id) {
    auto* h = static_cast<History*>(self);
    if (id != kMenuOpen) return;
#ifdef _WIN32
    ShellExecuteW(nullptr, L"open", Wide(h->file).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    (void)h;
#endif
}

void Shutdown(void* self) { delete static_cast<History*>(self); }

}  // namespace

extern "C" PM_PLUGIN_EXPORT int PlayerMoonPluginInit(const PmHost* host, PmPlugin* plugin) {
    if (!host || host->apiVersion < 1) return 1;
    auto* h = new History;
    h->host = host;
    h->file = std::string(host->dataDir(host->ctx)) + "/history.txt";
    plugin->self = h;
    plugin->onTrackChanged = &OnTrackChanged;
    plugin->onMenuItem = &OnMenuItem;
    plugin->shutdown = &Shutdown;
    host->addMenuItem(host->ctx, kMenuOpen, "Открыть журнал прослушиваний");
    return 0;
}
