#pragma once
// Пружина: масса 1, жёсткость k, затухание c. Движение из 0 в 1 без начальной скорости.
// Формула и порог успокоения совпадают с функцией springCurve в mockup/index.html,
// эталоны - в mockup/ANIMATIONS.md (раздел «Эталоны для unit-теста пружины»).

namespace motion {

struct SpringParams {
    double stiffness = 420;
    double damping = 26;
};

// Положение пружины в момент t (секунды). x(0) = 0, x(бесконечность) = 1.
double SpringValue(const SpringParams& p, double t);

// Время успокоения, секунды: последний момент, когда |1 - x(t)| > 0.005,
// плюс шаг поиска 4 мс; не больше 2 с. Как в макете.
double SpringSettleTime(const SpringParams& p);

// Наибольшее значение x(t) и момент, когда оно достигается (поиск шагом 0.5 мс до 1.5 с)
struct SpringPeak {
    double value = 1;
    double time = 0;
};
SpringPeak FindSpringPeak(const SpringParams& p);

}  // namespace motion
