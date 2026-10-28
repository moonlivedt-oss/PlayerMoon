#include "view/visualizer.h"

#include <algorithm>

namespace view {

void VisualizerModel::SetBars(int count) {
    count = std::clamp(count, 1, 256);
    if (static_cast<int>(values_.size()) == count) return;
    // Новое число полос: старые значения растягиваются, чтобы не было скачка
    std::vector<float> fresh(static_cast<size_t>(count));
    for (size_t i = 0; i < fresh.size(); ++i)
        fresh[i] = values_.empty() ? 0.f : values_[i * values_.size() / fresh.size()];
    values_ = std::move(fresh);
}

bool VisualizerModel::Step(double dt, const std::vector<float>& targets, bool playing, float attack, float fall) {
    float maxValue = 0;
    const float fdt = static_cast<float>(dt);
    for (size_t i = 0; i < values_.size(); ++i) {
        float& v = values_[i];
        if (playing && !targets.empty()) {
            const float t = std::clamp(targets[i * targets.size() / values_.size()], 0.f, 1.f);
            if (t > v) v += (t - v) * std::min(1.f, fdt * attack);
            else v = std::max(t, v - fdt * fall);
        } else {
            // Пауза: падение ускоряется для высоких полос (fall * (0.6 + v))
            v = std::max(0.f, v - fdt * fall * (0.6f + v));
        }
        maxValue = std::max(maxValue, v);
    }
    if (!playing && maxValue <= 0.001f) {
        std::fill(values_.begin(), values_.end(), 0.f);
        return false;
    }
    return true;
}

}  // namespace view
