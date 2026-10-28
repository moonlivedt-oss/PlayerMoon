#include "motion/spring.h"

#include <algorithm>
#include <cmath>

namespace motion {

double SpringValue(const SpringParams& p, double t) {
    if (t <= 0) return 0;
    const double w = std::sqrt(std::max(p.stiffness, 1e-9));
    const double z = p.damping / (2 * w);
    if (z < 1 - 1e-6) {
        // Недодемпфированная пружина: затухающие колебания с перелётом
        const double wd = w * std::sqrt(1 - z * z);
        return 1 - std::exp(-z * w * t) * (std::cos(wd * t) + (z * w / wd) * std::sin(wd * t));
    }
    if (z <= 1 + 1e-6) {
        // Критическое затухание: самый быстрый подход без перелёта
        return 1 - std::exp(-w * t) * (1 + w * t);
    }
    // Передемпфированная: два экспоненциальных слагаемых
    const double q = std::sqrt(z * z - 1);
    const double r1 = -w * (z - q);
    const double r2 = -w * (z + q);
    return 1 - (r2 * std::exp(r1 * t) - r1 * std::exp(r2 * t)) / (r2 - r1);
}

double SpringSettleTime(const SpringParams& p) {
    // Шаг 4 мс и порог 0.5% взяты из макета, чтобы длительности совпали один в один
    double settle = 0.05;
    for (int i = 0; i < 750; ++i) {
        const double t = i * 0.004;
        if (std::abs(1 - SpringValue(p, t)) > 0.005) settle = t;
    }
    return std::min(2.0, settle + 0.004);
}

SpringPeak FindSpringPeak(const SpringParams& p) {
    SpringPeak peak{0, 0};
    for (int i = 0; i <= 3000; ++i) {
        const double t = i * 0.0005;
        const double v = SpringValue(p, t);
        if (v > peak.value) peak = {v, t};
    }
    return peak;
}

}  // namespace motion
