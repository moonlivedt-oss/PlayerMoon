#pragma once
// Плагины: поиск в папках, загрузка DLL, события плеера, пункты меню.
// Интерфейс для авторов плагинов - plugins/sdk/playermoon_plugin.h (C, версия 1).
//
// Владение и потоки: менеджер и все вызовы плагинов - в потоке UI. Новые плагины выключены,
// пока пользователь их не включит; список включённых хранится в state.json.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "plugins/sdk/playermoon_plugin.h"

namespace pm {

struct PluginInfo {
    std::string id, name, version, description, author;
    std::filesystem::path dir;      // папка плагина
    std::filesystem::path library;  // путь к DLL (.dll на Windows, .so на Linux)
    bool enabled = false;
    bool loaded = false;
    std::string error;  // почему не загрузился (для показа в разделе «Плагины»)
};

struct PluginMenuItem {
    size_t plugin = 0;  // индекс в Plugins()
    int id = 0;         // номер пункта у плагина
    std::string title;
};

// Что плеер умеет для плагинов (заполняет окно)
struct PluginHostFunctions {
    std::function<PmPlayback()> playback;
    std::function<void(int command)> command;
    std::function<void(double seconds)> seek;
};

class PluginManager {
public:
    PluginManager();
    ~PluginManager();
    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    void SetHost(PluginHostFunctions host) { host_ = std::move(host); }
    // Где плагины хранят данные (<root>/<id>) и куда пишется plugins.log
    void SetDataRoot(const std::filesystem::path& root) { dataRoot_ = root; }

    // Ищет плагины в папках (каждый - подпапка с plugin.json). Уже загруженные выгружаются.
    // Плагин с тем же id из следующей папки пропускается.
    void Discover(const std::vector<std::filesystem::path>& roots);
    const std::vector<PluginInfo>& Plugins() const { return plugins_; }

    // Включить или выключить (с загрузкой или выгрузкой). false - не загрузился, причина в error.
    bool SetEnabled(size_t index, bool on);
    // При запуске: включить плагины из сохранённого списка
    void EnableSaved(const std::set<std::string>& ids);
    std::set<std::string> EnabledIds() const;

    // События плеера. track == nullptr - плейлист пуст.
    void NotifyTrack(const PmTrack* track);
    void NotifyPlayState(bool playing);

    const std::vector<PluginMenuItem>& MenuItems() const { return menu_; }
    void InvokeMenuItem(size_t index);

    void UnloadAll();

    // Внутреннее: состояние загруженного плагина (нужно функциям PmHost)
    struct Loaded;

private:
    bool Load(size_t index);
    void Unload(size_t index);
    void Log(const std::string& line);

    PluginHostFunctions host_;
    std::filesystem::path dataRoot_;
    std::vector<PluginInfo> plugins_;
    std::vector<std::unique_ptr<Loaded>> loaded_;  // параллельно plugins_
    std::vector<PluginMenuItem> menu_;
};

}  // namespace pm
