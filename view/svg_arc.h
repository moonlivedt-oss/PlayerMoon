#pragma once
// Дуга SVG (команда A) -> кубические кривые Безье. Перевод по SVG 1.1, приложение F.6:
// концы дуги -> центр и углы, затем куски не больше 90 градусов, каждый - одна кубическая кривая.
// Свой перевод вместо ID2D1GeometrySink::AddArc: одинаковый результат на любой реализации D2D
// и проверяется тестами без Windows.

#include <array>
#include <vector>

#include "view/geometry.h"

namespace view {

// Кривая: две управляющие точки и конец (начало - конец предыдущей)
using Cubic = std::array<Point, 3>;

std::vector<Cubic> ArcToCubics(Point from, float rx, float ry, float rotationDeg, bool largeArc, bool sweep,
                               Point to);

}  // namespace view
