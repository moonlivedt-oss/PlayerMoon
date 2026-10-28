#pragma once
// Модель визуализатора: сглаживание полос как в макете (рост к цели со скоростью attack,
// линейное падение со скоростью fall). Цели - полосы спектра из FFT (pm::SpectrumAnalyzer).
// На паузе полосы опускаются ускоренно, и при max < 0.001 модель сообщает, что кадры не нужны.

#include <vector>

namespace view {

class VisualizerModel {
public:
    void SetBars(int count);
    int Bars() const { return static_cast<int>(values_.size()); }
    const std::vector<float>& Values() const { return values_; }

    // targets - полосы спектра 0..1 (пустой вектор - тишина/пауза). Возвращает true, пока что-то движется.
    bool Step(double dt, const std::vector<float>& targets, bool playing, float attack, float fall);

private:
    std::vector<float> values_;
};

}  // namespace view
