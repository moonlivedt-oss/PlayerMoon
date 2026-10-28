#pragma once
// БПФ и полосы спектра для визуализатора.
// Spectrum берёт последние N моно-сэмплов, окно Ханна, БПФ, и раскладывает
// амплитуды по полосам в логарифмической шкале частот (40 Гц - 16 кГц).
// Значение полосы 0..1: от -60 дБ до 0 дБ относительно полной шкалы.

#include <complex>
#include <cstddef>
#include <vector>

namespace pm {

// БПФ на месте, размер - степень двойки
void Fft(std::vector<std::complex<float>>& a);

class SpectrumAnalyzer {
public:
    explicit SpectrumAnalyzer(size_t fftSize = 2048);

    size_t FftSize() const { return size_; }
    // samples - ровно FftSize() сэмплов; out получает bands значений 0..1
    void Compute(const float* samples, double sampleRate, int bands, std::vector<float>& out);
    // Амплитуды бинов после последнего Compute, дБ относительно полной шкалы (для тестов)
    const std::vector<float>& BinsDb() const { return binsDb_; }

    static constexpr double kMinHz = 40;
    static constexpr double kMaxHz = 16000;
    static constexpr double kFloorDb = -60;

private:
    size_t size_;
    std::vector<float> window_;
    std::vector<std::complex<float>> work_;
    std::vector<float> binsDb_;
};

}  // namespace pm
