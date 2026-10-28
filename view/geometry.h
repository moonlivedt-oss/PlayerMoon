#pragma once
// Простая геометрия в DIP (1/96 дюйма). Переводом в физические пиксели занимается окно.

#include <algorithm>

namespace view {

struct Point {
    float x = 0, y = 0;
};

struct Size {
    float w = 0, h = 0;
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;

    float Right() const { return x + w; }
    float Bottom() const { return y + h; }
    float CenterX() const { return x + w / 2; }
    float CenterY() const { return y + h / 2; }
    bool Contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    bool Empty() const { return w <= 0 || h <= 0; }
    Rect Inflate(float d) const { return {x - d, y - d, w + 2 * d, h + 2 * d}; }
    Rect Offset(float dx, float dy) const { return {x + dx, y + dy, w, h}; }
};

inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float Clamp01(float v) { return std::clamp(v, 0.f, 1.f); }

}  // namespace view
