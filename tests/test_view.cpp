// Интерфейс без окна: раскладка «Классика» против координат макета, переходы режимов,
// правило «ноль кадров в покое», бегущая строка, визуализатор.
#include <cmath>

#include "tests/check.h"
#include "theme/theme.h"
#include "view/layout.h"
#include "view/marquee.h"
#include "view/player_view.h"
#include "view/svg_arc.h"
#include "view/visualizer.h"

namespace {

theme::ThemeLibrary& Lib() {
    static theme::ThemeLibrary lib;
    static bool loaded = lib.Load(std::filesystem::path(tests::SourceDir()) / "assets");
    (void)loaded;
    return lib;
}

bool Near(const view::Rect& r, float x, float y, float w, float h, float eps = 0.6f) {
    return std::abs(r.x - x) <= eps && std::abs(r.y - y) <= eps && std::abs(r.w - w) <= eps && std::abs(r.h - h) <= eps;
}

// Окно-заглушка: запоминает вызовы
class FakeHost : public view::IViewHost {
public:
    int toggles = 0, nexts = 0, prevs = 0, resizes = 0, menus = 0;
    double seekTo = -1;
    float volume = -1;
    view::Size lastSize;
    void TogglePlay() override { ++toggles; }
    void Next() override { ++nexts; }
    void Prev() override { ++prevs; }
    void Seek(double s) override { seekTo = s; }
    void SetVolume(float v) override { volume = v; }
    void ToggleMute() override {}
    void CycleRepeat() override {}
    void ToggleShuffle() override {}
    void PlayIndex(int) override {}
    void ResizeWindow(view::Size s) override {
        ++resizes;
        lastSize = s;
    }
    void MinimizeWindow() override {}
    void CloseWindow() override {}
    void ShowMenu(view::Point) override { ++menus; }
    float MeasureText(view::TextStyle, const std::string& t) override { return static_cast<float>(t.size()) * 9.f; }
};

// Прогнать кадры до момента t с шагом 1/60 с
void RunUntil(view::PlayerView& v, double& now, double t, const view::PlaybackSnapshot& s) {
    while (now < t) {
        now += 1.0 / 60;
        v.Update(now, s, nullptr, true);
    }
}

}  // namespace

TEST(ClassicLayoutMatchesMockup) {
    // Эталон - getBoundingClientRect() элементов mockup/index.html (тема «Стекло», окно 420x680)
    const theme::Theme t = Lib().Make("glass");
    view::LayoutInput in;
    in.theme = &t;
    in.geometry = Lib().Layout("classic");
    auto layout = view::MakeLayout("classic");
    view::Frame f;
    layout->Arrange(in, true, f);
    using view::Elem;
    CHECK(Near(f[Elem::TbSettings], 244 + 360, 0, 44, 40));  // с открытым плейлистом окно 780: кнопки справа
    view::Frame closed;
    layout->Arrange(in, false, closed);
    CHECK(Near(closed[Elem::TbSettings], 244, 0, 44, 40));
    CHECK(Near(closed[Elem::TbClose], 376, 0, 44, 40));
    CHECK(Near(f[Elem::Cover], 64, 44, 292, 292));
    CHECK(Near(f[Elem::Title], 26, 358, 368, 26.25f));
    CHECK(Near(f[Elem::Artist], 26, 387.3f, 368, 19.6f));
    CHECK(Near(f[Elem::Album], 26, 409.8f, 368, 16.8f));
    CHECK(Near(f[Elem::Viz], 26, 440.6f, 368, 40));
    CHECK(Near(f[Elem::Seek], 26, 492.6f, 368, 20));
    CHECK(std::abs(f[Elem::TimeCur].y - 514.6f) < 0.6f);
    CHECK(Near(f[Elem::Shuffle], 26, 551.6f, 42, 42));
    CHECK(Near(f[Elem::Prev], 102, 551.6f, 42, 42));
    CHECK(Near(f[Elem::Play], 178, 540.6f, 64, 64));
    CHECK(Near(f[Elem::Next], 276, 551.6f, 42, 42));
    CHECK(Near(f[Elem::Repeat], 352, 551.6f, 42, 42));
    CHECK(Near(f[Elem::VolBtn], 26, 612, 42, 42));
    CHECK(Near(f[Elem::Volume], 74, 623, 107, 20));
    CHECK(Near(f[Elem::PlBtn], 352, 612, 42, 42));
    // Плейлист сбоку
    CHECK(Near(f[Elem::Panel], 420, 40, 360, 640));
    CHECK(std::abs(f[Elem::PanelTitle].x - 441) < 0.6f && std::abs(f[Elem::PanelTitle].y - 50) < 0.6f);
    const view::Rect row0 = f.Row(0, 0);
    CHECK(Near(row0, 431, 79, 339, 54, 0.8f));
    CHECK(std::abs(row0.x + f.rowTextX - 483) < 0.6f);
    // Мини-режим
    view::ArrangeMini(t.material, 1, f);
    CHECK(Near(f[Elem::MiniCover], 14, 13, 60, 60));
    CHECK(Near(f[Elem::MiniTitle], 86, 25.4f, 162, 18.2f));
    CHECK(Near(f[Elem::MiniPlay], 260, 21, 44, 44));
    CHECK(Near(f[Elem::MiniExpand], 316, 26, 34, 34));
    CHECK(Near(f[Elem::MiniSeek], 0, 77, 360, 18));
    // Размер окна
    CHECK(layout->WindowSize(in, false).w == 420 && layout->WindowSize(in, true).w == 780);
}

TEST(ViewZeroFramesAtRest) {
    FakeHost host;
    view::PlayerView v;
    v.SetHost(&host);
    double now = 1;
    v.ApplyTheme(Lib().Make("glass"), Lib().Layout("classic"), false, now, false);
    view::TrackText tt;
    tt.title = "Короткое";
    tt.palette = theme::MakeCoverPalette(tt.title);
    v.OnTrackChanged(tt, 0, now, false);
    view::PlaybackSnapshot s;
    s.hasTrack = true;
    s.duration = 100;
    s.count = 3;
    v.Update(now, s, nullptr, true);
    // Пауза, ничего не происходит - кадры не нужны
    RunUntil(v, now, now + 0.5, s);
    CHECK(!v.NeedsFrames());
    // Наведение запускает анимацию, по её окончании - снова покой
    v.OnMouseMove({200, 570}, now);
    v.Update(now, s, nullptr, true);
    CHECK(v.NeedsFrames());
    CHECK(v.Button(view::Elem::Play).hover);
    RunUntil(v, now, now + 1.0, s);
    CHECK(!v.NeedsFrames());
    // Игра в полном режиме - кадры нужны (прогресс и визуализатор)
    s.playing = true;
    std::vector<float> spectrum(48, 0.7f);
    v.Update(now + 0.016, s, &spectrum, true);
    CHECK(v.NeedsFrames());
    // Пауза: полосы опускаются и цикл кадров останавливается
    s.playing = false;
    for (int i = 0; i < 30; ++i) v.Update(now += 0.016, s, &spectrum, true);
    RunUntil(v, now, now + 2.0, s);
    CHECK(!v.NeedsFrames());
    for (float val : v.viz.Values()) CHECK(val == 0.f);
    // Окно свёрнуто - даже при игре кадров нет
    s.playing = true;
    v.Update(now += 0.016, s, &spectrum, false);
    CHECK(!v.NeedsFrames());
}

TEST(ViewButtonsAndSliders) {
    FakeHost host;
    view::PlayerView v;
    v.SetHost(&host);
    double now = 1;
    v.ApplyTheme(Lib().Make("glass"), Lib().Layout("classic"), false, now, false);
    view::PlaybackSnapshot s;
    s.hasTrack = true;
    s.duration = 200;
    s.volume = 0.5f;
    v.Update(now, s, nullptr, true);
    // Клик по «пуску»: нажатие уменьшает, отпускание вызывает действие
    v.OnMouseMove({210, 572}, now);
    v.OnMouseDown({210, 572}, now);
    RunUntil(v, now, now + 0.2, s);
    CHECK(v.Button(view::Elem::Play).scale.Value(now) < 1.0f);
    v.OnMouseUp({210, 572}, now);
    CHECK(host.toggles == 1);
    // Клик по середине полосы перемотки - перемотка на середину
    v.OnMouseDown({26 + 184, 502}, now);
    v.OnMouseUp({26 + 184, 502}, now);
    CHECK(std::abs(host.seekTo - 100) < 0.5);
    // Перетаскивание громкости меняет её сразу
    v.OnMouseDown({74, 633}, now);
    v.OnMouseMove({74 + 107, 633}, now);
    CHECK(std::abs(host.volume - 1.0f) < 1e-4);
    v.OnMouseUp({74 + 107, 633}, now);
    // Заголовок окна тянет окно, кнопки заголовка - нет
    CHECK(v.HitTest({100, 20}) == view::PlayerView::Hit::Caption);
    CHECK(v.HitTest({400, 20}) == view::PlayerView::Hit::Client);
}

TEST(ViewPlaylistAndMiniTransitions) {
    FakeHost host;
    view::PlayerView v;
    v.SetHost(&host);
    double now = 1;
    v.ApplyTheme(Lib().Make("glass"), Lib().Layout("classic"), false, now, false);
    view::PlaybackSnapshot s;
    s.count = 12;
    v.Update(now, s, nullptr, true);
    // Открытие плейлиста: окно сразу шире, панель проявляется
    v.TogglePlaylist(now);
    CHECK(host.lastSize.w == 780);
    RunUntil(v, now, now + 1, s);
    CHECK(std::abs(v.panelAlpha.Value(now) - 1) < 1e-4);
    // Закрытие: окно сужается после того, как панель скрылась
    const int before = host.resizes;
    v.TogglePlaylist(now);
    CHECK(host.resizes == before);
    RunUntil(v, now, now + 1, s);
    CHECK(host.lastSize.w == 420);
    // Мини-режим: окно 360x88 после затухания полного содержимого, и обратно
    v.ToggleMini(now);
    RunUntil(v, now, now + 0.5, s);
    CHECK(host.lastSize.w == 360 && host.lastSize.h == 88);
    CHECK(std::abs(v.miniAlpha.Value(now) - 1) < 1e-4 && v.fullAlpha.Value(now) < 1e-4);
    CHECK(v.HitTest({200, 40}) == view::PlayerView::Hit::Caption);  // мини-полоску можно тянуть
    v.ToggleMini(now);
    RunUntil(v, now, now + 0.5, s);
    CHECK(host.lastSize.w == 420 && host.lastSize.h == 680);
    CHECK(!v.NeedsFrames());
}

TEST(ViewTrackChangeAnimates) {
    FakeHost host;
    view::PlayerView v;
    v.SetHost(&host);
    double now = 1;
    v.ApplyTheme(Lib().Make("glass"), Lib().Layout("classic"), false, now, false);
    view::PlaybackSnapshot s;
    v.Update(now, s, nullptr, true);
    view::TrackText a, b;
    a.title = "Первый";
    a.serial = 1;
    a.palette = theme::MakeCoverPalette(a.title);
    b.title = "Последний трамвай до рассвета над спящим заливом";
    b.serial = 2;
    b.palette = theme::MakeCoverPalette(b.title);
    v.OnTrackChanged(a, 0, now, false);
    v.OnTrackChanged(b, 1, now, true);
    // Две обложки на время перехода, текст меняется посередине
    CHECK(v.covers.size() == 2);
    CHECK(v.title.shown == "Первый" && v.title.hasPending);
    RunUntil(v, now, now + 1, s);
    CHECK(v.covers.size() == 1 && v.covers[0].serial == 2);
    CHECK(v.title.shown == b.title);
    // Фон перетёк в цвета новой обложки
    const motion::Rgba c = v.c1.Value(now);
    CHECK(std::abs(c.r - b.palette.bg1.r) < 1e-4 && std::abs(c.g - b.palette.bg1.g) < 1e-4);
    // Длинное название (по ширине-заглушке 9 px на байт) не помещается - бегущая строка включена
    CHECK(v.titleMarquee.Active());
}

TEST(MarqueeAndVisualizer) {
    view::Marquee m;
    m.Measure(500, 368, 48, 1.6, false);
    CHECK(m.Active());
    // Пауза 1.6 с перед движением (два шага по 1 с), потом движение
    CHECK(m.Step(1.0, true, 30));
    CHECK(m.Step(1.0, true, 30));
    CHECK(m.Offset() == 0);
    m.Step(1.0, true, 30);
    CHECK(m.Offset() > 0);
    // На паузе докручивает до шва и останавливается
    int frames = 0;
    while (m.Step(1.0 / 60, false, 30) && frames < 1000) ++frames;
    CHECK(m.Offset() == 0 && frames < 120);
    CHECK(!m.Step(1.0 / 60, false, 30));
    // Текст помещается - строка не бежит; в сокращённом режиме - тоже
    m.Measure(100, 368, 48, 1.6, false);
    CHECK(!m.Active());
    m.Measure(500, 368, 48, 1.6, true);
    CHECK(!m.Active());

    view::VisualizerModel viz;
    viz.SetBars(48);
    std::vector<float> target(48, 0.8f);
    for (int i = 0; i < 60; ++i) viz.Step(1.0 / 60, target, true, 18, 1.5f);
    CHECK(viz.Values()[10] > 0.7f);
    int steps = 0;
    while (viz.Step(1.0 / 60, {}, false, 18, 1.5f) && steps < 1000) ++steps;
    CHECK(steps < 120);  // меньше двух секунд до полной остановки
}

TEST(SvgArcToBeziers) {
    // Полуокружность радиуса 10 из (0,0) в (20,0), sweep=1: по часовой (ось y вниз) - через верх (10,-10)
    auto c = view::ArcToCubics({0, 0}, 10, 10, 0, false, true, {20, 0});
    CHECK(c.size() == 2);
    CHECK(std::abs(c[0][2].x - 10) < 1e-3f && std::abs(c[0][2].y + 10) < 1e-3f);
    CHECK(c.back()[2].x == 20 && c.back()[2].y == 0);
    // Точка середины кривой лежит на окружности (погрешность приближения < 0.03%)
    auto mid = [](const view::Point& p0, const view::Cubic& q) {
        return view::Point{0.125f * p0.x + 0.375f * q[0].x + 0.375f * q[1].x + 0.125f * q[2].x,
                           0.125f * p0.y + 0.375f * q[0].y + 0.375f * q[1].y + 0.125f * q[2].y};
    };
    const view::Point m = mid({0, 0}, c[0]);
    CHECK(std::abs(std::hypot(m.x - 10, m.y) - 10) < 0.01f);
    // sweep=0 - через низ
    auto d = view::ArcToCubics({0, 0}, 10, 10, 0, false, false, {20, 0});
    CHECK(std::abs(d[0][2].y - 10) < 1e-3f);
    // Луна из иконки: большая дуга 9 из (15.2,3.2) в (20.8,18.3), large=1 sweep=0 - три куска,
    // обход через левую сторону: середина левого куска около x = 5.8 (центр около 14.2)
    auto moon = view::ArcToCubics({15.2f, 3.2f}, 9, 9, 0, true, false, {20.8f, 18.3f});
    CHECK(moon.size() == 3);
    float minX = 100;
    view::Point prev{15.2f, 3.2f};
    for (const auto& q : moon) {
        minX = std::min(minX, mid(prev, q).x);
        prev = q[2];
    }
    CHECK(minX > 5.2f && minX < 6.2f);
    // Радиус меньше половины хорды увеличивается до минимального: получается полуокружность
    auto small = view::ArcToCubics({0, 0}, 1, 1, 0, false, true, {20, 0});
    CHECK(std::abs(small[0][2].y + 10) < 1e-3f);
    // Совпадающие концы - ничего не рисуется
    CHECK(view::ArcToCubics({1, 1}, 5, 5, 0, false, true, {1, 1}).empty());
}
