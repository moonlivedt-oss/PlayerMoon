#pragma once
// Состояния виджетов с анимацией: кнопка (наведение, нажатие, пружина), ползунок (ручка,
// дорожка, «доезд» при клике), переключатель (точка и цифра повтора). Только данные и
// твины - рисует их окно.

#include "motion/tween.h"
#include "view/motion_params.h"

namespace view {

struct ButtonState {
    bool hover = false;
    bool pressed = false;
    motion::Tween hoverAmt{0};  // 0..1: цвет фона и иконки btn -> btnHover
    motion::Tween pressAmt{0};  // 0..1: -> btnPress
    motion::Tween scale{1};
    motion::Tween lift{0};

    void SetHover(bool on, const MotionParams& p, motion::Timeline& tl);
    void SetPressed(bool on, const MotionParams& p, motion::Timeline& tl);

private:
    void Retarget(const MotionParams& p, motion::Timeline& tl, bool pressEvent);
};

struct ToggleState {
    bool on = false;
    motion::Tween dot{0};  // масштаб точки под иконкой
    motion::Tween one{0};  // цифра «1» у повтора

    void Set(bool value, bool showOne, const MotionParams& p, motion::Timeline& tl);
};

struct SliderState {
    bool hover = false;
    bool dragging = false;
    float dragValue = 0;      // значение под мышью во время перетаскивания
    motion::Tween thumb{1};   // масштаб ручки
    float idleScale = 1;      // ручка в покое (у мини-прогресса 0 - не видна)
    float hoverScale = 1.45f; // ручка при наведении
    motion::Tween track{0};   // 0..1: толщина дорожки track -> trackHover
    motion::Tween jump{0};    // плавный «доезд» к точке клика
    double jumpUntil = -1;    // пока идёт доезд, показываем jump, а не живое значение

    void Init(float idle, float hovered) {
        idleScale = idle;
        hoverScale = hovered;
        thumb.Snap(hover || dragging ? hoverScale : idleScale);
    }
    void SetHover(bool on, const MotionParams& p, motion::Timeline& tl);
    // Клик по дорожке: показ плавно доезжает от from до to
    void JumpTo(float from, float to, const MotionParams& p, motion::Timeline& tl);
    // Значение для показа: перетаскивание > доезд > живое значение
    float Display(float live, double now) const;
};

}  // namespace view
