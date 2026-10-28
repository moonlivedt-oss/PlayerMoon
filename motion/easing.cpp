#include "motion/easing.h"

#include <algorithm>
#include <cmath>

namespace motion {

namespace {

// Координата кубической кривой Безье с концами 0 и 1 и опорами a, b
double BezierCoord(double t, double a, double b) {
    const double s = 1 - t;
    return 3 * a * t * s * s + 3 * b * t * t * s + t * t * t;
}

double BezierSlope(double t, double a, double b) {
    const double s = 1 - t;
    return 3 * a * s * s + 6 * (b - a) * t * s + 3 * (1 - b) * t * t;
}

}  // namespace

double CubicBezier::Eval(double u) const {
    if (u <= 0) return 0;
    if (u >= 1) return 1;
    // Находим параметр t, при котором x(t) = u: сначала Ньютон, при плохой производной - деление пополам
    double t = u;
    for (int i = 0; i < 8; ++i) {
        const double err = BezierCoord(t, x1, x2) - u;
        if (std::abs(err) < 1e-7) return BezierCoord(t, y1, y2);
        const double d = BezierSlope(t, x1, x2);
        if (std::abs(d) < 1e-6) break;
        t -= err / d;
    }
    double lo = 0, hi = 1;
    t = u;
    for (int i = 0; i < 40; ++i) {
        const double x = BezierCoord(t, x1, x2);
        if (std::abs(x - u) < 1e-7) break;
        if (x < u) lo = t; else hi = t;
        t = (lo + hi) / 2;
    }
    return BezierCoord(t, y1, y2);
}

Easing Easing::Bezier(double x1, double y1, double x2, double y2) {
    Easing e;
    e.kind = Kind::Bezier;
    e.bezier = {x1, y1, x2, y2};
    return e;
}

Easing Easing::Spring(const SpringParams& p) {
    Easing e;
    e.kind = Kind::Spring;
    e.spring = p;
    e.springSettle = SpringSettleTime(p);
    return e;
}

double Easing::Eval(double u) const {
    u = std::clamp(u, 0.0, 1.0);
    switch (kind) {
        case Kind::Linear: return u;
        case Kind::Bezier: return bezier.Eval(u);
        case Kind::Spring: return u >= 1 ? 1 : SpringValue(spring, u * springSettle);
    }
    return u;
}

}  // namespace motion
