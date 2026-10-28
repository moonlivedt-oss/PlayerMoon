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

// Стили текста, ширину которых view спрашивает у окна (размеры - в ui/painter.cpp)
enum class TextStyle { Title, MiniTitle, Tab, TabSmall, Pill, Tooltip };

// Страница плагина в панели (копия данных PluginManager, переведённая в вид для показа)
struct PanelAction {
    int id = 0;
    std::string label;
    float width = 0;  // ширина кнопки-«пилюли» в DIP (считает SetPages)
};

struct PanelRow {
    int id = 0;
    std::string title, subtitle, status;
    float progress = -1;  // меньше 0 - без полосы
    int state = 0;        // PM_ROW_*: 0 обычная, 1 идёт работа, 2 готово, 3 ошибка, 4 в очереди
    std::vector<PanelAction> actions;
};

struct PanelPage {
    std::string title, emptyText;
    std::vector<PanelAction> actions;  // кнопки шапки
    std::vector<PanelRow> rows;
};

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
    // Меню строки плейлиста (правый клик): играть следующим, убрать, показать в папке
    virtual void ShowRowMenu(int row, Point at) = 0;
    // Раздел «Плагины»: включить или выключить плагин, открыть папку плагинов
    virtual void TogglePlugin(int index) = 0;
    virtual void OpenPluginsFolder() = 0;
    // Страница плагина: нажата кнопка (rowId = 0 - кнопка шапки)
    virtual void PageAction(int page, int rowId, int actionId) = 0;
    // Пустой плейлист: кнопки «Открыть папку» и «Открыть файлы»
    virtual void OpenFiles(bool folder) = 0;
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
    // ----- вкладки панели: «Очередь», страницы плагинов, «Плагины» -----
    enum class TabKind { Queue, Page, Plugins };
    int TabCount() const { return 2 + static_cast<int>(pages.size()); }
    TabKind KindOf(int tab) const {
        return tab <= 0 ? TabKind::Queue : tab >= TabCount() - 1 ? TabKind::Plugins : TabKind::Page;
    }
    int PageOf(int tab) const { return tab - 1; }
    int PluginsTab() const { return TabCount() - 1; }
    int PageTab(int page) const { return 1 + page; }
    TabKind CurrentKind() const { return KindOf(panelTab); }
    // Открывает панель, если она закрыта, и переключает вкладку
    void ShowPanelTab(int tab, double now);
    // Плагины для раздела: включён ли каждый (переключатели анимируются)
    void SetPlugins(const std::vector<bool>& enabled, double now);
    // Страницы плагинов (вкладка остаётся на месте, если её страница жива)
    void SetPages(std::vector<PanelPage> newPages, double now);
    // Сколько строк в списке текущей вкладки (у плагинов последняя - «Открыть папку плагинов»)
    int RowCount() const;

    // Геометрия содержимого вкладок (общая для попадания мышью и рисования)
    static constexpr float kPageRowHeight = 64;
    static constexpr float kPillHeight = 26;
    float PageBarHeight(int page) const;
    Rect PageActionRect(int page, int i) const;  // кнопки шапки страницы
    Rect PageRowRect(int page, int row) const;
    Rect PageRowActionRect(int page, int row, int k) const;  // кнопки строки справа
    Rect EmptyActionRect(int i) const;  // пустой плейлист: 0 - «Открыть папку», 1 - «Открыть файлы»
    float ContentHeight() const;  // высота прокручиваемого списка текущей вкладки
    float ListViewHeight() const;  // видимая высота списка (без шапки страницы)

    // ----- подсказки и уведомления -----
    void ShowToast(const std::string& text, double now);
    // Занятость (поиск музыки) - пульсирующая подпись в шапке очереди
    void SetBusy(bool on, const std::string& text);
    // Когда понадобится следующий кадр по «ленивому» таймеру (подсказка, уведомление);
    // бесконечность - не нужен. Такие ожидания не держат цикл кадров - окно ставит таймер.
    double NextLazyWake() const;

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
    int panelTab = 0;             // индекс вкладки (см. KindOf)
    motion::Tween tabAnim{1};     // проявление списка после смены вкладки
    std::vector<PanelPage> pages;
    std::vector<Rect> tabRects;   // вкладки в шапке панели
    std::vector<motion::Tween> tabHover;
    float tabTextSize = 15;       // при трёх и больше вкладках - мельче
    motion::Tween tabLineX{0}, tabLineW{0};  // полоска под активной вкладкой (едет при смене)
    // Наведение на кнопку-«пилюлю»: строка (-1 шапка страницы, -3 пустой плейлист) и номер
    int hoverPillRow = -2, hoverPillIdx = -1;
    motion::Tween pillHover{0};
    int pressedRow = -1;          // нажатая строка (лёгкое уменьшение)
    motion::Tween rowPress{0};
    motion::Tween scrollbarAlpha{0};
    // Подсказка над элементом
    std::string tooltipText;
    Rect tooltipAnchor;
    motion::Tween tooltipAlpha{0};
    // Уведомление
    std::string toastText;
    motion::Tween toastAlpha{0};
    bool busy = false;
    std::string busyText;
    int pluginCount = 0;
    std::vector<motion::Tween> pluginOn;  // положение переключателей 0..1
    bool showRemaining = false;   // справа - оставшееся время (клик по времени)
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
    void SwitchTab(int tab);
    void LayoutTabs();
    int TabAt(Point p) const;
    // Кнопка-«пилюля» под точкой: true и row/idx (row: -1 шапка страницы, -3 пустой плейлист)
    bool PillAt(Point p, int* row, int* idx) const;
    void SetPillHover(int row, int idx);
    void UpdateTooltip(Point p);
    void HideTooltip();
    std::string TooltipFor(Elem e, Point p) const;
    void ShowScrollbar(bool autoHide);
    void ScheduleLazy(double at, std::function<void()> fn);
    int RowAt(Point p) const;
    Size FullWindowSize(bool withPanel) const;

    IViewHost* host_ = nullptr;
    struct Pending {
        double at;
        std::function<void()> fn;
    };
    std::vector<Pending> pending_;
    std::vector<Pending> lazy_;  // не держат цикл кадров
    Elem tooltipElem_ = Elem::Count;
    int tooltipGen_ = 0, toastGen_ = 0, scrollGen_ = 0;
    double tooltipHiddenAt_ = -10;
    bool overList_ = false;
    int pressedTab_ = -1;
    int pressedPillRow_ = -2, pressedPillIdx_ = -1;
    Point lastMouse_;
    float emptyPillW_[2] = {120, 120};
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
