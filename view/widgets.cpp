#include "view/widgets.h"

namespace view {

void ButtonState::Retarget(const MotionParams& p, motion::Timeline& tl, bool pressEvent) {
    const float target = pressed ? p.pressScale : hover ? p.hoverScale : 1.f;
    // Нажатие - быстро (duration.press), отпускание и наведение - пружиной с перелётом
    if (pressEvent && pressed) scale.To(target, p.press, p.springCurve, tl);
    else scale.To(target, p.spring, p.springCurve, tl);
    lift.To(hover && !pressed ? p.hoverLift : 0.f, p.spring, p.springCurve, tl);
}

void ButtonState::SetHover(bool on, const MotionParams& p, motion::Timeline& tl) {
    if (hover == on) return;
    hover = on;
    if (!on) pressed = false;
    hoverAmt.To(on ? 1.f : 0.f, p.fast, p.out, tl);
    pressAmt.To(pressed ? 1.f : 0.f, p.fast, p.out, tl);
    Retarget(p, tl, false);
}

void ButtonState::SetPressed(bool on, const MotionParams& p, motion::Timeline& tl) {
    if (pressed == on) return;
    pressed = on;
    pressAmt.To(on ? 1.f : 0.f, on ? p.press : p.fast, p.out, tl);
    Retarget(p, tl, true);
}

void ToggleState::Set(bool value, bool showOne, const MotionParams& p, motion::Timeline& tl) {
    on = value;
    dot.To(value ? 1.f : 0.f, p.spring, p.springCurve, tl);
    one.To(showOne ? 1.f : 0.f, p.spring, p.springCurve, tl);
}

void SliderState::SetHover(bool on, const MotionParams& p, motion::Timeline& tl) {
    if (hover == on) return;
    hover = on;
    const bool big = hover || dragging;
    thumb.To(big ? hoverScale : idleScale, p.spring, p.springCurve, tl);
    track.To(big ? 1.f : 0.f, p.fast, p.out, tl);
}

void SliderState::JumpTo(float from, float to, const MotionParams& p, motion::Timeline& tl) {
    jump.Snap(from);
    jump.To(to, p.base, p.out, tl);
    jumpUntil = tl.Now() + p.base;
}

float SliderState::Display(float live, double now) const {
    if (dragging) return dragValue;
    if (now < jumpUntil) return jump.Value(now);
    return live;
}

}  // namespace view
