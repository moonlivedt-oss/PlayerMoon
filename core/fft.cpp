#include "core/fft.h"

#include <algorithm>
#include <cmath>

namespace pm {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

void Fft(std::vector<std::complex<float>>& a) {
    const size_t n = a.size();
    // Перестановка с обращением бит
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    // Бабочки Кули-Тьюки
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * kPi / static_cast<double>(len);
        const std::complex<float> wl(static_cast<float>(std::cos(ang)), static_cast<float>(std::sin(ang)));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1, 0);
            for (size_t k = 0; k < len / 2; ++k) {
                const std::complex<float> u = a[i + k];
                const std::complex<float> v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

SpectrumAnalyzer::SpectrumAnalyzer(size_t fftSize) : size_(fftSize), window_(fftSize), work_(fftSize) {
    for (size_t i = 0; i < size_; ++i)
        window_[i] = static_cast<float>(0.5 - 0.5 * std::cos(2 * kPi * static_cast<double>(i) / (size_ - 1)));
}

void SpectrumAnalyzer::Compute(const float* samples, double sampleRate, int bands, std::vector<float>& out) {
    for (size_t i = 0; i < size_; ++i) work_[i] = {samples[i] * window_[i], 0.f};
    Fft(work_);
    // Нормировка: синус полной шкалы даёт 0 дБ (усиление окна Ханна 0.5)
    const size_t half = size_ / 2;
    binsDb_.assign(half, static_cast<float>(kFloorDb));
    const double norm = 2.0 / (static_cast<double>(size_) * 0.5);
    for (size_t k = 1; k < half; ++k) {
        const double mag = std::abs(work_[k]) * norm;
        binsDb_[k] = static_cast<float>(std::max(kFloorDb, 20 * std::log10(mag + 1e-12)));
    }
    out.assign(static_cast<size_t>(std::max(bands, 0)), 0.f);
    if (bands <= 0) return;
    const double binHz = sampleRate / static_cast<double>(size_);
    const double ratio = std::log(kMaxHz / kMinHz);
    for (int b = 0; b < bands; ++b) {
        const double lo = kMinHz * std::exp(ratio * b / bands);
        const double hi = kMinHz * std::exp(ratio * (b + 1) / bands);
        // Узкие низкие полосы могут не содержать ни одного бина - берём ближайший
        size_t k0 = static_cast<size_t>(std::floor(lo / binHz));
        size_t k1 = static_cast<size_t>(std::ceil(hi / binHz));
        k0 = std::clamp<size_t>(k0, 1, half - 1);
        k1 = std::clamp<size_t>(k1, k0 + 1, half);
        float peak = static_cast<float>(kFloorDb);
        for (size_t k = k0; k < k1; ++k) peak = std::max(peak, binsDb_[k]);
        out[static_cast<size_t>(b)] = static_cast<float>((peak - kFloorDb) / -kFloorDb);
    }
}

}  // namespace pm
