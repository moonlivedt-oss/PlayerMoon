#include "view/marquee.h"

#include <algorithm>
#include <cmath>

namespace view {

void Marquee::Measure(float textWidth, float boxWidth, float gap, double pause, bool reduce) {
    offset_ = 0;
    pause_ = pause;
    hold_ = pause;
    // В сокращённом режиме строка не бежит: длинное название обрезается многоточием
    active_ = !reduce && boxWidth > 0 && textWidth > boxWidth + 1;
    distance_ = textWidth + gap;
}

bool Marquee::Step(double dt, bool playing, float speed) {
    if (!active_) return false;
    if (!playing && offset_ == 0) return false;
    if (playing && hold_ > 0) {
        hold_ -= dt;
        return true;
    }
    if (playing) {
        offset_ += static_cast<float>(speed * dt);
    } else {
        // Докрутка до шва: шаг = max(speed*dt, (dist - off) * min(1, 6*dt)) - как в макете
        const float rest = distance_ - offset_;
        offset_ += std::max(static_cast<float>(speed * dt), rest * static_cast<float>(std::min(1.0, 6 * dt)));
    }
    if (offset_ >= distance_ - 0.5f) {
        offset_ = 0;
        hold_ = pause_;
    }
    return playing || offset_ != 0;
}

}  // namespace view
