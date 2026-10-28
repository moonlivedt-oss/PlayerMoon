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

#include "core/lock.h"
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

// Страница плагина (вкладка в панели), версия API 2
struct PluginAction {
    int id = 0;
    std::string label;
};

struct PluginPageRow {
    int id = 0;
    std::string title, subtitle, status;
    float progress = -1;
    int state = PM_ROW_NORMAL;
    std::vector<PluginAction> actions;
};

struct PluginPage {
    size_t plugin = 0;  // индекс в Plugins()
    int id = 0;         // номер страницы у плагина
    std::string title, emptyText;
    std::vector<PluginAction> actions;  // кнопки шапки
    std::vector<PluginPageRow> rows;    // новые сверху
};

// Что плеер умеет для плагинов (заполняет окно). Пустые функции - возможность недоступна.
struct PluginHostFunctions {
    std::function<PmPlayback()> playback;
    std::function<void(int command)> command;
    std::function<void(double seconds)> seek;
    // Версия 2
    std::function<void()> wake;  // из любого потока: в очереди post есть работа - вызвать RunPosted()
    std::function<void(size_t pageIndex)> pageShow;
    std::function<void(const std::string&)> toast;
    std::function<std::string()> clipboardText;
    std::function<void(const std::string&)> openPath;
    std::function<void(const std::vector<std::string>&, bool play)> addFiles;
    std::function<void(const std::string&)> addLibraryFolder;
    std::function<std::string(const std::string& title)> pickFolder;
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

    // Страницы плагинов. Revision растёт при каждом изменении - окну пора перечитать.
    const std::vector<PluginPage>& Pages() const { return pages_; }
    uint64_t PagesRevision() const { return pagesRevision_; }
    void PageAction(size_t pageIndex, int rowId, int actionId);
    // Поток UI: выполнить то, что плагины передали через host->post
    void RunPosted();

    void UnloadAll();

    // Внутреннее: состояние загруженного плагина (нужно функциям PmHost)
    struct Loaded;
    // Для функций PmHost (внутреннее)
    PluginPage* FindPage(size_t plugin, int pageId);
    std::vector<PluginPage>& MutablePages() { return pages_; }
    void Touch() { ++pagesRevision_; }
    void Post(size_t plugin, void (*fn)(void*), void* data);
    const PluginHostFunctions& Host() const { return host_; }

private:
    bool Load(size_t index);
    void Unload(size_t index);
    void Log(const std::string& line);

    PluginHostFunctions host_;
    std::filesystem::path dataRoot_;
    std::vector<PluginInfo> plugins_;
    std::vector<std::unique_ptr<Loaded>> loaded_;  // параллельно plugins_
    std::vector<PluginMenuItem> menu_;
    std::vector<PluginPage> pages_;
    uint64_t pagesRevision_ = 0;

    // Очередь host->post: пишут потоки плагинов, читает поток UI
    struct Posted {
        size_t plugin;
        void (*fn)(void*);
        void* data;
    };
    Lock postLock_;
    std::vector<Posted> posted_;
};

}  // namespace pm
