#include "theme/palette.h"

#include <algorithm>
#include <cmath>

#include "core/text.h"

namespace theme {

namespace {

// Округление как Number.prototype.toFixed(0): половины - от нуля
int Round0(double x) { return static_cast<int>(std::floor(x + 0.5)); }

}  // namespace

uint32_t Fnv1a(std::string_view utf8) {
    uint32_t h = 0x811c9dc5u;
    for (char32_t cp : text::CodePoints(utf8)) {
        h ^= static_cast<uint32_t>(cp);
        h *= 0x01000193u;  // Math.imul с беззнаковым результатом
    }
    return h;
}

double Mulberry32::Next() {
    state_ += 0x6d2b79f5u;
    uint32_t t = state_;
    t = (t ^ (t >> 15)) * (t | 1u);
    t ^= t + (t ^ (t >> 7)) * (t | 61u);
    return static_cast<double>(t ^ (t >> 14)) / 4294967296.0;
}

motion::Rgba Hsl(double h, double s, double l, float alpha) {
    // Формула из спецификации CSS Color 4
    h = std::fmod(std::fmod(h, 360.0) + 360.0, 360.0);
    s /= 100.0;
    l /= 100.0;
    auto f = [&](double n) {
        const double k = std::fmod(n + h / 30.0, 12.0);
        const double a = s * std::min(l, 1 - l);
        return l - a * std::max(-1.0, std::min({k - 3, 9 - k, 1.0}));
    };
    return {static_cast<float>(f(0)), static_cast<float>(f(8)), static_cast<float>(f(4)), alpha};
}

CoverPalette MakeCoverPalette(std::string_view title) {
    Mulberry32 r(Fnv1a(title));
    // Порядок вызовов r.Next() совпадает с макетом - иначе цвета разойдутся
    const double h1 = r.Next() * 360;
    const double h2 = std::fmod(h1 + 35 + r.Next() * 80, 360.0);
    const double h3 = std::fmod(h1 + 150 + r.Next() * 70, 360.0);
    const double s = 62 + r.Next() * 24;
    CoverPalette p;
    p.h3 = Round0(h3);
    p.a = Hsl(Round0(h1), Round0(s), Round0(46 + r.Next() * 12));
    p.b = Hsl(Round0(h2), Round0(s - 6), Round0(30 + r.Next() * 14));
    p.c = Hsl(Round0(h3), Round0(s + 6), Round0(62 + r.Next() * 12));
    p.bg1 = Hsl(Round0(h1), Round0(s * 0.8), 40);
    p.bg2 = Hsl(Round0(h2), Round0(s * 0.7), 30);
    p.bg3 = Hsl(Round0(h3), Round0(s), 55);
    p.angle = Round0(r.Next() * 360);
    p.x1 = Round0(15 + r.Next() * 70);
    p.y1 = Round0(15 + r.Next() * 55);
    p.x2 = Round0(r.Next() * 100);
    p.y2 = Round0(r.Next() * 100);
    p.disc = Round0(14 + r.Next() * 14);
    p.shape = static_cast<int>(std::floor(r.Next() * 3));
    return p;
}

motion::Rgba CoverAccent(const CoverPalette& p, bool darkUi) {
    return darkUi ? Hsl(p.h3, 90, 66) : Hsl(p.h3, 72, 40);
}

}  // namespace theme
