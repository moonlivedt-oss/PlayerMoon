// БПФ на синтетическом синусе: пик в нужной полосе, утечка мала, амплитуда в дБ верная
#include <algorithm>
#include <cmath>
#include <vector>

#include "core/fft.h"
#include "tests/check.h"

TEST(FftSinePeak) {
    const double rate = 44100;
    const size_t n = 2048;
    pm::SpectrumAnalyzer an(n);
    // Синус 1 кГц, амплитуда 0.5 (-6 дБ полной шкалы)
    std::vector<float> s(n);
    for (size_t i = 0; i < n; ++i) s[i] = static_cast<float>(0.5 * std::sin(2 * 3.14159265358979 * 1000 * i / rate));
    std::vector<float> bands;
    an.Compute(s.data(), rate, 48, bands);
    const auto& db = an.BinsDb();
    const size_t peakBin = static_cast<size_t>(std::max_element(db.begin(), db.end()) - db.begin());
    const double binHz = rate / n;
    CHECK(std::abs(peakBin * binHz - 1000) <= binHz);
    // Пик около -6 дБ (окно Ханна теряет до 1.4 дБ между бинами)
    CHECK(db[peakBin] > -7.5 && db[peakBin] < -5.5);
    // Далеко от пика (5 кГц и выше) - глубоко внизу
    const size_t far = static_cast<size_t>(5000 / binHz);
    CHECK(*std::max_element(db.begin() + static_cast<long>(far), db.end()) < -50);
    // Самая громкая полоса - та, что содержит 1 кГц
    const int loudest = static_cast<int>(std::max_element(bands.begin(), bands.end()) - bands.begin());
    const double ratio = std::log(pm::SpectrumAnalyzer::kMaxHz / pm::SpectrumAnalyzer::kMinHz);
    const double lo = pm::SpectrumAnalyzer::kMinHz * std::exp(ratio * loudest / 48);
    const double hi = pm::SpectrumAnalyzer::kMinHz * std::exp(ratio * (loudest + 1) / 48);
    CHECK(lo <= 1000 + binHz && hi >= 1000 - binHz);
    CHECK(bands[static_cast<size_t>(loudest)] > 0.85f);
}

TEST(FftSilenceAndImpulse) {
    pm::SpectrumAnalyzer an(1024);
    std::vector<float> zero(1024, 0.f), bands;
    an.Compute(zero.data(), 48000, 32, bands);
    CHECK(*std::max_element(bands.begin(), bands.end()) == 0.f);
    // Прямое БПФ: импульс даёт плоский спектр
    std::vector<std::complex<float>> a(16);
    a[0] = 1;
    pm::Fft(a);
    for (const auto& c : a) CHECK_NEAR(std::abs(c), 1.0, 1e-5);
}
