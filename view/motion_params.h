#pragma once
// Параметры анимаций текущей темы: длительности с множителем speed, кривые, пружина,
// реакция кнопок. Сокращённый режим (характер «Без анимаций» или системная настройка
// Windows «Эффекты анимации» выключена) - как :root.reduce в макете.

#include "motion/easing.h"
#include "theme/theme.h"

namespace view {

struct MotionParams {
    // Секунды, уже умноженные на speed
    double fast = 0.15, base = 0.22, slow = 0.35, cover = 0.35, text = 0.26, bg = 0.35, resize = 0.32, press = 0.09,
           stagger = 0.022, spring = 0.428;
    motion::Easing out, in, springCurve, linear;
    // Реакция кнопок (btnFx): масштаб при наведении и нажатии, подъём при наведении (DIP)
    float hoverScale = 1.08f, pressScale = 0.9f, hoverLift = 0;
    bool glowButtons = false;
    bool liftShadow = false;
    float thumbIdle = 1, thumbHover = 1.45f;
    bool reduce = false;
};

MotionParams MakeMotionParams(const theme::Theme& t, bool systemReduce);

}  // namespace view
