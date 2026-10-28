#pragma once
// Векторные иконки: пути SVG из mockup/index.html, разобранные в ID2D1PathGeometry.
// Каждая иконка - набор слоёв: обводка (stroke) или заливка (solid), в сетке 24x24
// (кнопки заголовка - 16x16). Слои с ролью (волна громкости, крестик, цифра повтора)
// рисуются отдельно, чтобы их можно было анимировать.

#include <d2d1_1.h>
#include <wrl/client.h>

#include <string_view>
#include <vector>

namespace ui {

using Microsoft::WRL::ComPtr;

enum class IconId {
    Shuffle,
    Prev,
    Next,
    Repeat,
    Volume,
    Playlist,
    Expand,
    Settings,
    MiniMode,
    Minimize,
    Close,
    Moon,
    Count
};

// Роль слоя: обычный или анимируемая часть
enum class IconPart { Base, Wave1, Wave2, Mute, RepeatOne };

struct IconLayer {
    ComPtr<ID2D1PathGeometry> geometry;
    bool solid = false;  // заливка вместо обводки
    IconPart part = IconPart::Base;
};

struct Icon {
    float viewBox = 24;  // сторона сетки
    std::vector<IconLayer> layers;
};

class IconSet {
public:
    bool Init(ID2D1Factory1* factory);
    const Icon& Get(IconId id) const { return icons_[static_cast<size_t>(id)]; }

private:
    std::vector<Icon> icons_;
};

// Разбор атрибута d из SVG: M L H V C S Q T A Z в абсолютной и относительной форме
ComPtr<ID2D1PathGeometry> ParseSvgPath(ID2D1Factory1* factory, std::string_view d);

}  // namespace ui
