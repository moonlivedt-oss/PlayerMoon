#pragma once
// Кривые анимации: линейная, cubic-bezier (как в CSS) и пружина, растянутая на длительность.

#include "motion/spring.h"

namespace motion {

// Кривая cubic-bezier(x1, y1, x2, y2), как в CSS: x - время, y - прогресс
struct CubicBezier {
    double x1 = 0, y1 = 0, x2 = 1, y2 = 1;
    // Прогресс для доли времени u в [0, 1]
    double Eval(double u) const;
};

struct Easing {
    enum class Kind { Linear, Bezier, Spring };
    Kind kind = Kind::Linear;
    CubicBezier bezier;
    SpringParams spring;
    double springSettle = 0.4;  // кэш SpringSettleTime(spring)

    static Easing Linear() { return {}; }
    static Easing Bezier(double x1, double y1, double x2, double y2);
    // Пружина как кривая: за время анимации проходит путь от 0 до времени успокоения.
    // Так же устроена выборка linear(...) в макете.
    static Easing Spring(const SpringParams& p);

    // Прогресс для доли времени u в [0, 1]; у пружины может выходить за 1 (перелёт)
    double Eval(double u) const;
};

}  // namespace motion
