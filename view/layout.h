#pragma once
// Раскладки окна: где лежит каждый элемент. Переносимый код без Win32 - проверяется тестами
// против координат элементов в mockup/index.html.
//
// Раскладка - отдельный класс (ILayout). Этап 1: «Классика» (ClassicLayout); остальные
// («Сцена», «Журнал», «Горизонт») добавляются новыми классами и строкой в MakeLayout(),
// ядро и тема при этом не меняются. Мини-режим общий для всех раскладок (ArrangeMini).

#include <array>
#include <cstddef>
#include <memory>
#include <string_view>

#include "theme/theme.h"
#include "view/geometry.h"

namespace view {

// Элементы окна, у которых есть прямоугольник
enum class Elem {
    TitleBar,
    Brand,
    TbSettings,
    TbMini,
    TbMinimize,
    TbClose,
    Main,  // область полного режима без плейлиста
    Cover,
    Title,
    Artist,
    Album,
    Viz,
    Seek,
    TimeCur,
    TimeDur,
    Rate,
    Shuffle,
    Prev,
    Play,
    Next,
    Repeat,
    VolBtn,
    Volume,
    PlBtn,
    Panel,  // плейлист
    PanelTitle,  // вкладка «Очередь»
    PanelTab2,   // вкладка «Плагины»
    PanelCount,
    List,   // область строк плейлиста
    MiniCover,
    MiniTitle,
    MiniArtist,
    MiniPlay,
    MiniExpand,
    MiniSeek,
    Count
};

struct Frame {
    std::array<Rect, static_cast<size_t>(Elem::Count)> rects{};
    float rowHeight = 54;
    // Колонки строки плейлиста относительно её левого края
    float rowNumX = 12, rowNumW = 28, rowTextX = 52, rowDurRight = 12;

    Rect& operator[](Elem e) { return rects[static_cast<size_t>(e)]; }
    const Rect& operator[](Elem e) const { return rects[static_cast<size_t>(e)]; }
    // Прямоугольник строки плейлиста с номером i при прокрутке scroll
    Rect Row(int i, float scroll) const;
};

// Высоты строк текста: line-height из CSS макета (1.25 у названия, 1.4 у подписей)
struct TextMetrics {
    static float TitleLine(float fs) { return fs * 1.25f; }
    static float SubLine(float fs) { return fs * 1.4f; }
    static float RowTitleLine(float fs) { return fs * 1.3f; }
    static float RowSubLine(float fs) { return fs * 1.35f; }
    static constexpr float kTimesHeight = 16;  // ряд времени: шрифт 12 и значок скорости
};

struct LayoutInput {
    const theme::Theme* theme = nullptr;
    theme::LayoutGeometry geometry;  // из assets/layouts.json для выбранной раскладки
    float textScale = 1;             // settings.textScale
};

class ILayout {
public:
    virtual ~ILayout() = default;
    virtual std::string_view Id() const = 0;
    // Размер окна в DIP: полный режим, с открытым плейлистом (если он расширяет окно)
    virtual Size WindowSize(const LayoutInput& in, bool playlistOpen) const = 0;
    // Раскладка полного режима и плейлиста. playlistOpen - окно сейчас с панелью плейлиста
    // (от этого зависит ширина заголовка и место кнопок справа).
    virtual void Arrange(const LayoutInput& in, bool playlistOpen, Frame& f) const = 0;
};

// Раскладка по id из темы. Неизвестные (ещё не сделанные) - «Классика».
std::unique_ptr<ILayout> MakeLayout(std::string_view id);

// Мини-режим: одинаков для всех раскладок (mockup/LAYOUTS.md)
constexpr float kMiniWidth = 360;
constexpr float kMiniHeight = 88;
void ArrangeMini(const theme::Material& m, float textScale, Frame& f);

}  // namespace view
