#pragma once
// Отрисовка окна плеера: берёт состояние view::PlayerView и рисует кадр через Direct2D.
// Здесь только «как выглядит» (перенос CSS из mockup/index.html); что и когда двигается,
// решает PlayerView. Все размеры в DIP: контекст Renderer уже настроен на DPI окна.
//
// Ресурсы устройства (кисти, эффекты тени и размытия, текстура шума) создаются лениво и
// сбрасываются при смене поколения устройства Renderer.

#include <d2d1_1.h>
#include <d2d1effects.h>
#include <wrl/client.h>

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "theme/theme.h"
#include "ui/covers.h"
#include "ui/icons.h"
#include "ui/renderer.h"
#include "ui/text.h"
#include "view/player_view.h"

namespace ui {

using Microsoft::WRL::ComPtr;

// Строка плейлиста, готовая к показу
struct PlaylistRow {
    std::wstring title, artist, duration;
};

struct PaintInput {
    view::Size window;            // размер окна, DIP
    bool systemBackdrop = false;  // системный акрил под окном работает (Windows 11)
    const std::vector<PlaylistRow>* rows = nullptr;
    std::wstring countText;  // «12 треков, 48 мин»
    std::wstring emptyHint;  // подсказка, когда плейлист пуст
};

class Painter {
public:
    void Init(Renderer* renderer, TextSystem* text, IconSet* icons, CoverCache* covers);
    void Paint(ID2D1DeviceContext* dc, const view::PlayerView& v, const PaintInput& in);

    // Стили текста, общие с измерением ширины для бегущей строки
    static TextSpec TitleSpec(const view::PlayerView& v);
    static TextSpec MiniTitleSpec(const view::PlayerView& v);

private:
    using Rgba = motion::Rgba;

    // ----- ресурсы -----
    void EnsureResources(ID2D1DeviceContext* dc);
    ID2D1SolidColorBrush* Brush(const Rgba& c, float opacity = 1);
    ComPtr<ID2D1GradientStopCollection> Stops(std::initializer_list<std::pair<float, Rgba>> stops);
    ComPtr<ID2D1LinearGradientBrush> Linear(D2D1_POINT_2F a, D2D1_POINT_2F b,
                                            std::initializer_list<std::pair<float, Rgba>> stops);
    ComPtr<ID2D1RadialGradientBrush> Radial(D2D1_POINT_2F center, float rx, float ry,
                                            std::initializer_list<std::pair<float, Rgba>> stops);
    // Линейный градиент по правилам CSS: угол в градусах, 0 - вверх, по часовой
    ComPtr<ID2D1LinearGradientBrush> CssLinear(const D2D1_RECT_F& r, float deg,
                                               std::initializer_list<std::pair<float, Rgba>> stops);

    // ----- части кадра -----
    void DrawBackground(const view::PlayerView& v, const PaintInput& in);
    void DrawTitleBar(const view::PlayerView& v);
    void DrawCover(const view::PlayerView& v, const view::Rect& r, float radius, bool withAmbient);
    void DrawPlaceholder(const theme::CoverPalette& p, const D2D1_RECT_F& r, ID2D1Geometry* clip, float alpha);
    void DrawMeta(const view::PlayerView& v);
    void DrawVisualizer(const view::PlayerView& v);
    void DrawSlider(const view::PlayerView& v, const view::SliderState& s, const view::Rect& r, float value,
                    bool squareTrack);
    void DrawTimes(const view::PlayerView& v);
    void DrawControls(const view::PlayerView& v);
    void DrawButton(const view::PlayerView& v, view::Elem e, IconId icon, float iconSize, const view::ToggleState* tg);
    void DrawPlayButton(const view::PlayerView& v, view::Elem e);
    void DrawPlaylist(const view::PlayerView& v, const PaintInput& in);
    void DrawMini(const view::PlayerView& v);

    // Иконка: квадрат size с центром c. alphaOf - прозрачность и масштаб анимируемых слоёв
    struct PartLook {
        float alpha = 1, scale = 1;
        D2D1_POINT_2F origin{12, 12};
    };
    void DrawIcon(const Icon& icon, D2D1_POINT_2F c, float size, const Rgba& color, float stroke,
                  const PartLook* parts = nullptr, float rotateDeg = 0);
    // Строка текста с анимацией смены и (если нужно) бегущей строкой
    void DrawLine(const std::wstring& text, const TextSpec& spec, const view::Rect& r, const Rgba& color,
                  const view::LineAnim& anim, const view::Marquee* mq, double now);

    // ----- тени и размытие -----
    void DrawShadow(const D2D1_RECT_F& r, float radius, const theme::Shadow& shadow, float opacity);
    void DrawShadowLayer(const D2D1_RECT_F& r, float radius, float dx, float dy, float blur, float spread,
                         const Rgba& color);
    ID2D1Effect* ShadowFor(float w, float h, float radius, float blur);

    // Текущее преобразование, к которому добавляются локальные
    void PushTransform(const D2D1_MATRIX_3X2_F& m);
    void PopTransform();
    void PushOpacity(float opacity, const D2D1_RECT_F* clip = nullptr);
    void PopOpacity();

    Rgba Color(const theme::ThemeColor& c) const { return theme::Resolve(c, ctx_); }
    Rgba Text2() const;
    Rgba Text3() const;
    float Radius(float r, float w, float h) const;

    Renderer* renderer_ = nullptr;
    TextSystem* text_ = nullptr;
    IconSet* icons_ = nullptr;
    CoverCache* covers_ = nullptr;

    ID2D1DeviceContext* dc_ = nullptr;
    ComPtr<ID2D1DeviceContext> aux_;  // для записи фигур в command list (входы эффектов)
    uint64_t generation_ = 0;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1StrokeStyle> roundStroke_;
    ComPtr<ID2D1Bitmap1> noise_;
    ComPtr<ID2D1BitmapBrush> noiseBrush_;
    std::map<std::tuple<int, int, int, int>, ComPtr<ID2D1Effect>> shadows_;
    struct Ambient {
        ComPtr<ID2D1Effect> blur;
        std::vector<uint32_t> key;
    } ambient_;
    std::vector<D2D1_MATRIX_3X2_F> transforms_;
    std::vector<bool> layers_;  // true - слой, false - пропущен (прозрачность 1)

    theme::ColorContext ctx_;
    Rgba accent_, accent2_, text_c_;
    const view::PlayerView* v_ = nullptr;
};

}  // namespace ui
