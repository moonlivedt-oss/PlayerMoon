#pragma once
// Приложение: окно со своей рамкой, цикл кадров, ввод, меню, медиаклавиши, сохранение.
// Связывает плеер (pm::Player), темы (theme::ThemeLibrary), логику интерфейса
// (view::PlayerView) и отрисовку (Painter). Всё живёт в потоке UI; аудиопоток только
// будит окно сообщением kWakeMessage.

#include <windows.h>

#include <shellapi.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

#include "core/fft.h"
#include "core/player.h"
#include "theme/theme.h"
#include "ui/covers.h"
#include "ui/icons.h"
#include "ui/painter.h"
#include "ui/renderer.h"
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
    float MeasureText(view::TextStyle style, const std::string& text) override;

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    bool CreateMainWindow(HINSTANCE instance);
    void ResolvePaths();
    void LoadSavedState();
    void SaveCurrentState();
    void MessageLoop();
    void RenderFrame();
    double Now() const;
    view::Point ToDip(LPARAM lp) const;
    view::Size ClientDip() const;
    int ToPx(float dip) const;

    void ApplyTheme(const theme::Theme& t, bool animate);
    void ApplyBackdrop();
    void ReadSystemReduce();
    void OnTrackChanged(int index, int direction);
    void RebuildRows();
    void ShowContextMenu(POINT screen);
    void OpenFiles(bool folder);
    void ImportTheme();
    void ExportTheme();
    void LoadDropped(HDROP drop);
    void LoadPaths(const std::vector<std::filesystem::path>& paths);
    void RegisterMediaKeys();
    void HandleMediaCommand(int command);
    void Invalidate() { dirty_ = true; }

    HWND hwnd_ = nullptr;
    float dpi_ = 96;
    LARGE_INTEGER freq_{};

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

    pm::SpectrumAnalyzer analyzer_{2048};
    std::vector<float> pcm_, spectrum_;
    std::vector<PlaylistRow> rows_;
    std::wstring countText_;

    std::filesystem::path exeDir_, assetsDir_, statePath_;
    bool systemReduce_ = false;
    bool win11Backdrop_ = false;  // сборка Windows умеет системный акрил (22621+)
    bool acrylicOn_ = false;      // акрил сейчас включён
    bool visible_ = true;
    bool dirty_ = true;
    bool started_ = false;  // после запуска смена трека анимируется
    bool trackingMouse_ = false;
    bool tickTimer_ = false;
    std::atomic<bool> wakePosted_{false};
};

}  // namespace ui
