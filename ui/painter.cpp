#include "ui/painter.h"

#include <d2d1_1helper.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

#include "core/text.h"

namespace ui {

using view::Elem;
using view::Rect;

namespace {

constexpr float kPi = 3.14159265f;

D2D1_COLOR_F ToD2D(const motion::Rgba& c, float opacity = 1) { return {c.r, c.g, c.b, c.a * opacity}; }
D2D1_RECT_F ToD2D(const Rect& r) { return {r.x, r.y, r.Right(), r.Bottom()}; }
D2D1_RECT_F Inflate(const D2D1_RECT_F& r, float d) { return {r.left - d, r.top - d, r.right + d, r.bottom + d}; }
float Width(const D2D1_RECT_F& r) { return r.right - r.left; }
float Height(const D2D1_RECT_F& r) { return r.bottom - r.top; }

motion::Rgba WithAlpha(motion::Rgba c, float a) {
    c.a *= a;
    return c;
}

// Время для подписи: 3:07 или 1:02:03
std::wstring FormatTime(double seconds) {
    if (!(seconds > 0)) seconds = 0;
    const long t = static_cast<long>(seconds);
    wchar_t buf[32];
    if (t >= 3600) swprintf(buf, 32, L"%ld:%02ld:%02ld", t / 3600, (t / 60) % 60, t % 60);
    else swprintf(buf, 32, L"%ld:%02ld", t / 60, t % 60);
    return buf;
}

// Масштаб относительно точки (для кнопок и слоёв иконок)
D2D1_MATRIX_3X2_F ScaleAround(float s, D2D1_POINT_2F c) { return D2D1::Matrix3x2F::Scale(s, s, c); }

// a, затем b (порядок как у Matrix3x2F: точка * a * b)
D2D1_MATRIX_3X2_F Multiply(const D2D1_MATRIX_3X2_F& a, const D2D1_MATRIX_3X2_F& b) {
    D2D1_MATRIX_3X2_F r;
    r._11 = a._11 * b._11 + a._12 * b._21;
    r._12 = a._11 * b._12 + a._12 * b._22;
    r._21 = a._21 * b._11 + a._22 * b._21;
    r._22 = a._21 * b._12 + a._22 * b._22;
    r._31 = a._31 * b._11 + a._32 * b._21 + b._31;
    r._32 = a._31 * b._12 + a._32 * b._22 + b._32;
    return r;
}

// CSS ease-in-out (для полосок «эквалайзера» текущего трека)
const motion::CubicBezier kEaseInOut{0.42, 0, 0.58, 1};

}  // namespace

void Painter::Init(Renderer* renderer, TextSystem* text, IconSet* icons, CoverCache* covers) {
    renderer_ = renderer;
    text_ = text;
    icons_ = icons;
    covers_ = covers;
}

// ---------------- ресурсы ----------------

void Painter::EnsureResources(ID2D1DeviceContext* dc) {
    dc_ = dc;
    if (generation_ == renderer_->DeviceGeneration() && brush_) return;
    generation_ = renderer_->DeviceGeneration();
    brush_.Reset();
    aux_.Reset();
    noise_.Reset();
    noiseBrush_.Reset();
    shadows_.clear();
    ambient_ = {};

    dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush_);
    if (!roundStroke_) {
        renderer_->Factory()->CreateStrokeStyle(
            D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                        D2D1_LINE_JOIN_ROUND),
            nullptr, 0, &roundStroke_);
    }
    if (renderer_->Device()) renderer_->Device()->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &aux_);

    // Зерно: 128x128 серого шума (замена feTurbulence из макета), повторяется по окну
    constexpr UINT kSide = 128;
    std::vector<uint8_t> px(kSide * kSide * 4);
    std::mt19937 rng(20240611);
    std::uniform_int_distribution<int> dist(0, 255);
    for (size_t i = 0; i < kSide * kSide; ++i) {
        const int v = dist(rng);
        const int a = 90 + dist(rng) * 100 / 255;  // непрозрачность 35-75%
        const auto pm = static_cast<uint8_t>(v * a / 255);  // premultiplied
        px[i * 4 + 0] = pm;
        px[i * 4 + 1] = pm;
        px[i * 4 + 2] = pm;
        px[i * 4 + 3] = static_cast<uint8_t>(a);
    }
    const auto props =
        D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(kSide, kSide), px.data(), kSide * 4, &props, &noise_))) {
        dc->CreateBitmapBrush(noise_.Get(),
                              D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP,
                                                          D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR),
                              &noiseBrush_);
    }
}

ID2D1SolidColorBrush* Painter::Brush(const Rgba& c, float opacity) {
    brush_->SetColor(ToD2D(c, opacity));
    return brush_.Get();
}

ComPtr<ID2D1GradientStopCollection> Painter::Stops(std::initializer_list<std::pair<float, Rgba>> stops) {
    D2D1_GRADIENT_STOP s[8];
    UINT32 n = 0;
    for (const auto& [pos, color] : stops) {
        if (n == 8) break;
        s[n++] = {pos, ToD2D(color)};
    }
    // Смешивание в premultiplied, как в CSS: «цвет -> transparent» не уходит в серый
    ComPtr<ID2D1GradientStopCollection1> coll;
    if (SUCCEEDED(dc_->CreateGradientStopCollection(s, n, D2D1_COLOR_SPACE_SRGB, D2D1_COLOR_SPACE_SRGB,
                                                    D2D1_BUFFER_PRECISION_8BPC_UNORM, D2D1_EXTEND_MODE_CLAMP,
                                                    D2D1_COLOR_INTERPOLATION_MODE_PREMULTIPLIED, &coll)))
        return coll;
    // Запасной путь (реализации D2D без этого метода): обычная коллекция
    ComPtr<ID2D1GradientStopCollection> plain;
    dc_->CreateGradientStopCollection(s, n, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &plain);
    return plain;
}

ComPtr<ID2D1LinearGradientBrush> Painter::Linear(D2D1_POINT_2F a, D2D1_POINT_2F b,
                                                 std::initializer_list<std::pair<float, Rgba>> stops) {
    ComPtr<ID2D1LinearGradientBrush> br;
    auto coll = Stops(stops);
    if (coll) dc_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(a, b), coll.Get(), &br);
    return br;
}

ComPtr<ID2D1RadialGradientBrush> Painter::Radial(D2D1_POINT_2F center, float rx, float ry,
                                                 std::initializer_list<std::pair<float, Rgba>> stops) {
    ComPtr<ID2D1RadialGradientBrush> br;
    auto coll = Stops(stops);
    if (coll)
        dc_->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(center, D2D1::Point2F(), rx, ry),
                                       coll.Get(), &br);
    return br;
}

ComPtr<ID2D1LinearGradientBrush> Painter::CssLinear(const D2D1_RECT_F& r, float deg,
                                                    std::initializer_list<std::pair<float, Rgba>> stops) {
    // Линия градиента CSS проходит через центр; длина - чтобы углы получили крайние цвета
    const float a = deg * kPi / 180;
    const float dx = std::sin(a), dy = -std::cos(a);
    const float w = Width(r), h = Height(r);
    const float len = std::fabs(w * dx) + std::fabs(h * dy);
    const float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    return Linear({cx - dx * len / 2, cy - dy * len / 2}, {cx + dx * len / 2, cy + dy * len / 2}, stops);
}

// ---------------- преобразования и слои ----------------

void Painter::PushTransform(const D2D1_MATRIX_3X2_F& m) {
    const D2D1_MATRIX_3X2_F top = transforms_.empty() ? D2D1::Matrix3x2F::Identity() : transforms_.back();
    const D2D1_MATRIX_3X2_F next = Multiply(m, top);
    transforms_.push_back(next);
    dc_->SetTransform(next);
}

void Painter::PopTransform() {
    transforms_.pop_back();
    dc_->SetTransform(transforms_.empty() ? D2D1::Matrix3x2F::Identity() : transforms_.back());
}

void Painter::PushOpacity(float opacity, const D2D1_RECT_F* clip) {
    if (opacity >= 0.999f && !clip) {
        layers_.push_back(false);
        return;
    }
    dc_->PushLayer(D2D1::LayerParameters1(clip ? *clip : D2D1::InfiniteRect(), nullptr,
                                          D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(),
                                          std::clamp(opacity, 0.f, 1.f)),
                   nullptr);
    layers_.push_back(true);
}

void Painter::PopOpacity() {
    if (layers_.back()) dc_->PopLayer();
    layers_.pop_back();
}

// ---------------- цвета ----------------

motion::Rgba Painter::Text2() const {
    // Свой цвет текста: второй и третий уровни из него (74% и 58%), иначе - из материала темы
    return v_->theme.customText ? WithAlpha(text_c_, 0.74f) : Color(v_->theme.material.color.text2);
}

motion::Rgba Painter::Text3() const {
    return v_->theme.customText ? WithAlpha(text_c_, 0.58f) : Color(v_->theme.material.color.text3);
}

float Painter::Radius(float r, float w, float h) const { return std::min(r, std::min(w, h) / 2); }

// ---------------- тени ----------------

ID2D1Effect* Painter::ShadowFor(float w, float h, float radius, float blur) {
    // Фигура (прямоугольник со скруглением) записывается один раз в command list;
    // эффект Shadow размывает её на GPU. Цвет задаётся при каждом рисовании.
    const auto key = std::make_tuple(static_cast<int>(std::lround(w * 2)), static_cast<int>(std::lround(h * 2)),
                                     static_cast<int>(std::lround(radius * 2)), static_cast<int>(std::lround(blur * 2)));
    auto it = shadows_.find(key);
    if (it != shadows_.end()) return it->second.Get();
    if (!aux_ || shadows_.size() > 64) shadows_.clear();
    if (!aux_) return nullptr;

    ComPtr<ID2D1CommandList> list;
    ComPtr<ID2D1SolidColorBrush> white;
    if (FAILED(aux_->CreateCommandList(&list)) ||
        FAILED(aux_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &white)))
        return nullptr;
    aux_->SetTarget(list.Get());
    aux_->BeginDraw();
    aux_->SetTransform(D2D1::IdentityMatrix());
    aux_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, w, h), radius, radius), white.Get());
    const HRESULT hr = aux_->EndDraw();
    aux_->SetTarget(nullptr);
    if (FAILED(hr) || FAILED(list->Close())) return nullptr;

    ComPtr<ID2D1Effect> fx;
    if (FAILED(dc_->CreateEffect(CLSID_D2D1Shadow, &fx))) return nullptr;
    fx->SetInput(0, list.Get());
    // Размытие box-shadow в CSS - это два стандартных отклонения
    fx->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, std::max(0.f, blur / 2));
    shadows_[key] = fx;
    return fx.Get();
}

void Painter::DrawShadowLayer(const D2D1_RECT_F& r, float radius, float dx, float dy, float blur, float spread,
                              const Rgba& color) {
    if (color.a <= 0.003f) return;
    const D2D1_RECT_F s = Inflate(r, spread);
    const float w = Width(s), h = Height(s);
    if (w <= 0 || h <= 0) return;
    const float rad = std::max(0.f, std::min(radius + spread, std::min(w, h) / 2));
    if (blur < 0.5f) {
        dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(s.left + dx, s.top + dy, s.right + dx, s.bottom + dy),
                                                    rad, rad),
                                  Brush(color));
        return;
    }
    ID2D1Effect* fx = ShadowFor(w, h, rad, blur);
    if (!fx) return;
    const D2D1_VECTOR_4F c{color.r, color.g, color.b, color.a};
    fx->SetValue(D2D1_SHADOW_PROP_COLOR, c);
    dc_->DrawImage(fx, D2D1::Point2F(s.left + dx, s.top + dy));
}

void Painter::DrawShadow(const D2D1_RECT_F& r, float radius, const theme::Shadow& shadow, float opacity) {
    if (opacity <= 0.003f) return;
    for (const theme::ShadowLayer& l : shadow) {
        const Rgba c = WithAlpha(Color(l.color), opacity);
        if (c.a <= 0.003f) continue;
        if (!l.inset) {
            DrawShadowLayer(r, radius, l.x, l.y, l.blur, l.spread, c);
            continue;
        }
        // Внутренняя тень этапа 1 упрощённо: кольцо внутри фигуры (без размытия - точно как в CSS)
        const float width = l.blur > 0 ? std::max(1.f, l.blur * 0.5f) + std::max(0.f, l.spread)
                                       : std::max(0.f, l.spread);
        if (width <= 0) continue;
        const D2D1_RECT_F in = Inflate(r, -width / 2);
        const float rad = std::max(0.f, radius - width / 2);
        dc_->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(in.left + l.x, in.top + l.y, in.right + l.x, in.bottom + l.y), rad, rad),
            Brush(c, l.blur > 0 ? 0.5f : 1.f), width);
    }
}

// ---------------- стили текста ----------------

TextSpec Painter::TitleSpec(const view::PlayerView& v) {
    TextSpec s;
    s.size = v.layoutInput.geometry.fsTitle * v.layoutInput.textScale;
    s.weight = static_cast<float>(v.theme.settings.titleWeight);
    s.display = true;
    s.spacing = v.theme.material.font.lsTitle;
    s.align = DWRITE_TEXT_ALIGNMENT_CENTER;
    return s;
}

TextSpec Painter::MiniTitleSpec(const view::PlayerView& v) {
    TextSpec s;
    s.size = v.theme.material.font.fsMiniTitle * v.layoutInput.textScale;
    s.weight = 600;
    return s;
}

// ---------------- кадр ----------------

void Painter::Paint(ID2D1DeviceContext* dc, const view::PlayerView& v, const PaintInput& in) {
    if (!v.hasTheme) return;
    EnsureResources(dc);
    if (!brush_) return;
    v_ = &v;
    const double now = v.now;
    const theme::Settings& s = v.theme.settings;
    accent_ = v.accent.Value(now);
    accent2_ = s.accent2;
    text_c_ = s.text;
    ctx_.accent = accent_;
    ctx_.accent2 = accent2_;
    ctx_.text = text_c_;
    ctx_.windowSolid = s.winColor;
    ctx_.windowSolid.a = 1;
    ctx_.glow = static_cast<float>(s.glow);

    // Прозрачная поверхность: ClearType невозможен, только градации серого
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    transforms_.clear();
    layers_.clear();
    dc->SetTransform(D2D1::IdentityMatrix());

    // Смена темы: окно гаснет и чуть уменьшается
    const float winScale = v.windowScale.Value(now);
    PushTransform(ScaleAround(winScale, {in.window.w / 2, in.window.h / 2}));
    PushOpacity(v.windowAlpha.Value(now));

    DrawBackground(v, in);

    const float fullA = v.fullAlpha.Value(now);
    if (fullA > 0.002f) {
        PushOpacity(fullA);
        DrawTitleBar(v);
        const Rect& c = v.frame[Elem::Cover];
        const std::string& shape = s.coverShape;
        const float coverR = shape == "square" ? 2.f : shape == "rounded" ? 18.f : c.w / 2;
        DrawCover(v, c, coverR, true);
        DrawMeta(v);
        DrawVisualizer(v);
        DrawSlider(v, v.seek, v.frame[Elem::Seek], v.SeekDisplay(), false);
        DrawTimes(v);
        DrawControls(v);
        DrawPlaylist(v, in);
        PopOpacity();
    }
    const float miniA = v.miniAlpha.Value(now);
    if (miniA > 0.002f) {
        PushOpacity(miniA);
        DrawMini(v);
        PopOpacity();
    }

    PopOpacity();
    PopTransform();
}

void Painter::DrawBackground(const view::PlayerView& v, const PaintInput& in) {
    const theme::Settings& s = v.theme.settings;
    const double now = v.now;
    const D2D1_RECT_F r = D2D1::RectF(0, 0, in.window.w, in.window.h);
    const float w = in.window.w, h = in.window.h;
    const float tint = static_cast<float>(s.tint);
    const Rgba c1 = v.c1.Value(now), c2 = v.c2.Value(now), c3 = v.c3.Value(now);
    Rgba solid = s.winColor;
    solid.a = 1;
    auto fill = [&](ID2D1Brush* b) {
        if (b) dc_->FillRectangle(r, b);
    };
    auto radial = [&](float cxPct, float cyPct, float rxPct, float ryPct, const Rgba& c, float alpha, float stop) {
        auto br = Radial({w * cxPct, h * cyPct}, w * rxPct, h * ryPct,
                         {{0.f, WithAlpha(c, alpha)}, {stop, WithAlpha(c, 0)}});
        fill(br.Get());
    };

    if (s.backdrop == "acrylic") {
        // Под окном системный акрил; без него (Windows 10) основа непрозрачная
        Rgba base = s.winColor;
        base.a = in.systemBackdrop ? static_cast<float>(s.winDensity) : 1.f;
        fill(Brush(base));
        auto lin = CssLinear(r, 170, {{0.f, WithAlpha(c1, tint * 0.8f)}, {1.f, WithAlpha(c2, tint * 0.8f)}});
        fill(lin.Get());
        radial(0.5f, 0, 0.9f, 0.6f, c1, tint, 0.75f);
    } else if (s.backdrop == "solid") {
        fill(Brush(solid));
        radial(0.5f, 1, 0.8f, 0.4f, c2, tint * 0.7f, 0.8f);
        radial(0.5f, 0.25f, 0.7f, 0.45f, c3, tint, 0.8f);
    } else {
        // cover; coverblur (размытая картинка) - этап 2, пока как cover
        fill(Brush(solid));
        auto lin = CssLinear(r, 150, {{0.f, WithAlpha(c1, tint)}, {1.f, WithAlpha(c2, tint)}});
        fill(lin.Get());
        radial(0.15f, 0, 0.7f, 0.6f, c3, tint * 0.9f, 0.75f);
    }

    // Сетка 28 px цветом акцента
    if (s.grid > 0.001) {
        ID2D1SolidColorBrush* b = Brush(accent_, static_cast<float>(s.grid));
        for (float y = 0; y < h; y += 28) dc_->FillRectangle(D2D1::RectF(0, y, w, y + 1), b);
        for (float x = 0; x < w; x += 28) dc_->FillRectangle(D2D1::RectF(x, 0, x + 1, h), b);
    }
    // Зерно
    if (s.noise > 0.001 && noiseBrush_) {
        noiseBrush_->SetOpacity(static_cast<float>(s.noise));
        fill(noiseBrush_.Get());
    }
}

void Painter::DrawTitleBar(const view::PlayerView& v) {
    const theme::Material& m = v.theme.material;
    const Rect& brand = v.frame[Elem::Brand];
    const Icon& moon = icons_->Get(IconId::Moon);
    DrawIcon(moon, {brand.x + 8, brand.CenterY()}, 16, accent_, 0);
    TextSpec spec;
    spec.size = 12;
    spec.weight = 600;
    spec.spacing = 0.2f;
    spec.ellipsis = false;
    if (auto l = text_->Layout(L"PlayerMoon", spec, 0, 16))
        dc_->DrawTextLayout({brand.x + 24, brand.CenterY() - 8}, l.Get(), Brush(Text2()));

    struct TbDef {
        Elem e;
        IconId icon;
    };
    for (const TbDef& d : {TbDef{Elem::TbSettings, IconId::Settings}, TbDef{Elem::TbMini, IconId::MiniMode},
                           TbDef{Elem::TbMinimize, IconId::Minimize}, TbDef{Elem::TbClose, IconId::Close}}) {
        const Rect& r = v.frame[d.e];
        const view::ButtonState& st = v.Button(d.e);
        const float h = std::clamp(st.hoverAmt.Value(v.now), 0.f, 1.f);
        const bool close = d.e == Elem::TbClose;
        const Rgba bg = close ? Color(m.color.closeHover) : Color(m.color.btnHover);
        if (h > 0.003f) dc_->FillRectangle(ToD2D(r), Brush(bg, h));
        const Rgba fg = motion::Mix(Text2(), close ? Rgba{1, 1, 1, 1} : text_c_, h);
        // Значок настроек при наведении поворачивается, остальные - пружинят масштабом
        const bool settings = d.e == Elem::TbSettings;
        const float scale = settings ? 1.f : st.scale.Value(v.now);
        PushTransform(ScaleAround(scale, {r.CenterX(), r.CenterY()}));
        DrawIcon(icons_->Get(d.icon), {r.CenterX(), r.CenterY()}, 16, fg, 1.4f, nullptr, settings ? 60 * h : 0);
        PopTransform();
    }
}

void Painter::DrawIcon(const Icon& icon, D2D1_POINT_2F c, float size, const Rgba& color, float stroke,
                       const PartLook* parts, float rotateDeg) {
    const float k = size / icon.viewBox;
    D2D1_MATRIX_3X2_F m = D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(c.x - size / 2, c.y - size / 2);
    if (rotateDeg != 0)
        m = Multiply(D2D1::Matrix3x2F::Rotation(rotateDeg, {icon.viewBox / 2, icon.viewBox / 2}), m);
    PushTransform(m);
    for (const IconLayer& layer : icon.layers) {
        PartLook look;
        if (parts) look = parts[static_cast<size_t>(layer.part)];
        if (look.alpha <= 0.003f) continue;
        const bool scaled = std::fabs(look.scale - 1) > 0.001f;
        if (scaled) PushTransform(ScaleAround(look.scale, look.origin));
        ID2D1SolidColorBrush* b = Brush(color, look.alpha);
        if (layer.solid) {
            // .solid в макете: заливка и обводка 1.2
            dc_->FillGeometry(layer.geometry.Get(), b);
            dc_->DrawGeometry(layer.geometry.Get(), b, 1.2f, roundStroke_.Get());
        } else {
            dc_->DrawGeometry(layer.geometry.Get(), b, stroke, roundStroke_.Get());
        }
        if (scaled) PopTransform();
    }
    PopTransform();
}

// ---------------- обложка ----------------

void Painter::DrawPlaceholder(const theme::CoverPalette& p, const D2D1_RECT_F& r, ID2D1Geometry* clip,
                              float alpha) {
    // Перенос coverBackground() из макета: каждый слой CSS - кисть, которой заливается рамка обложки.
    // Без слоёв D2D: дешевле и обрезка по скруглению получается сама.
    const float w = Width(r), h = Height(r);
    auto fill = [&](ID2D1Brush* b) {
        if (!b) return;
        b->SetOpacity(alpha);
        dc_->FillGeometry(clip, b);
    };
    fill(CssLinear(r, static_cast<float>(p.angle), {{0.f, p.a}, {1.f, p.b}}).Get());
    // Мягкое пятно: radial-gradient(90% 90% at x2 y2, c / 0.55, transparent 60%)
    fill(Radial({r.left + w * p.x2 / 100.f, r.top + h * p.y2 / 100.f}, w * 0.9f, h * 0.9f,
                {{0.f, WithAlpha(p.c, 0.55f)}, {0.6f, WithAlpha(p.c, 0)}})
             .Get());
    // Диск: circle - радиус в % от расстояния до дальнего угла, край 0.7% (сглаживание)
    const float cx = r.left + w * p.x1 / 100.f;
    const float cy = r.top + h * (p.shape == 1 ? 64 : p.y1) / 100.f;
    const float fx = std::max(cx - r.left, r.right - cx), fy = std::max(cy - r.top, r.bottom - cy);
    const float ray = std::sqrt(fx * fx + fy * fy);
    const float discR = ray * p.disc / 100.f, edge = ray * (p.disc + 0.7f) / 100.f;
    fill(Radial({cx, cy}, edge, edge, {{0.f, p.c}, {discR / edge, p.c}, {1.f, WithAlpha(p.c, 0)}}).Get());
    if (p.shape == 1) {
        // Горизонт: linear-gradient(180deg, transparent 64%, rgba(0,0,0,.22) 64%)
        fill(Linear({0, r.top}, {0, r.bottom},
                    {{0.f, {0, 0, 0, 0}}, {0.639f, {0, 0, 0, 0}}, {0.641f, {0, 0, 0, 0.22f}}, {1.f, {0, 0, 0, 0.22f}}})
                 .Get());
    } else if (p.shape == 2) {
        // Полосы: repeating-linear-gradient(ang + 45deg, white 10% 0 10px, transparent 10px 26px) -
        // градиент длиной 26 px с повтором (EXTEND_MODE_WRAP) от начала линии градиента CSS
        const float a = static_cast<float>(p.angle + 45) * kPi / 180;
        const float dx = std::sin(a), dy = -std::cos(a);
        const float len = std::fabs(w * dx) + std::fabs(h * dy);
        const D2D1_POINT_2F start{(r.left + r.right) / 2 - dx * len / 2, (r.top + r.bottom) / 2 - dy * len / 2};
        D2D1_GRADIENT_STOP st[4] = {{0, {1, 1, 1, 0.10f}}, {10.f / 26 - 0.001f, {1, 1, 1, 0.10f}},
                                    {10.f / 26 + 0.001f, {1, 1, 1, 0}}, {1, {1, 1, 1, 0}}};
        ComPtr<ID2D1GradientStopCollection> coll;
        ComPtr<ID2D1LinearGradientBrush> br;
        if (SUCCEEDED(dc_->CreateGradientStopCollection(st, 4, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_WRAP, &coll)) &&
            SUCCEEDED(dc_->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(start, {start.x + dx * 26, start.y + dy * 26}), coll.Get(), &br)))
            fill(br.Get());
    }
}

void Painter::DrawCover(const view::PlayerView& v, const Rect& rect, float radius, bool withAmbient) {
    const theme::Material& m = v.theme.material;
    const double now = v.now;
    const D2D1_RECT_F r = ToD2D(rect);
    radius = Radius(radius, rect.w, rect.h);

    // Подсветка под обложкой: размытый градиент цветов обложки, сдвиг вниз 14 и масштаб 0.92
    const float ambient = static_cast<float>(v.theme.settings.ambient);
    if (withAmbient && ambient > 0.003f && aux_) {
        const Rgba c1 = v.c1.Value(now), c2 = v.c2.Value(now), c3 = v.c3.Value(now);
        const float blur = m.effect.coverAmbientBlur;
        auto q = [](float x) { return static_cast<uint32_t>(std::lround(std::clamp(x, 0.f, 1.f) * 255)); };
        const std::vector<uint32_t> key{static_cast<uint32_t>(rect.w * 4), static_cast<uint32_t>(radius * 4),
                                        static_cast<uint32_t>(blur * 4),  q(c1.r), q(c1.g), q(c1.b), q(c2.r),
                                        q(c2.g), q(c2.b), q(c3.r), q(c3.g), q(c3.b)};
        if (ambient_.key != key || !ambient_.blur) {
            ambient_ = {};
            ComPtr<ID2D1CommandList> list;
            if (SUCCEEDED(aux_->CreateCommandList(&list))) {
                const float s = rect.w;
                const D2D1_RECT_F box = D2D1::RectF(0, 0, s, s);
                aux_->SetTarget(list.Get());
                aux_->BeginDraw();
                aux_->SetTransform(D2D1::IdentityMatrix());
                // Кисти здесь нужны от вспомогательного контекста: временно подменяем dc_
                ID2D1DeviceContext* main = dc_;
                dc_ = aux_.Get();
                auto geo = D2D1::RoundedRect(box, radius, radius);
                if (auto lin = CssLinear(box, 135, {{0.f, c1}, {1.f, c2}})) aux_->FillRoundedRectangle(geo, lin.Get());
                if (auto rad = Radial({s * 0.3f, s * 0.3f}, s * 0.6f, s * 0.6f, {{0.f, c3}, {0.7f, WithAlpha(c3, 0)}}))
                    aux_->FillRoundedRectangle(geo, rad.Get());
                dc_ = main;
                const HRESULT hr = aux_->EndDraw();
                aux_->SetTarget(nullptr);
                if (SUCCEEDED(hr) && SUCCEEDED(list->Close()) &&
                    SUCCEEDED(dc_->CreateEffect(CLSID_D2D1GaussianBlur, &ambient_.blur))) {
                    ambient_.blur->SetInput(0, list.Get());
                    // filter: blur(r) в CSS - это стандартное отклонение r
                    ambient_.blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, blur);
                    ambient_.blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_SOFT);
                    ambient_.key = key;
                }
            }
        }
        if (ambient_.blur) {
            PushOpacity(ambient);
            PushTransform(D2D1::Matrix3x2F::Scale(0.92f, 0.92f) *
                          D2D1::Matrix3x2F::Translation(rect.x + rect.w * 0.04f, rect.y + rect.h * 0.04f + 14));
            dc_->DrawImage(ambient_.blur.Get());
            PopTransform();
            PopOpacity();
        }
    }

    DrawShadow(r, radius, m.shadow.cover, 1);

    // Слои обложки: заливки скруглённой рамки (сама рамка неподвижна, содержимое сдвигается и масштабируется)
    ComPtr<ID2D1RoundedRectangleGeometry> clip;
    renderer_->Factory()->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r, radius, radius), &clip);
    if (!clip) return;
    if (v.covers.empty()) {
        if (auto g = CssLinear(r, 135, {{0.f, v.c1.Value(now)}, {1.f, v.c2.Value(now)}})) dc_->FillGeometry(clip.Get(), g.Get());
    }
    for (const view::CoverSlot& slot : v.covers) {
        const float a = std::clamp(slot.alpha.Value(now), 0.f, 1.f);
        if (a <= 0.003f) continue;
        // Прямоугольник содержимого после transform: scale вокруг центра и сдвиг по x
        const float sc = slot.scale.Value(now), dx = slot.dx.Value(now);
        const float hw = rect.w * sc / 2, hh = rect.h * sc / 2;
        const D2D1_RECT_F cr = D2D1::RectF(rect.CenterX() + dx - hw, rect.CenterY() - hh, rect.CenterX() + dx + hw,
                                           rect.CenterY() + hh);
        ID2D1Bitmap1* bmp = slot.hasImage ? covers_->Get(slot.serial, dc_, renderer_->DeviceGeneration()) : nullptr;
        if (bmp) {
            // object-fit: cover - картинка заполняет рамку с обрезкой по центру
            const D2D1_SIZE_F bs = bmp->GetSize();
            const float k = std::max(Width(cr) / bs.width, Height(cr) / bs.height);
            const D2D1_MATRIX_3X2_F mtx =
                D2D1::Matrix3x2F::Scale(k, k) *
                D2D1::Matrix3x2F::Translation((cr.left + cr.right - bs.width * k) / 2, (cr.top + cr.bottom - bs.height * k) / 2);
            ComPtr<ID2D1BitmapBrush1> hq;
            ComPtr<ID2D1BitmapBrush> brush;
            if (SUCCEEDED(dc_->CreateBitmapBrush(
                    bmp, D2D1::BitmapBrushProperties1(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                                                      D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC),
                    D2D1::BrushProperties(a, mtx), &hq)))
                brush = hq;
            else
                dc_->CreateBitmapBrush(bmp, D2D1::BitmapBrushProperties(), D2D1::BrushProperties(a, mtx), &brush);
            if (brush) dc_->FillGeometry(clip.Get(), brush.Get());
        } else {
            DrawPlaceholder(slot.palette, cr, clip.Get(), a);
        }
    }
    // Блик: диагональный отсвет и тонкая светлая рамка
    const float sheen = m.effect.coverSheen;
    if (sheen > 0.003f) {
        if (auto g = CssLinear(r, 160, {{0.f, {1, 1, 1, 0.12f * sheen}}, {0.4f, {1, 1, 1, 0}}}))
            dc_->FillGeometry(clip.Get(), g.Get());
        dc_->DrawRoundedRectangle(D2D1::RoundedRect(Inflate(r, -0.5f), std::max(0.f, radius - 0.5f), std::max(0.f, radius - 0.5f)),
                                  Brush({1, 1, 1, 0.16f * sheen}), 1);
    }
}

// ---------------- текст ----------------

void Painter::DrawLine(const std::wstring& text, const TextSpec& spec, const Rect& r, const Rgba& color,
                       const view::LineAnim& anim, const view::Marquee* mq, double now) {
    const float alpha = std::clamp(anim.alpha.Value(now), 0.f, 1.f);
    if (alpha <= 0.003f || text.empty()) return;
    const float dy = anim.dy.Value(now);
    ID2D1SolidColorBrush* b = Brush(color, alpha);
    if (!mq || !mq->Active()) {
        if (auto l = text_->Layout(text, spec, r.w, r.h)) dc_->DrawTextLayout({r.x, r.y + dy}, l.Get(), b);
        return;
    }
    // Бегущая строка: две копии подряд, края гаснут маской
    TextSpec s = spec;
    s.align = DWRITE_TEXT_ALIGNMENT_LEADING;
    s.ellipsis = false;
    auto l = text_->Layout(text, s, 0, r.h);
    if (!l) return;
    const D2D1_RECT_F box = D2D1::RectF(r.x, r.y - 4, r.Right(), r.Bottom() + 4);
    const float w = std::max(r.w, 1.f);
    const float fadeR = std::clamp((w - 28) / w, 0.f, 1.f);
    ComPtr<ID2D1LinearGradientBrush> mask;
    if (mq->Moving())
        mask = Linear({r.x, 0}, {r.Right(), 0},
                      {{0.f, {0, 0, 0, 0}}, {std::min(20 / w, fadeR), {0, 0, 0, 1}}, {fadeR, {0, 0, 0, 1}},
                       {1.f, {0, 0, 0, 0}}});
    else
        mask = Linear({r.x, 0}, {r.Right(), 0}, {{0.f, {0, 0, 0, 1}}, {fadeR, {0, 0, 0, 1}}, {1.f, {0, 0, 0, 0}}});
    dc_->PushLayer(D2D1::LayerParameters1(box, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 1,
                                          mask.Get()),
                   nullptr);
    const float x = r.x - mq->Offset();
    dc_->DrawTextLayout({x, r.y + dy}, l.Get(), b);
    dc_->DrawTextLayout({x + mq->Distance(), r.y + dy}, l.Get(), b);
    dc_->PopLayer();
}

void Painter::DrawMeta(const view::PlayerView& v) {
    const theme::Material& m = v.theme.material;
    const float ts = v.layoutInput.textScale;
    DrawLine(::text::Utf8ToWide(v.title.shown), TitleSpec(v), v.frame[Elem::Title], text_c_, v.title, &v.titleMarquee,
             v.now);

    TextSpec artist;
    artist.size = m.font.fsArtist * ts;
    artist.weight = m.font.fwArtist;
    artist.spacing = m.font.lsArtist;
    artist.upper = m.font.artistUpper;
    artist.align = DWRITE_TEXT_ALIGNMENT_CENTER;
    DrawLine(::text::Utf8ToWide(v.artist.shown), artist, v.frame[Elem::Artist], Text2(), v.artist, nullptr, v.now);

    TextSpec album;
    album.size = m.font.fsAlbum * ts;
    album.align = DWRITE_TEXT_ALIGNMENT_CENTER;
    DrawLine(::text::Utf8ToWide(v.album.shown), album, v.frame[Elem::Album], Text3(), v.album, nullptr, v.now);
}

// ---------------- визуализатор «Зеркало» ----------------

void Painter::DrawVisualizer(const view::PlayerView& v) {
    const theme::Settings& s = v.theme.settings;
    if (s.vizStyle == "off") return;
    const Rect& r = v.frame[Elem::Viz];
    const auto& vals = v.viz.Values();
    const int n = static_cast<int>(vals.size());
    if (n <= 0 || r.w <= 0) return;
    // Цвета полос: от A к B по индексу (vizColors() в макете)
    Rgba a = accent_, b = accent2_;
    if (s.vizColor == "cover" && !v.covers.empty()) {
        a = v.covers.back().palette.c;
        b = v.covers.back().palette.a;
    } else if (s.vizColor == "mono") {
        a = b = text_c_;
    }
    const float W = r.w, H = r.h;
    const float gap = std::max(1.f, W / n * 0.28f);
    const float bw = (W - gap * (n - 1)) / n;
    const float rad = std::min(bw / 2, 3.f);
    for (int i = 0; i < n; ++i) {
        const float val = vals[static_cast<size_t>(i)];
        const float t = n > 1 ? static_cast<float>(i) / (n - 1) : 0;
        const float x = r.x + i * (bw + gap);
        const float h = std::max(1.5f, val * (H / 2 - 1));
        const float cy = r.y + H / 2;
        dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, cy - h, x + bw, cy + h), rad, rad),
                                  Brush(motion::Mix(a, b, t), 0.4f + val * 0.6f));
    }
}

// ---------------- ползунки ----------------

void Painter::DrawSlider(const view::PlayerView& v, const view::SliderState& s, const Rect& r, float value,
                         bool squareTrack) {
    const theme::Material& m = v.theme.material;
    const double now = v.now;
    const float th = view::Lerp(m.size.track, m.size.trackHover, std::clamp(s.track.Value(now), 0.f, 1.f));
    const float cy = r.CenterY();
    const D2D1_RECT_F track = D2D1::RectF(r.x, cy - th / 2, r.Right(), cy + th / 2);
    const float tr = squareTrack ? 0.f : Radius(m.radius.track, r.w, th);
    dc_->FillRoundedRectangle(D2D1::RoundedRect(track, tr, tr), Brush(Color(m.color.track)));
    const float fw = r.w * std::clamp(value, 0.f, 1.f);
    if (fw > 0.5f) {
        const D2D1_RECT_F fill = D2D1::RectF(r.x, track.top, r.x + fw, track.bottom);
        const float fr = std::min(tr, fw / 2);
        const Rgba glow = Color(theme::ThemeColor{theme::ThemeColor::Ref::Glow, {}, 1});
        if (glow.a > 0.01f) DrawShadowLayer(fill, fr, 0, 0, 10, 0, glow);
        if (auto g = Linear({r.x, 0}, {r.x + fw, 0}, {{0.f, accent_}, {1.f, accent2_}}))
            dc_->FillRoundedRectangle(D2D1::RoundedRect(fill, fr, fr), g.Get());
    }
    const float sc = s.thumb.Value(now);
    if (sc > 0.01f) {
        const float w = m.size.thumbW * sc, h = m.size.thumbH * sc;
        const float x = r.x + fw;
        const D2D1_RECT_F t = D2D1::RectF(x - w / 2, cy - h / 2, x + w / 2, cy + h / 2);
        const float rr = Radius(m.radius.thumb * sc, w, h);
        DrawShadow(t, rr, m.shadow.thumb, 1);
        dc_->FillRoundedRectangle(D2D1::RoundedRect(t, rr, rr), Brush(Color(m.color.thumb)));
    }
}

void Painter::DrawTimes(const view::PlayerView& v) {
    const theme::Material& m = v.theme.material;
    TextSpec spec;
    spec.size = m.font.fsTime * v.layoutInput.textScale;
    spec.tabular = true;
    spec.ellipsis = false;
    const Rect& cur = v.frame[Elem::TimeCur];
    const Rect& dur = v.frame[Elem::TimeDur];
    if (auto l = text_->Layout(FormatTime(v.DisplayPosition()), spec, cur.w, cur.h))
        dc_->DrawTextLayout({cur.x, cur.y}, l.Get(), Brush(Text3()));
    spec.align = DWRITE_TEXT_ALIGNMENT_TRAILING;
    // Справа - длительность или оставшееся время (клик по времени переключает)
    const std::wstring right = v.showRemaining
                                   ? L"-" + FormatTime(std::ceil(std::max(0.0, v.snap.duration - v.DisplayPosition())))
                                   : FormatTime(v.snap.duration);
    if (auto l = text_->Layout(right, spec, dur.w, dur.h))
        dc_->DrawTextLayout({dur.x, dur.y}, l.Get(), Brush(Text3()));
}

// ---------------- кнопки ----------------

void Painter::DrawButton(const view::PlayerView& v, Elem e, IconId icon, float iconSize, const view::ToggleState* tg) {
    const theme::Material& m = v.theme.material;
    const view::MotionParams& mp = v.motion;
    const double now = v.now;
    const Rect& r = v.frame[e];
    const view::ButtonState& st = v.Button(e);
    const float h = std::clamp(st.hoverAmt.Value(now), 0.f, 1.f);
    const float p = std::clamp(st.pressAmt.Value(now), 0.f, 1.f);
    const float on = tg ? std::clamp(tg->dot.Value(now), 0.f, 1.f) : 0.f;
    const D2D1_POINT_2F c{r.CenterX(), r.CenterY()};
    PushTransform(ScaleAround(st.scale.Value(now), c) * D2D1::Matrix3x2F::Translation(0, st.lift.Value(now)));

    const D2D1_RECT_F rr = ToD2D(r);
    const float rad = Radius(m.radius.btn, r.w, r.h);
    // Реакция кнопок: тень при подъёме или ореол цвета акцента
    if (mp.liftShadow && h > 0.003f) DrawShadowLayer(rr, rad, 0, 6, 14, 0, {0, 0, 0, 0.22f * h});
    if (mp.glowButtons && h > 0.003f) {
        DrawShadowLayer(rr, rad, 0, 0, 16, 0, WithAlpha(accent_, 0.4f * h));
        dc_->DrawRoundedRectangle(D2D1::RoundedRect(Inflate(rr, 0.5f), rad + 0.5f, rad + 0.5f),
                                  Brush(accent_, 0.5f * h), 1);
    }
    if (on > 0.003f) DrawShadow(rr, rad, m.shadow.btnActive, on);
    const Rgba bg = motion::Mix(motion::Mix(Color(m.color.btn), Color(m.color.btnHover), h), Color(m.color.btnPress), p);
    if (bg.a > 0.003f) dc_->FillRoundedRectangle(D2D1::RoundedRect(rr, rad, rad), Brush(bg));

    Rgba fg = motion::Mix(Text2(), mp.glowButtons ? accent_ : text_c_, h);
    fg = motion::Mix(fg, accent_, on);

    PartLook parts[5];
    if (icon == IconId::Repeat && tg) {
        const float one = std::clamp(tg->one.Value(now), 0.f, 1.f);
        parts[static_cast<size_t>(IconPart::RepeatOne)] = {one, 0.4f + 0.6f * tg->one.Value(now), {12, 12}};
    }
    if (icon == IconId::Volume) {
        const float mute = std::clamp(v.muteCross.Value(now), 0.f, 1.f);
        const float waveScale = view::Lerp(1, 0.6f, mute);
        parts[static_cast<size_t>(IconPart::Wave1)] = {std::clamp(v.wave1.Value(now), 0.f, 1.f) * (1 - mute),
                                                        waveScale, {12, 12}};
        parts[static_cast<size_t>(IconPart::Wave2)] = {std::clamp(v.wave2.Value(now), 0.f, 1.f) * (1 - mute),
                                                        waveScale, {12, 12}};
        parts[static_cast<size_t>(IconPart::Mute)] = {mute, 0.3f + 0.7f * v.muteCross.Value(now), {18.5f, 12}};
    } else if (icon != IconId::Repeat) {
        parts[static_cast<size_t>(IconPart::RepeatOne)].alpha = 0;
    }
    DrawIcon(icons_->Get(icon), c, iconSize, fg, m.size.iconStroke, parts);

    // Точка под включённой кнопкой
    if (tg) {
        const float ds = tg->dot.Value(now);
        if (ds > 0.01f) {
            const float w = m.size.dotW * ds, hh = m.size.dotH * ds;
            const float cy = r.Bottom() - 3 - m.size.dotH / 2;
            const D2D1_RECT_F d = D2D1::RectF(c.x - w / 2, cy - hh / 2, c.x + w / 2, cy + hh / 2);
            const float dr = Radius(m.radius.dot * ds, w, hh);
            dc_->FillRoundedRectangle(D2D1::RoundedRect(d, dr, dr), Brush(accent_, m.effect.dot));
        }
    }
    PopTransform();
}

void Painter::DrawPlayButton(const view::PlayerView& v, Elem e) {
    const theme::Material& m = v.theme.material;
    const double now = v.now;
    const Rect& r = v.frame[e];
    const view::ButtonState& st = v.Button(e);
    const float h = std::clamp(st.hoverAmt.Value(now), 0.f, 1.f);
    const D2D1_POINT_2F c{r.CenterX(), r.CenterY()};
    PushTransform(ScaleAround(st.scale.Value(now), c) * D2D1::Matrix3x2F::Translation(0, st.lift.Value(now)));
    const D2D1_RECT_F rr = ToD2D(r);
    const float rad = Radius(m.radius.play, r.w, r.h);
    DrawShadow(rr, rad, m.shadow.play, 1 - h);
    DrawShadow(rr, rad, m.shadow.playHover, h);
    dc_->FillRoundedRectangle(D2D1::RoundedRect(rr, rad, rad), Brush(Color(m.color.play)));
    // inset 0 0 0 2px playRing
    const Rgba ring = Color(m.color.playRing);
    if (ring.a > 0.003f)
        dc_->DrawRoundedRectangle(D2D1::RoundedRect(Inflate(rr, -1), rad - 1, rad - 1), Brush(ring), 2);

    // Морфинг play/pause: два четырёхугольника, точки по прямой (как transition: d в макете)
    const float t = std::clamp(v.playMorph.Value(now), 0.f, 1.f);
    static const D2D1_POINT_2F kA0[4] = {{8.5f, 5}, {14, 8.5f}, {14, 15.5f}, {8.5f, 19}};
    static const D2D1_POINT_2F kA1[4] = {{6.5f, 5}, {10, 5}, {10, 19}, {6.5f, 19}};
    static const D2D1_POINT_2F kB0[4] = {{14, 8.5f}, {19.5f, 12}, {19.5f, 12}, {14, 15.5f}};
    static const D2D1_POINT_2F kB1[4] = {{14, 5}, {17.5f, 5}, {17.5f, 19}, {14, 19}};
    const float size = r.w * 0.42f;
    const float k = size / 24;
    PushTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(c.x - size / 2, c.y - size / 2));
    ID2D1SolidColorBrush* b = Brush(Color(m.color.onPlay));
    const std::pair<const D2D1_POINT_2F*, const D2D1_POINT_2F*> quads[2] = {{kA0, kA1}, {kB0, kB1}};
    for (const auto& [from, to] : quads) {
        ComPtr<ID2D1PathGeometry> geo;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(renderer_->Factory()->CreatePathGeometry(&geo)) || FAILED(geo->Open(&sink))) continue;
        D2D1_POINT_2F pts[4];
        for (int i = 0; i < 4; ++i)
            pts[i] = {view::Lerp(from[i].x, to[i].x, t), view::Lerp(from[i].y, to[i].y, t)};
        sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLines(pts + 1, 3);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        dc_->FillGeometry(geo.Get(), b);
        dc_->DrawGeometry(geo.Get(), b, 1.6f, roundStroke_.Get());
    }
    PopTransform();
    PopTransform();
}

void Painter::DrawControls(const view::PlayerView& v) {
    const theme::Material& m = v.theme.material;
    const float icon = m.size.icon;
    DrawButton(v, Elem::Shuffle, IconId::Shuffle, icon, &v.shuffleToggle);
    DrawButton(v, Elem::Prev, IconId::Prev, icon + 4, nullptr);
    DrawPlayButton(v, Elem::Play);
    DrawButton(v, Elem::Next, IconId::Next, icon + 4, nullptr);
    DrawButton(v, Elem::Repeat, IconId::Repeat, icon, &v.repeatToggle);
    DrawButton(v, Elem::VolBtn, IconId::Volume, icon, nullptr);
    DrawSlider(v, v.volume, v.frame[Elem::Volume], v.VolumeDisplay(), false);
    DrawButton(v, Elem::PlBtn, IconId::Playlist, icon, &v.playlistToggle);
}

// ---------------- плейлист ----------------

void Painter::DrawPlaylist(const view::PlayerView& v, const PaintInput& in) {
    const double now = v.now;
    const float alpha = std::clamp(v.panelAlpha.Value(now), 0.f, 1.f);
    if (alpha <= 0.003f) return;
    const theme::Material& m = v.theme.material;
    const float ts = v.layoutInput.textScale;
    const Rect& panel = v.frame[Elem::Panel];
    PushOpacity(alpha);
    PushTransform(D2D1::Matrix3x2F::Translation(v.panelShift.Value(now), 0));

    dc_->FillRectangle(ToD2D(panel), Brush(Color(m.color.panel)));
    dc_->FillRectangle(D2D1::RectF(panel.x, panel.y, panel.x + 1, panel.Bottom()), Brush(Color(m.color.panelBorder)));

    TextSpec heading;
    heading.size = m.font.fsHeading * ts;
    heading.weight = 600;
    heading.display = true;
    heading.upper = m.font.headingUpper;
    heading.spacing = m.font.lsHeading;
    heading.ellipsis = false;
    // Вкладки: активная - цветом текста с полоской акцента, другая - приглушённая (ярче при наведении)
    struct TabDef {
        Elem e;
        const wchar_t* text;
        int tab;
    };
    for (const TabDef& t : {TabDef{Elem::PanelTitle, L"Очередь", 0}, TabDef{Elem::PanelTab2, L"Плагины", 1}}) {
        const Rect& r = v.frame[t.e];
        const bool active = v.panelTab == t.tab;
        const float h = std::clamp(v.Button(t.e).hoverAmt.Value(now), 0.f, 1.f);
        const Rgba color = active ? text_c_ : motion::Mix(Text3(), text_c_, h * 0.6f);
        auto l = text_->Layout(t.text, heading, 0, r.h);
        if (!l) continue;
        dc_->DrawTextLayout({r.x, r.y}, l.Get(), Brush(color));
        if (active) {
            DWRITE_TEXT_METRICS tm{};
            l->GetMetrics(&tm);
            const float y = r.Bottom() + 3;
            dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.x, y, r.x + tm.width, y + 2), 1, 1), Brush(accent_));
        }
    }
    TextSpec caps;
    caps.size = m.font.fsCaps;
    caps.tabular = true;
    caps.align = DWRITE_TEXT_ALIGNMENT_TRAILING;
    const Rect& hc = v.frame[Elem::PanelCount];
    const std::wstring& countText = v.panelTab == 0 ? in.countText : in.pluginCountText;
    if (auto l = text_->Layout(countText, caps, hc.w, hc.h)) dc_->DrawTextLayout({hc.x, hc.y}, l.Get(), Brush(Text3()));

    const Rect& list = v.frame[Elem::List];
    dc_->PushAxisAlignedClip(ToD2D(list), D2D1_ANTIALIAS_MODE_ALIASED);
    // После смены вкладки список проявляется со сдвигом снизу
    const float tabA = std::clamp(v.tabAnim.Value(now), 0.f, 1.f);
    PushOpacity(tabA);
    PushTransform(D2D1::Matrix3x2F::Translation(0, (1 - tabA) * 8));
    if (v.panelTab == 1) DrawPluginRows(v, in);
    else DrawQueueRows(v, in);
    PopTransform();
    PopOpacity();
    dc_->PopAxisAlignedClip();
    PopTransform();
    PopOpacity();
}

void Painter::DrawQueueRows(const view::PlayerView& v, const PaintInput& in) {
    const double now = v.now;
    const theme::Material& m = v.theme.material;
    const float ts = v.layoutInput.textScale;
    const Rect& list = v.frame[Elem::List];
    const int count = std::min(v.listCount, in.rows ? static_cast<int>(in.rows->size()) : 0);
    if (count == 0 && !in.emptyHint.empty()) {
        TextSpec hint;
        hint.size = 13;
        hint.align = DWRITE_TEXT_ALIGNMENT_CENTER;
        if (auto l = text_->Layout(in.emptyHint, hint, list.w - 24, 40))
            dc_->DrawTextLayout({list.x + 12, list.y + 24}, l.Get(), Brush(Text3()));
    }
    const float rowH = v.frame.rowHeight;
    const int first = std::max(0, static_cast<int>(v.listScroll / rowH));
    const int last = std::min(count - 1, static_cast<int>((v.listScroll + list.h) / rowH) + 1);

    TextSpec num;
    num.size = m.font.fsRowSub;
    num.tabular = true;
    num.align = DWRITE_TEXT_ALIGNMENT_CENTER;
    num.ellipsis = false;
    TextSpec dur = num;
    dur.align = DWRITE_TEXT_ALIGNMENT_TRAILING;
    TextSpec rowTitle;
    rowTitle.size = m.font.fsRow * ts;
    rowTitle.weight = 500;
    TextSpec rowArtist;
    rowArtist.size = m.font.fsRowSub * ts;
    const float titleH = rowTitle.size * 1.3f, artistH = rowArtist.size * 1.35f;
    const float durW = 48;

    for (int i = first; i <= last; ++i) {
        const auto idx = static_cast<size_t>(i);
        const PlaylistRow& row = (*in.rows)[idx];
        const float ra = idx < v.rowAlpha.size() ? std::clamp(v.rowAlpha[idx].Value(now), 0.f, 1.f) : 1.f;
        if (ra <= 0.003f) continue;
        const float shift = idx < v.rowShift.size() ? v.rowShift[idx].Value(now) : 0.f;
        const float hov = idx < v.rowHover.size() ? std::clamp(v.rowHover[idx].Value(now), 0.f, 1.f) : 0.f;
        const bool current = i == v.snap.index;
        const Rect r = v.frame.Row(i, v.listScroll);
        PushOpacity(ra);
        PushTransform(D2D1::Matrix3x2F::Translation(shift, 0));
        const D2D1_RECT_F rr = ToD2D(r);
        const float rad = Radius(m.radius.row, r.w, r.h);
        const Rgba bg = current ? Color(m.color.rowActive) : WithAlpha(Color(m.color.rowHover), hov);
        if (bg.a > 0.003f) dc_->FillRoundedRectangle(D2D1::RoundedRect(rr, rad, rad), Brush(bg));
        const Rgba line = Color(m.color.rowLine);
        if (line.a > 0.003f) dc_->FillRectangle(D2D1::RectF(rr.left, rr.bottom - 1, rr.right, rr.bottom), Brush(line));

        // Номер или «эквалайзер» у текущего трека
        const float numX = r.x + v.frame.rowNumX, cy = r.CenterY();
        if (current) {
            // Три полоски 14x14: scaleY 0.25 <-> 1 за 900 мс туда-обратно, сдвиг фаз 300 мс.
            // Время берём из позиции трека: на паузе полоски замирают, как animation-play-state.
            const float bx = numX + v.frame.rowNumW / 2 - 7, by = cy - 7;
            auto g = Linear({0, by}, {0, by + 14}, {{0.f, accent2_}, {1.f, accent_}});
            for (int k = 0; k < 3; ++k) {
                double ph = std::fmod(v.snap.position + k * 0.3, 1.8);
                ph = ph < 0.9 ? ph / 0.9 : 2 - ph / 0.9;
                const float sy = 0.25f + 0.75f * static_cast<float>(kEaseInOut.Eval(ph));
                const float x0 = bx + k * (10.f / 3 + 2);
                const float bh = 14 * (v.motion.reduce ? 0.6f : sy);
                if (g)
                    dc_->FillRoundedRectangle(
                        D2D1::RoundedRect(D2D1::RectF(x0, by + 14 - bh, x0 + 10.f / 3, by + 14), 1, 1), g.Get());
            }
        } else {
            wchar_t n[16];
            swprintf(n, 16, L"%d", i + 1);
            if (auto l = text_->Layout(n, num, v.frame.rowNumW, 16))
                dc_->DrawTextLayout({numX, cy - 8}, l.Get(), Brush(Text3()));
        }
        // Название и исполнитель
        const float textX = r.x + v.frame.rowTextX;
        const float textW = r.Right() - v.frame.rowDurRight - durW - m.spacing.row - textX;
        // Пустая строка исполнителя в макете не занимает места - название по центру
        const float blockTop = cy - (titleH + (row.artist.empty() ? 0 : artistH)) / 2;
        TextSpec ttl = rowTitle;
        if (current) ttl.weight = 600;
        if (auto l = text_->Layout(row.title, ttl, textW, titleH))
            dc_->DrawTextLayout({textX, blockTop}, l.Get(), Brush(current ? accent_ : text_c_));
        if (auto l = text_->Layout(row.artist, rowArtist, textW, artistH))
            dc_->DrawTextLayout({textX, blockTop + titleH}, l.Get(), Brush(Text3()));
        if (auto l = text_->Layout(row.duration, dur, durW, 16))
            dc_->DrawTextLayout({r.Right() - v.frame.rowDurRight - durW, cy - 8}, l.Get(), Brush(Text3()));
        PopTransform();
        PopOpacity();
    }
}

void Painter::DrawPluginRows(const view::PlayerView& v, const PaintInput& in) {
    const double now = v.now;
    const theme::Material& m = v.theme.material;
    const float ts = v.layoutInput.textScale;
    const Rect& list = v.frame[Elem::List];
    const int count = in.plugins ? static_cast<int>(in.plugins->size()) : 0;
    TextSpec name;
    name.size = m.font.fsRow * ts;
    name.weight = 500;
    TextSpec desc;
    desc.size = m.font.fsRowSub * ts;
    const float nameH = name.size * 1.3f, descH = desc.size * 1.35f;
    const float rowH = v.frame.rowHeight;
    const int rows = count + 1;  // последняя - «Открыть папку плагинов»
    const int first = std::max(0, static_cast<int>(v.listScroll / rowH));
    const int last = std::min(rows - 1, static_cast<int>((v.listScroll + list.h) / rowH) + 1);
    for (int i = first; i <= last; ++i) {
        const auto idx = static_cast<size_t>(i);
        const Rect r = v.frame.Row(i, v.listScroll);
        const float hov = idx < v.rowHover.size() ? std::clamp(v.rowHover[idx].Value(now), 0.f, 1.f) : 0.f;
        const D2D1_RECT_F rr = ToD2D(r);
        const float rad = Radius(m.radius.row, r.w, r.h);
        const Rgba bg = WithAlpha(Color(m.color.rowHover), hov);
        if (bg.a > 0.003f) dc_->FillRoundedRectangle(D2D1::RoundedRect(rr, rad, rad), Brush(bg));
        const float textX = r.x + 14, cy = r.CenterY();
        const float toggleW = 38, toggleH = 22, toggleX = r.Right() - 12 - toggleW;
        const float textW = toggleX - 12 - textX;
        const float top = cy - (nameH + descH) / 2;
        if (i == count) {
            if (auto l = text_->Layout(L"Открыть папку плагинов", name, textW, nameH))
                dc_->DrawTextLayout({textX, top}, l.Get(), Brush(accent_));
            if (auto l = text_->Layout(L"Плагин - папка с plugin.json и DLL", desc, textW, descH))
                dc_->DrawTextLayout({textX, top + nameH}, l.Get(), Brush(Text3()));
            continue;
        }
        const PluginRow& p = (*in.plugins)[idx];
        const std::wstring title = p.version.empty() ? p.name : p.name + L"  " + p.version;
        if (auto l = text_->Layout(title, name, textW, nameH)) dc_->DrawTextLayout({textX, top}, l.Get(), Brush(text_c_));
        // Ошибка загрузки важнее описания
        const bool failed = !p.error.empty();
        const std::wstring line = failed ? L"Не загрузился: " + p.error : p.description;
        if (auto l = text_->Layout(line, desc, textW, descH))
            dc_->DrawTextLayout({textX, top + nameH}, l.Get(), Brush(failed ? Color(m.color.closeHover) : Text3()));
        // Переключатель: дорожка от цвета трека к акценту, кружок едет с пружиной
        const float on = idx < v.pluginOn.size() ? v.pluginOn[idx].Value(now) : 0.f;
        const float onC = std::clamp(on, 0.f, 1.f);
        const D2D1_RECT_F tr = D2D1::RectF(toggleX, cy - toggleH / 2, toggleX + toggleW, cy + toggleH / 2);
        dc_->FillRoundedRectangle(D2D1::RoundedRect(tr, toggleH / 2, toggleH / 2),
                                  Brush(motion::Mix(Color(m.color.track), accent_, onC)));
        const float kx = toggleX + 3 + 8 + 16 * on;
        // Кружок включённого - цветом значка на кнопке «пуск»: виден на любом акценте
        dc_->FillEllipse(D2D1::Ellipse({kx, cy}, 8, 8), Brush(motion::Mix(Rgba{1, 1, 1, 1}, Color(m.color.onPlay), onC)));
    }
    if (count == 0 && !in.pluginsHint.empty()) {
        TextSpec hint;
        hint.size = 13;
        hint.align = DWRITE_TEXT_ALIGNMENT_CENTER;
        const float y = list.y + rowH + 20 - v.listScroll;
        if (auto l = text_->Layout(in.pluginsHint, hint, list.w - 24, 60))
            dc_->DrawTextLayout({list.x + 12, y}, l.Get(), Brush(Text3()));
    }
}

// ---------------- мини-режим ----------------

void Painter::DrawMini(const view::PlayerView& v) {
    const theme::Material& m = v.theme.material;
    const float ts = v.layoutInput.textScale;
    const Rect& cover = v.frame[Elem::MiniCover];
    const std::string& shape = v.theme.settings.coverShape;
    const float r = shape == "square" ? 2.f : shape == "rounded" ? 10.f : cover.w / 2;
    DrawCover(v, cover, r, false);

    DrawLine(::text::Utf8ToWide(v.miniTitle.shown), MiniTitleSpec(v), v.frame[Elem::MiniTitle], text_c_, v.miniTitle,
             &v.miniMarquee, v.now);
    TextSpec artist;
    artist.size = 12;
    artist.spacing = m.font.lsArtist;
    artist.upper = m.font.artistUpper;
    DrawLine(::text::Utf8ToWide(v.miniArtist.shown), artist, v.frame[Elem::MiniArtist], Text2(), v.miniArtist, nullptr,
             v.now);
    (void)ts;

    DrawPlayButton(v, Elem::MiniPlay);
    DrawButton(v, Elem::MiniExpand, IconId::Expand, m.size.icon, nullptr);
    DrawSlider(v, v.miniSeek, v.frame[Elem::MiniSeek], v.SeekDisplay(), true);
}

}  // namespace ui
