#include "view/svg_arc.h"

#include <algorithm>
#include <cmath>

namespace view {

std::vector<Cubic> ArcToCubics(Point from, float rx, float ry, float rotationDeg, bool largeArc, bool sweep,
                               Point to) {
    std::vector<Cubic> out;
    if (from.x == to.x && from.y == to.y) return out;  // нулевая дуга не рисуется
    double a = std::fabs(rx), b = std::fabs(ry);
    if (a == 0 || b == 0) {
        // Нулевой радиус - отрезок
        out.push_back({from, to, to});
        return out;
    }
    const double kPi = 3.14159265358979323846;
    const double phi = rotationDeg * kPi / 180;
    const double c = std::cos(phi), s = std::sin(phi);
    // Шаг 1: середина хорды в повёрнутой системе
    const double dx = (from.x - to.x) / 2.0, dy = (from.y - to.y) / 2.0;
    const double x1 = c * dx + s * dy, y1 = -s * dx + c * dy;
    // Слишком маленькие радиусы увеличиваются до минимально возможных
    const double lambda = (x1 * x1) / (a * a) + (y1 * y1) / (b * b);
    if (lambda > 1) {
        a *= std::sqrt(lambda);
        b *= std::sqrt(lambda);
    }
    // Шаг 2: центр
    const double num = a * a * b * b - a * a * y1 * y1 - b * b * x1 * x1;
    const double den = a * a * y1 * y1 + b * b * x1 * x1;
    double k = den > 0 ? std::sqrt(std::max(0.0, num / den)) : 0;
    if (largeArc == sweep) k = -k;
    const double cx1 = k * a * y1 / b, cy1 = -k * b * x1 / a;
    const double cx = c * cx1 - s * cy1 + (from.x + to.x) / 2.0;
    const double cy = s * cx1 + c * cy1 + (from.y + to.y) / 2.0;
    // Шаг 3: начальный угол и размах
    auto angle = [](double ux, double uy, double vx, double vy) { return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy); };
    const double t1 = angle(1, 0, (x1 - cx1) / a, (y1 - cy1) / b);
    double dt = angle((x1 - cx1) / a, (y1 - cy1) / b, (-x1 - cx1) / a, (-y1 - cy1) / b);
    if (!sweep && dt > 0) dt -= 2 * kPi;
    if (sweep && dt < 0) dt += 2 * kPi;

    // Куски по 90 градусов и меньше; длина ручек 4/3 * tan(d/4)
    const int parts = std::max(1, static_cast<int>(std::ceil(std::fabs(dt) / (kPi / 2) - 1e-9)));
    const double step = dt / parts;
    const double h = 4.0 / 3.0 * std::tan(step / 4);
    auto point = [&](double t, double ox, double oy) {
        // Точка эллипса со смещением (ox, oy) в системе эллипса
        const double ex = a * std::cos(t) + ox, ey = b * std::sin(t) + oy;
        return Point{static_cast<float>(c * ex - s * ey + cx), static_cast<float>(s * ex + c * ey + cy)};
    };
    double t = t1;
    for (int i = 0; i < parts; ++i) {
        const double t2 = t + step;
        // Ручки - вдоль касательной (-a sin t, b cos t)
        const Point p1 = point(t, -h * a * std::sin(t), h * b * std::cos(t));
        const Point p2 = point(t2, h * a * std::sin(t2), -h * b * std::cos(t2));
        const Point p3 = i == parts - 1 ? to : point(t2, 0, 0);
        out.push_back({p1, p2, p3});
        t = t2;
    }
    return out;
}

}  // namespace view
