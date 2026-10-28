#include "motion/tween.h"

namespace motion {

void Tween::Snap(float v) {
    from_ = to_ = v;
    duration_ = 0;
}

void Tween::To(float target, double duration, const Easing& easing, Timeline& timeline, double delay) {
    const double now = timeline.Now();
    if (target == to_ && Active(now)) return;  // уже едем туда же
    from_ = Value(now);
    to_ = target;
    easing_ = easing;
    start_ = now + delay;
    duration_ = duration > 0 ? duration : 0;
    if (from_ == to_ && delay <= 0) duration_ = 0;
    timeline.KeepBusyUntil(start_ + duration_);
}

float Tween::Value(double now) const {
    if (duration_ <= 0 || now >= start_ + duration_) return to_;
    if (now <= start_) return from_;
    const double p = easing_.Eval((now - start_) / duration_);
    return static_cast<float>(from_ + (to_ - from_) * p);
}

Rgba Mix(const Rgba& x, const Rgba& y, float t) {
    return {x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t, x.a + (y.a - x.a) * t};
}

bool operator==(const Rgba& x, const Rgba& y) { return x.r == y.r && x.g == y.g && x.b == y.b && x.a == y.a; }

void ColorTween::Snap(const Rgba& c) {
    from_ = to_ = c;
    duration_ = 0;
}

void ColorTween::To(const Rgba& target, double duration, const Easing& easing, Timeline& timeline) {
    const double now = timeline.Now();
    if (target == to_) return;
    from_ = Value(now);
    to_ = target;
    easing_ = easing;
    start_ = now;
    duration_ = duration > 0 ? duration : 0;
    timeline.KeepBusyUntil(start_ + duration_);
}

Rgba ColorTween::Value(double now) const {
    if (duration_ <= 0 || now >= start_ + duration_) return to_;
    const double p = easing_.Eval((now - start_) / duration_);
    return Mix(from_, to_, static_cast<float>(p));
}

}  // namespace motion
