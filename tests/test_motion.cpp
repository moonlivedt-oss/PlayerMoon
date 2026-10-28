// Пружина и кривые: значения должны совпасть с макетом (mockup/ANIMATIONS.md, «Эталоны для unit-теста пружины»).
#include "motion/easing.h"
#include "motion/spring.h"
#include "motion/tween.h"
#include "tests/check.h"

namespace {

struct SpringRef {
    const char* name;
    double k, c;
    double settleMs, peak, peakMs, x100, x200;
};

// Таблица из ANIMATIONS.md
const SpringRef kRefs[] = {
    {"soft", 420, 26, 428, 1.0759, 199, 0.7801, 1.0759},
    {"bouncy", 300, 14, 684, 1.2496, 199, 0.7873, 1.2494},
    {"calm", 300, 30, 284, 1.0043, 363, 0.5610, 0.9229},
    {"snappy", 700, 46, 188, 1.0040, 240, 0.8037, 0.9998},
    {"slow", 160, 22, 392, 1.0040, 503, 0.3871, 0.7798},
};

}  // namespace

TEST(SpringMatchesMockup) {
    for (const auto& r : kRefs) {
        const motion::SpringParams p{r.k, r.c};
        CHECK_NEAR(motion::SpringSettleTime(p) * 1000, r.settleMs, 4.0);
        const auto peak = motion::FindSpringPeak(p);
        CHECK_NEAR(peak.value, r.peak, 0.002);
        CHECK_NEAR(peak.time * 1000, r.peakMs, 4.0);
        CHECK_NEAR(motion::SpringValue(p, 0.1), r.x100, 0.002);
        CHECK_NEAR(motion::SpringValue(p, 0.2), r.x200, 0.002);
    }
}

TEST(SpringEdgeCases) {
    // Критическое и передемпфированное затухание: без перелёта, доходят до 1
    const motion::SpringParams critical{400, 40};  // z = 1
    const motion::SpringParams over{100, 60};      // z = 3
    CHECK_NEAR(motion::SpringValue(critical, 0), 0, 1e-9);
    CHECK(motion::FindSpringPeak(critical).value <= 1.0 + 1e-9);
    CHECK(motion::FindSpringPeak(over).value <= 1.0 + 1e-9);
    CHECK_NEAR(motion::SpringValue(over, 10), 1, 1e-3);
}

TEST(CubicBezierCss) {
    // ease-out макета: начало быстрое, конец мягкий
    const motion::CubicBezier ease{0.22, 1, 0.36, 1};
    CHECK_NEAR(ease.Eval(0), 0, 1e-9);
    CHECK_NEAR(ease.Eval(1), 1, 1e-9);
    CHECK(ease.Eval(0.25) > 0.6);
    // Линейная кривая (0,0,1,1) - тождество
    const motion::CubicBezier lin{0, 0, 1, 1};
    for (double u = 0; u <= 1; u += 0.125) CHECK_NEAR(lin.Eval(u), u, 1e-5);
    // Стандартный CSS ease (0.25, 0.1, 0.25, 1) в середине ~0.8024
    const motion::CubicBezier cssEase{0.25, 0.1, 0.25, 1};
    CHECK_NEAR(cssEase.Eval(0.5), 0.8024, 0.001);
}

TEST(TweenAndTimeline) {
    motion::Timeline tl;
    motion::Tween t(0);
    tl.SetNow(1.0);
    t.To(10, 0.5, motion::Easing::Linear(), tl);
    CHECK(tl.Busy());
    tl.SetNow(1.25);
    CHECK_NEAR(t.Value(tl.Now()), 5, 1e-4);
    // Новая цель стартует с текущего значения, как прерванный CSS-переход
    t.To(0, 0.5, motion::Easing::Linear(), tl);
    CHECK_NEAR(t.Value(tl.Now()), 5, 1e-4);
    tl.SetNow(2.0);
    CHECK_NEAR(t.Value(tl.Now()), 0, 1e-6);
    CHECK(!tl.Busy());
    // Пружина как кривая: в конце ровно цель
    t.To(1, 0.4, motion::Easing::Spring({420, 26}), tl);
    tl.SetNow(2.4);
    CHECK_NEAR(t.Value(tl.Now()), 1, 1e-6);
}
