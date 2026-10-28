#include "core/plugins.h"

#include <algorithm>
#include <fstream>

#include "core/json.h"
#include "core/text.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace pm {

namespace {

constexpr int kMaxMenuId = 999;

#ifdef _WIN32
using LibHandle = HMODULE;
LibHandle OpenLib(const std::filesystem::path& p, std::string& err) {
    // Зависимости DLL ищутся в её собственной папке
    HMODULE h = LoadLibraryExW(p.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) err = "не загружается DLL (код " + std::to_string(GetLastError()) + ")";
    return h;
}
void* FindSym(LibHandle h, const char* name) { return reinterpret_cast<void*>(GetProcAddress(h, name)); }
void CloseLib(LibHandle h) { FreeLibrary(h); }
const char* kLibExt = ".dll";
#else
using LibHandle = void*;
LibHandle OpenLib(const std::filesystem::path& p, std::string& err) {
    void* h = dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) err = dlerror();
    return h;
}
void* FindSym(LibHandle h, const char* name) { return dlsym(h, name); }
void CloseLib(LibHandle h) { dlclose(h); }
const char* kLibExt = ".so";
#endif

bool ValidId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

}  // namespace

// Загруженный плагин: библиотека, таблицы для обмена и папка данных
struct PluginManager::Loaded {
    PluginManager* owner = nullptr;
    size_t index = 0;
    LibHandle lib{};
    PmHost host{};
    PmPlugin plugin{};
    std::string dataDir;  // UTF-8, живёт пока плагин загружен
};

namespace {

PluginManager::Loaded* Self(void* ctx) { return static_cast<PluginManager::Loaded*>(ctx); }

}  // namespace

PluginManager::PluginManager() = default;
PluginManager::~PluginManager() { UnloadAll(); }

void PluginManager::Log(const std::string& line) {
    if (dataRoot_.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(dataRoot_, ec);
    std::ofstream f(dataRoot_ / "plugins.log", std::ios::app | std::ios::binary);
    f << line << "\n";
}

void PluginManager::Discover(const std::vector<std::filesystem::path>& roots) {
    const std::set<std::string> enabled = EnabledIds();
    UnloadAll();
    plugins_.clear();
    loaded_.clear();
    for (const auto& root : roots) {
        std::error_code ec;
        std::vector<std::filesystem::path> dirs;
        for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_directory(ec)) dirs.push_back(it->path());
        std::sort(dirs.begin(), dirs.end());
        for (const auto& dir : dirs) {
            std::ifstream f(dir / "plugin.json", std::ios::binary);
            if (!f) continue;
            const std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            json::Value v;
            std::string err;
            PluginInfo p;
            p.dir = dir;
            if (!json::Parse(data, v, &err) || !v.IsObject()) {
                p.id = text::PathToUtf8(dir.filename());
                p.name = p.id;
                p.error = "plugin.json: " + (err.empty() ? std::string("не объект") : err);
            } else {
                p.id = v.Get("id").AsString();
                p.name = v.Get("name").AsString();
                p.version = v.Get("version").AsString();
                p.description = v.Get("description").AsString();
                p.author = v.Get("author").AsString();
                const std::string lib = v.Get("library").AsString();
                const int api = static_cast<int>(v.Get("apiVersion").AsNumber(1));
                if (!ValidId(p.id)) p.error = "неверный id";
                else if (lib.empty() || lib.find_first_of("/\\") != std::string::npos) p.error = "неверное поле library";
                else if (api > PM_PLUGIN_API_VERSION) p.error = "нужна более новая версия PlayerMoon";
                else p.library = dir / text::Utf8ToPath(lib + kLibExt);
                if (p.name.empty()) p.name = p.id;
            }
            const bool duplicate = std::any_of(plugins_.begin(), plugins_.end(),
                                               [&](const PluginInfo& q) { return q.id == p.id; });
            if (duplicate) continue;
            plugins_.push_back(std::move(p));
            loaded_.push_back(nullptr);
        }
    }
    EnableSaved(enabled);
}

bool PluginManager::Load(size_t i) {
    PluginInfo& p = plugins_[i];
    if (loaded_[i]) return true;
    if (!p.error.empty() && p.library.empty()) return false;
    p.error.clear();
    auto l = std::make_unique<Loaded>();
    l->owner = this;
    l->index = i;
    std::string err;
    l->lib = OpenLib(p.library, err);
    if (!l->lib) {
        p.error = err;
        Log(p.id + ": " + err);
        return false;
    }
    auto init = reinterpret_cast<PmPluginInit>(FindSym(l->lib, PM_PLUGIN_INIT_NAME));
    if (!init) {
        CloseLib(l->lib);
        p.error = "нет функции " PM_PLUGIN_INIT_NAME;
        Log(p.id + ": " + p.error);
        return false;
    }
    std::error_code ec;
    const std::filesystem::path data = dataRoot_ / text::Utf8ToPath(p.id);
    std::filesystem::create_directories(data, ec);
    l->dataDir = text::PathToUtf8(data);

    PmHost& h = l->host;
    h.apiVersion = PM_PLUGIN_API_VERSION;
    h.ctx = l.get();
    h.log = [](void* ctx, const char* msg) {
        Loaded* s = Self(ctx);
        s->owner->Log(s->owner->plugins_[s->index].id + ": " + (msg ? msg : ""));
    };
    h.dataDir = [](void* ctx) { return Self(ctx)->dataDir.c_str(); };
    h.getPlayback = [](void* ctx, PmPlayback* out) {
        if (!out) return;
        const auto& fn = Self(ctx)->owner->host_.playback;
        *out = fn ? fn() : PmPlayback{0, -1, 0, 0, 0, 0};
    };
    h.command = [](void* ctx, int cmd) {
        if (const auto& fn = Self(ctx)->owner->host_.command) fn(cmd);
    };
    h.seek = [](void* ctx, double s) {
        if (const auto& fn = Self(ctx)->owner->host_.seek) fn(s);
    };
    h.addMenuItem = [](void* ctx, int id, const char* title) -> int {
        Loaded* s = Self(ctx);
        if (id < 0 || id > kMaxMenuId || !title || !*title) return 0;
        auto& menu = s->owner->menu_;
        for (const auto& m : menu)
            if (m.plugin == s->index && m.id == id) return 0;
        menu.push_back({s->index, id, title});
        return 1;
    };

    // Точку входа вызываем после того, как плагин занял место: addMenuItem знает его индекс
    Loaded* raw = l.get();
    loaded_[i] = std::move(l);
    if (init(&raw->host, &raw->plugin) != 0) {
        menu_.erase(std::remove_if(menu_.begin(), menu_.end(), [&](const PluginMenuItem& m) { return m.plugin == i; }),
                    menu_.end());
        CloseLib(raw->lib);
        loaded_[i].reset();
        p.error = "плагин отказался запускаться";
        Log(p.id + ": " + p.error);
        return false;
    }
    p.loaded = true;
    Log(p.id + ": загружен");
    return true;
}

void PluginManager::Unload(size_t i) {
    if (i >= loaded_.size() || !loaded_[i]) return;
    Loaded& l = *loaded_[i];
    if (l.plugin.shutdown) l.plugin.shutdown(l.plugin.self);
    CloseLib(l.lib);
    loaded_[i].reset();
    plugins_[i].loaded = false;
    menu_.erase(std::remove_if(menu_.begin(), menu_.end(), [&](const PluginMenuItem& m) { return m.plugin == i; }),
                menu_.end());
}

bool PluginManager::SetEnabled(size_t i, bool on) {
    if (i >= plugins_.size()) return false;
    plugins_[i].enabled = on;
    if (!on) {
        Unload(i);
        return true;
    }
    if (Load(i)) return true;
    plugins_[i].enabled = false;
    return false;
}

void PluginManager::EnableSaved(const std::set<std::string>& ids) {
    for (size_t i = 0; i < plugins_.size(); ++i)
        if (ids.count(plugins_[i].id)) SetEnabled(i, true);
}

std::set<std::string> PluginManager::EnabledIds() const {
    std::set<std::string> out;
    for (const auto& p : plugins_)
        if (p.enabled) out.insert(p.id);
    return out;
}

void PluginManager::NotifyTrack(const PmTrack* track) {
    for (auto& l : loaded_)
        if (l && l->plugin.onTrackChanged) l->plugin.onTrackChanged(l->plugin.self, track);
}

void PluginManager::NotifyPlayState(bool playing) {
    for (auto& l : loaded_)
        if (l && l->plugin.onPlayState) l->plugin.onPlayState(l->plugin.self, playing ? 1 : 0);
}

void PluginManager::InvokeMenuItem(size_t index) {
    if (index >= menu_.size()) return;
    const PluginMenuItem m = menu_[index];
    Loaded* l = loaded_[m.plugin].get();
    if (l && l->plugin.onMenuItem) l->plugin.onMenuItem(l->plugin.self, m.id);
}

void PluginManager::UnloadAll() {
    for (size_t i = 0; i < loaded_.size(); ++i) Unload(i);
    menu_.clear();
}

}  // namespace pm
