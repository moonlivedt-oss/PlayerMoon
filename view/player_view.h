#pragma once
// Состояние интерфейса плеера и его логика: наведение и нажатия, переходы режимов
// (плейлист, мини-режим, смена темы), смена трека с анимацией обложки и текста,
// перетекание цветов, бегущая строка, визуализатор.
//
// Переносимый код без Win32: окно передаёт сюда ввод в DIP и снимок состояния плеера,
// а потом рисует то, что лежит в публичных полях (см. ui/painter.cpp).
// Владелец - поток UI.

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/playlist.h"
#include "motion/tween.h"
#include "theme/palette.h"
#include "theme/theme.h"
#include "view/layout.h"
#include "view/marquee.h"
#include "view/motion_params.h"
#include "view/visualizer.h"
#include "view/widgets.h"

namespace view {

enum class TextStyle { Title, MiniTitle };

// Обратная связь с окном и плеером
class IViewHost {
public:
    virtual ~IViewHost() = default;
    virtual void TogglePlay() = 0;
    virtual void Next() = 0;
    virtual void Prev() = 0;
    virtual void Seek(double seconds) = 0;
    virtual void SetVolume(float volume) = 0;
    virtual void ToggleMute() = 0;
    virtual void CycleRepeat() = 0;
    virtual void ToggleShuffle() = 0;
    virtual void PlayIndex(int index) = 0;
    // Мгновенно поменять размер окна (DIP); левый верхний угол остаётся на месте
    virtual void ResizeWindow(Size dip) = 0;
    virtual void MinimizeWindow() = 0;
    virtual void CloseWindow() = 0;
    virtual void ShowMenu(Point at) = 0;
    // Ширина текста в DIP для бегущей строки
    virtual float MeasureText(TextStyle style, const std::string& text) = 0;
};

// Снимок состояния плеера на кадр
struct PlaybackSnapshot {
    bool playing = false;
    bool hasTrack = false;
    double position = 0;
    double duration = 0;
    float volume = 0.7f;
    bool muted = false;
    pm::Repeat repeat = pm::Repeat::Off;
    bool shuffle = false;
    int index = -1;
    int count = 0;
};

struct TrackText {
    std::string title, artist, album;
    uint64_t serial = 0;  // для поиска картинки обложки в кэше окна
    // Цвета для фона окна и заглушки: из картинки обложки или из хеша названия (как в макете)
    theme::CoverPalette palette;
    bool hasImage = false;  // есть настоящая картинка обложки
};

// Строка текста с анимацией смены (уход, подмена, приход)
struct LineAnim {
    std::string shown;
    std::string pending;
    bool hasPending = false;
    double switchAt = 0;
    motion::Tween alpha{1};
    motion::Tween dy{0};
};

// Слой обложки: при смене трека старый уходит, новый приходит
struct CoverSlot {
    uint64_t serial = 0;
    theme::CoverPalette palette;
    bool hasImage = false;
    motion::Tween alpha{1};
    motion::Tween dx{0};
    motion::Tween scale{1};
    bool leaving = false;
    double removeAt = 0;
};

class PlayerView {
public:
    PlayerView();

    void SetHost(IViewHost* host) { host_ = host; }

    // Применить тему. animate - с затуханием окна (как смена пресета в макете).
    void ApplyTheme(const theme::Theme& t, const theme::LayoutGeometry& geometry, bool systemReduce, double now,
                    bool animate);
    // Восстановить режимы при запуске (без анимации)
    void RestoreModes(bool mini, bool playlistOpen);

    void OnTrackChanged(const TrackText& text, int direction, double now, bool animate);
    void SetPlaylistSize(int count) { listCount = count; }

    // ----- ввод (DIP) -----
    void OnMouseMove(Point p, double now);
    void OnMouseDown(Point p, double now);
    void OnMouseUp(Point p, double now);
    void OnMouseLeave(double now);
    void OnRightClick(Point p);
    void OnWheel(Point p, float delta, double now);
    bool HasCapture() const { return captureSlider_ != nullptr; }

    enum class Hit { Client, Caption };
    Hit HitTest(Point p) const;

    void TogglePlaylist(double now);
    void ToggleMini(double now);

    // ----- кадр -----
    // spectrum - полосы спектра или nullptr (окно не видно, мини-режим, пауза)
    void Update(double now, const PlaybackSnapshot& snap, const std::vector<float>* spectrum, bool visible);
    bool NeedsFrames() const { return needsFrames_; }
    bool WantsSecondTicks() const { return wantsTicks_; }
    // Нужен ли спектр в этом кадре (визуализатор виден)
    bool WantsSpectrum() const { return !mini && visible_; }

    // ----- состояние для рисования (только чтение) -----
    // Своя копия темы: при смене темы старая ещё рисуется, пока окно гаснет
    theme::Theme theme;
    bool hasTheme = false;
    MotionParams motion;
    std::unique_ptr<ILayout> layout;
    LayoutInput layoutInput;
    Frame frame;
    motion::Timeline timeline;

    bool mini = false;
    bool playlistOpen = false;
    motion::Tween fullAlpha{1};  // содержимое полного режима
    motion::Tween miniAlpha{0};  // содержимое мини-режима
    motion::Tween panelAlpha{0};
    motion::Tween panelShift{-28};
    std::vector<motion::Tween> rowAlpha;  // лесенка строк при открытии
    std::vector<motion::Tween> rowShift;
    std::vector<motion::Tween> rowHover;
    int hoverRow = -1;
    int listCount = 0;
    float listScroll = 0;
    motion::Tween scrollAnim{0};

    motion::Tween windowAlpha{1};  // затухание окна при смене темы
    motion::Tween windowScale{1};

    std::array<ButtonState, static_cast<size_t>(Elem::Count)> buttons;
    ToggleState shuffleToggle, repeatToggle, playlistToggle;
    motion::Tween playMorph{0};  // 0 - треугольник, 1 - две полоски
    SliderState seek, volume, miniSeek;
    motion::Tween wave1{1}, wave2{1}, muteCross{0};

    motion::ColorTween c1, c2, c3, accent;  // цвета обложки для фона и акцент
    std::vector<CoverSlot> covers;
    LineAnim title, artist, album, miniTitle, miniArtist;
    Marquee titleMarquee, miniMarquee;
    float titleTextWidth = 0, miniTextWidth = 0;
    VisualizerModel viz;

    PlaybackSnapshot snap;  // последний снимок
    double now = 0;

    ButtonState& Button(Elem e) { return buttons[static_cast<size_t>(e)]; }
    const ButtonState& Button(Elem e) const { return buttons[static_cast<size_t>(e)]; }
    // Положение показа ползунков (с учётом перетаскивания и доезда)
    float SeekDisplay() const;
    float VolumeDisplay() const;
    // Время для подписи слева (при перетаскивании - под мышью)
    double DisplayPosition() const;
    bool DarkUi() const;  // светлый текст - тёмная тема

private:
    void Schedule(double at, std::function<void()> fn);
    void Rearrange();
    // Изменить размер окна и пересчитать раскладку под него
    void Resize(bool withPanel);
    void ApplyThemeNow(const theme::Theme& t, const theme::LayoutGeometry& geometry, bool systemReduce);
    void ApplyCoverColors(const theme::CoverPalette& pal, bool animate);
    void SwapLine(LineAnim& line, const std::string& value, int order, bool animate);
    void MeasureMarquees();
    Elem HitElement(Point p) const;
    SliderState* SliderAt(Point p, Elem* which);
    float SliderValueAt(Elem e, float x) const;
    void Activate(Elem e);
    void SetHovered(Elem e);
    int RowAt(Point p) const;
    Size FullWindowSize(bool withPanel) const;

    IViewHost* host_ = nullptr;
    struct Pending {
        double at;
        std::function<void()> fn;
    };
    std::vector<Pending> pending_;
    Elem hovered_ = Elem::Count;
    Elem pressed_ = Elem::Count;
    SliderState* captureSlider_ = nullptr;
    Elem captureElem_ = Elem::Count;
    bool visible_ = true;
    bool needsFrames_ = false;
    bool wantsTicks_ = false;
    double lastUpdate_ = 0;
    theme::CoverPalette currentPalette_;
    bool firstSnapshot_ = true;
    bool systemReduce_ = false;
    bool windowWithPanel_ = false;  // окно сейчас шире на панель плейлиста
};

}  // namespace view
