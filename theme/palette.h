#pragma once
// Заглушка обложки и цвета фона из названия трека.
// Точный перенос palette() из mockup/index.html: хеш FNV-1a по кодовым точкам,
// генератор mulberry32, цвета HSL с округлением как toFixed(0) в JS.
// Один и тот же трек получает одинаковые цвета в макете и в плеере.

#include <cstdint>
#include <string_view>

#include "motion/tween.h"

namespace theme {

uint32_t Fnv1a(std::string_view utf8);

// Генератор mulberry32: числа в [0, 1)
class Mulberry32 {
public:
    explicit Mulberry32(uint32_t seed) : state_(seed) {}
    double Next();

private:
    uint32_t state_;
};

struct CoverPalette {
    motion::Rgba a, b, c;        // основные цвета обложки
    motion::Rgba bg1, bg2, bg3;  // приглушённые цвета для фона окна (--c1, --c2, --c3)
    int h3 = 0;                  // оттенок третьего цвета (для акцента из обложки)
    int angle = 0;               // угол основного градиента, градусы
    int x1 = 50, y1 = 50;        // центр диска, %
    int x2 = 50, y2 = 50;        // центр мягкого пятна, %
    int disc = 20;               // радиус диска, % от диагонали (как circle в CSS)
    int shape = 0;               // 0 - диск, 1 - горизонт с солнцем, 2 - полосы
};

CoverPalette MakeCoverPalette(std::string_view title);

// CSS hsl(h s% l%) -> RGB
motion::Rgba Hsl(double h, double s, double l, float alpha = 1);

// Палитра по пикселям настоящей обложки (уменьшенная картинка, BGRA, 8 бит).
// Средний цвет даёт bg1, самый насыщенный - bg2, bg3 и оттенок h3; остальные поля - из хеша
// названия (они нужны только заглушке, у которой нет картинки).
CoverPalette PaletteFromPixels(const uint8_t* bgra, int pixelCount, std::string_view title);

// Акцент из обложки: светлый для тёмных тем, тёмный для светлых (coverAccent в макете)
motion::Rgba CoverAccent(const CoverPalette& p, bool darkUi);

}  // namespace theme
