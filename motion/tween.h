#pragma once
// Твины и общий таймлайн окна.
//
// Таймлайн - единственные часы анимаций. Каждый запущенный твин продлевает
// «занятость» таймлайна до своего конца. Окно запрашивает следующий кадр,
// только пока таймлайн занят (или идёт что-то непрерывное: воспроизведение
// с видимым визуализатором, бегущая строка). Иначе кадров нет вообще.

#include "motion/easing.h"

namespace motion {

class Timeline {
public:
    double Now() const { return now_; }
    void SetNow(double seconds) { now_ = seconds; }
    // Анимация идёт до момента t: держать кадры до него
    void KeepBusyUntil(double t) {
        if (t > busyUntil_) busyUntil_ = t;
    }
    bool Busy() const { return now_ < busyUntil_; }

private:
    double now_ = 0;
    double busyUntil_ = 0;
};

// Анимируемое число. Новая цель всегда стартует от текущего значения,
// как CSS-переход, прерванный на середине.
class Tween {
public:
    Tween() = default;
    explicit Tween(float v) : from_(v), to_(v) {}

    // Мгновенно поставить значение без анимации
    void Snap(float v);
    // Плавно к цели за duration секунд по кривой easing, начиная с момента now (с задержкой delay)
    void To(float target, double duration, const Easing& easing, Timeline& timeline, double delay = 0);
    float Value(double now) const;
    float Target() const { return to_; }
    bool Active(double now) const { return now < start_ + duration_; }

private:
    float from_ = 0;
    float to_ = 0;
    double start_ = 0;
    double duration_ = 0;
    Easing easing_;
};

// Цвет RGBA (0..1, без умножения на альфу) - для перетекания фона и акцента
struct Rgba {
    float r = 0, g = 0, b = 0, a = 1;
};
Rgba Mix(const Rgba& x, const Rgba& y, float t);
bool operator==(const Rgba& x, const Rgba& y);

class ColorTween {
public:
    void Snap(const Rgba& c);
    void To(const Rgba& target, double duration, const Easing& easing, Timeline& timeline);
    Rgba Value(double now) const;
    const Rgba& Target() const { return to_; }

private:
    Rgba from_, to_;
    double start_ = 0;
    double duration_ = 0;
    Easing easing_;
};

}  // namespace motion
