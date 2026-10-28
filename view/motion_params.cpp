#include "view/motion_params.h"

namespace view {

MotionParams MakeMotionParams(const theme::Theme& t, bool systemReduce) {
    MotionParams p;
    const theme::Settings& s = t.settings;
    const theme::Material& m = t.material;
    p.reduce = systemReduce || s.motion == "none";
    const double speed = p.reduce ? 0.35 : s.speed;
    p.fast = m.duration.fast * speed;
    p.base = m.duration.base * speed;
    p.slow = m.duration.slow * speed;
    p.cover = m.duration.cover * speed;
    p.text = m.duration.text * speed;
    p.bg = m.duration.bg * speed;
    p.resize = p.reduce ? 0 : m.duration.resize * speed;
    p.press = m.duration.press * speed;
    p.stagger = p.reduce ? 0 : m.duration.stagger * speed;
    p.out = motion::Easing::Bezier(m.easeOut.x1, m.easeOut.y1, m.easeOut.x2, m.easeOut.y2);
    p.in = motion::Easing::Bezier(m.easeIn.x1, m.easeIn.y1, m.easeIn.x2, m.easeIn.y2);
    p.linear = motion::Easing::Linear();
    if (p.reduce) {
        // Пружина мгновенная: linear(0, 1) за 80 мс, как в макете
        p.springCurve = motion::Easing::Linear();
        p.spring = 0.08;
    } else {
        p.springCurve = motion::Easing::Spring({s.stiffness, s.damping});
        p.spring = p.springCurve.springSettle * s.speed;
    }
    // Реакция кнопок по btnFx (значения из CSS макета)
    const std::string& fx = p.reduce ? std::string("none") : s.btnFx;
    if (fx == "spring") { p.hoverScale = 1.08f; p.pressScale = 0.9f; p.hoverLift = 0; }
    else if (fx == "press") { p.hoverScale = 1; p.pressScale = 0.9f; p.hoverLift = 0; }
    else if (fx == "lift") { p.hoverScale = 1; p.pressScale = 0.96f; p.hoverLift = -3; p.liftShadow = true; }
    else if (fx == "glow") { p.hoverScale = 1.04f; p.pressScale = 0.94f; p.hoverLift = 0; p.glowButtons = true; }
    else { p.hoverScale = 1; p.pressScale = 1; p.hoverLift = 0; }
    p.thumbIdle = m.effect.thumbIdle;
    p.thumbHover = m.effect.thumbHover;
    return p;
}

}  // namespace view
