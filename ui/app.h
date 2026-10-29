#pragma once
// Приложение: окно со своей рамкой, цикл кадров, ввод, меню, медиаклавиши, сохранение.
// Связывает плеер (pm::Player), темы (theme::ThemeLibrary), логику интерфейса
// (view::PlayerView), отрисовку (Painter), библиотеку музыки и плагины.
// Всё живёт в потоке UI; аудиопоток, сканер библиотеки и наблюдатель за папками только
// будят окно сообщениями (kWakeMessage, kLibraryMessage, kWatchMessage).

#include <windows.h>

#include <shellapi.h>

#include <atomic>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "core/fft.h"
#include "core/library.h"
#include "core/player.h"
#include "core/plugins.h"
#include "theme/theme.h"
#include "ui/covers.h"
#include "ui/folder_watch.h"
#include "ui/icons.h"
#include "ui/painter.h"
#include "ui/renderer.h"
#include "ui/smtc.h"
#include "ui/taskbar.h"
#include "ui/text.h"
#include "view/player_view.h"

namespace ui {

class App final : public view::IViewHost {
public:
    int Run(HINSTANCE instance, int show);

    // ----- view::IViewHost -----
    void TogglePlay() override;
    void Next() override;
    void Prev() override;
    void Seek(double seconds) override;
    void SetVolume(float volume) override;
    void ToggleMute() override;
    void CycleRepeat() override;
    void ToggleShuffle() override;
    void PlayIndex(int index) override;
    void ResizeWindow(view::Size dip) override;
    void MinimizeWindow() override;
    void CloseWindow() override;
    void ShowMenu(view::Point at) override;
    void ShowRowMenu(int row, view::Point at) override;
    void TogglePlugin(int index) override;
    void OpenPluginsFolder() override;
    void PageAction(int page, int rowId, int actionId) override;
    void OpenFiles(bool folder) override;
    float MeasureText(view::TextStyle style, const std::string& text) override;

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    // Уже запущенный экземпляр: передать ему файлы из командной строки. true - он есть, выходим.
    static bool ForwardToRunningInstance();
    void HandleCopyData(const COPYDATASTRUCT* cds);

    bool CreateMainWindow(HINSTANCE instance);
    void ResolvePaths();
    void LoadSavedState();
    void SaveCurrentState();
    // Что-то поменялось: сохранить через 2 секунды (несколько изменений подряд - одна запись)
    void MarkDirty();
    void MessageLoop();
    void RenderFrame();
    double Now() const;
    view::Point ToDip(LPARAM lp) const;
    view::Size ClientDip() const;
    int ToPx(float dip) const;

    void ApplyTheme(const theme::Theme& t, bool animate);
    void ApplyBackdrop();
    void ApplyTopmost();
    void SnapToEdges(RECT* r) const;
    void ReadSystemReduce();
    static bool SystemPrefersLight();
    void OnTrackChanged(int index, int direction);
    void RebuildRows();
    void ShowContextMenu(POINT screen);
    void ImportTheme();
    void ExportTheme();
    void LoadDropped(HDROP drop);
    // append - добавить в конец; иначе заменить плейлист и начать с первого трека
    void LoadPaths(const std::vector<std::filesystem::path>& paths, bool append = false);
    void RegisterMediaKeys();
    void HandleMediaCommand(int command);
    void Invalidate() { dirty_ = true; }

    // ----- библиотека -----
    void InitLibrary(bool firstRun);
    void StartLibraryScan();
    void OnLibraryScanned();
    void RestartWatcher();
    void LoadLibraryPlaylist(bool play);
    void AddLibraryFolder();
    // Запомнить папки, из которых пользователь что-то открыл
    void LearnFolders(const std::vector<std::filesystem::path>& paths);

    // ----- плагины -----
    void InitPlugins(const std::set<std::string>& enabled);
    std::filesystem::path UserPluginsDir() const;
    void RebuildPluginRows();
    void NotifyPluginsTrack();
    // Страницы плагинов -> view (если изменились)
    void SyncPages();
    bool InPlaylist(const std::filesystem::path& p) const;
    // host->addFiles: дописать без повторов, play - сразу играть первый
    void AddFilesFromPlugin(const std::vector<std::string>& paths, bool play);

    HWND hwnd_ = nullptr;
    float dpi_ = 96;
    LARGE_INTEGER freq_{};
    UINT taskbarCreatedMsg_ = 0;

    pm::Player player_;
    theme::ThemeLibrary themes_;
    theme::Theme theme_;
    bool userTheme_ = false;  // своя тема (из импорта) - сохраняется документом целиком
    view::PlayerView view_;

    Renderer renderer_;
    TextSystem text_;
    IconSet icons_;
    CoverCache covers_;
    Painter painter_;
    Taskbar taskbar_;
    Smtc smtc_;

    pm::SpectrumAnalyzer analyzer_{2048};
    std::vector<float> pcm_, spectrum_;
    std::vector<PlaylistRow> rows_;
    std::wstring countText_;

    pm::Library library_;
    pm::LibraryScanner scanner_;
    FolderWatcher watcher_;
    bool rescanPending_ = false;
    bool playlistFromLibrary_ = false;

    pm::PluginManager plugins_;
    std::vector<PluginRow> pluginRows_;
    std::wstring pluginCountText_;
    bool pluginsRefreshPending_ = false;
    uint64_t pagesRevision_ = ~0ull;  // открыта папка плагинов - перечитать при возврате в окно

    std::filesystem::path exeDir_, assetsDir_, statePath_, libraryPath_;
    bool systemReduce_ = false;
    bool win11Backdrop_ = false;  // сборка Windows умеет системный акрил (22621+)
    bool acrylicOn_ = false;      // акрил сейчас включён
    bool miniOnTop_ = true;
    bool topmostApplied_ = false;
    bool visible_ = true;
    bool dirty_ = true;
    bool started_ = false;  // после запуска смена трека анимируется
    bool trackingMouse_ = false;
    bool tickTimer_ = false;
    bool saveTick_ = false;
    int lastPlaying_ = -1;      // для события «пуск/пауза» плагинам
    double lastCopyData_ = -10;  // время прошлой передачи файлов от второго экземпляра
    std::atomic<bool> wakePosted_{false};
};

}  // namespace ui
