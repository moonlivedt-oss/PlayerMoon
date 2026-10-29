// Содержимое панели: вкладки («Очередь», «Текст», страницы плагинов, «Плагины»), страницы плагинов,
// кнопки-«пилюли», подсказки над элементами, уведомления, полоса прокрутки.
// Это часть PlayerView (основная логика - в player_view.cpp).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "view/player_view.h"

namespace view {

namespace {

std::string FormatClock(double seconds) {
    if (!(seconds > 0)) seconds = 0;
    const long t = static_cast<long>(seconds);
    char buf[32];
    if (t >= 3600) std::snprintf(buf, sizeof(buf), "%ld:%02ld:%02ld", t / 3600, (t / 60) % 60, t % 60);
    else std::snprintf(buf, sizeof(buf), "%ld:%02ld", t / 60, t % 60);
    return buf;
}

constexpr float kPageBar = 40;     // шапка страницы с кнопками
constexpr float kRowPill = 24;     // кнопки в строке страницы
constexpr float kPillGap = 6;

}  // namespace

// ---------------- геометрия ----------------

int PlayerView::RowCount() const {
    switch (CurrentKind()) {
        case TabKind::Queue: return listCount;
        case TabKind::Lyrics: return static_cast<int>(lyrics.lines.size());
        case TabKind::Plugins: return pluginCount + 1;
        case TabKind::Page: {
            const int pg = PageOf(panelTab);
            return pg >= 0 && pg < static_cast<int>(pages.size()) ? static_cast<int>(pages[static_cast<size_t>(pg)].rows.size()) : 0;
        }
    }
    return 0;
}

float PlayerView::PageBarHeight(int page) const {
    if (page < 0 || page >= static_cast<int>(pages.size())) return 0;
    return pages[static_cast<size_t>(page)].actions.empty() ? 0.f : kPageBar;
}

Rect PlayerView::PageActionRect(int page, int i) const {
    const Rect& list = frame[Elem::List];
    const auto& acts = pages[static_cast<size_t>(page)].actions;
    // Не помещаются (крупный текст) - все сжимаются одинаково, подписи обрезаются многоточием
    float total = 0;
    for (const auto& a : acts) total += a.width;
    const float room = list.w - 4 - kPillGap * static_cast<float>(acts.size() - 1);
    const float k = total > room && total > 0 ? room / total : 1.f;
    float x = list.x + 2;
    for (int j = 0; j < i; ++j) x += acts[static_cast<size_t>(j)].width * k + kPillGap;
    return {x, list.y + (kPageBar - kPillHeight) / 2 - 2, acts[static_cast<size_t>(i)].width * k, kPillHeight};
}

Rect PlayerView::PageRowRect(int page, int row) const {
    const Rect& list = frame[Elem::List];
    return {list.x, list.y + PageBarHeight(page) + static_cast<float>(row) * kPageRowHeight - listScroll, list.w,
            kPageRowHeight};
}

Rect PlayerView::PageRowActionRect(int page, int row, int k) const {
    const Rect r = PageRowRect(page, row);
    const auto& acts = pages[static_cast<size_t>(page)].rows[static_cast<size_t>(row)].actions;
    float right = r.Right() - 10;
    for (int j = static_cast<int>(acts.size()) - 1; j >= 0; --j) {
        const float w = acts[static_cast<size_t>(j)].width;
        if (j == k) return {right - w, r.y + 34, w, kRowPill};
        right -= w + kPillGap;
    }
    return {};
}

Rect PlayerView::EmptyActionRect(int i) const {
    const Rect& list = frame[Elem::List];
    const float total = emptyPillW_[0] + 10 + emptyPillW_[1];
    const float x0 = list.CenterX() - total / 2;
    const float x = i == 0 ? x0 : x0 + emptyPillW_[0] + 10;
    return {x, list.y + 168, emptyPillW_[i], kPillHeight + 4};
}

float PlayerView::ContentHeight() const {
    if (CurrentKind() == TabKind::Page) return static_cast<float>(RowCount()) * kPageRowHeight;
    if (CurrentKind() == TabKind::Lyrics) return lyrics.contentHeight;
    return static_cast<float>(RowCount()) * frame.rowHeight;
}

float PlayerView::ListViewHeight() const {
    const float bar = CurrentKind() == TabKind::Page ? PageBarHeight(PageOf(panelTab)) : 0.f;
    return std::max(0.f, frame[Elem::List].h - bar);
}

// ---------------- вкладки и страницы ----------------

void PlayerView::LayoutTabs() {
    if (!hasTheme) return;
    const int n = TabCount();
    const bool small = n > 3;
    const TextStyle style = small ? TextStyle::TabSmall : TextStyle::Tab;
    tabTextSize = theme.material.font.fsHeading * layoutInput.textScale * (small ? 0.87f : 1.f);
    auto measure = [&](TextStyle st, const std::string& t) {
        return host_ ? host_->MeasureText(st, t) : static_cast<float>(t.size()) * 7.f;
    };
    const Rect& head = frame[Elem::PanelTitle];
    tabRects.assign(static_cast<size_t>(n), Rect{});
    float x = head.x;
    const float gap = small ? 14.f : 18.f;
    for (int i = 0; i < n; ++i) {
        const std::string name = i == 0                 ? std::string("Очередь")
                                  : i == kLyricsTab     ? std::string("Текст")
                                  : i == n - 1          ? std::string("Плагины")
                                                        : pages[static_cast<size_t>(i - 2)].title;
        const float w = std::min(110.f, measure(style, name));
        tabRects[static_cast<size_t>(i)] = {x, head.y, w, head.h};
        x += w + gap;
    }
    tabHover.resize(static_cast<size_t>(n), motion::Tween(0));
    if (panelTab >= n) panelTab = n - 1;
    const Rect& a = tabRects[static_cast<size_t>(panelTab)];
    tabLineX.Snap(a.x);
    tabLineW.Snap(a.w);
    emptyPillW_[0] = measure(TextStyle::Pill, "Открыть папку") + 28;
    emptyPillW_[1] = measure(TextStyle::Pill, "Открыть файлы") + 28;
}

int PlayerView::TabAt(Point p) const {
    if (!playlistOpen || mini) return -1;
    for (size_t i = 0; i < tabRects.size(); ++i) {
        // Зона нажатия чуть выше и ниже текста
        if (tabRects[i].Inflate(4).Contains(p.x, p.y)) return static_cast<int>(i);
    }
    return -1;
}

void PlayerView::SetPages(std::vector<PanelPage> newPages, double nowTime) {
    timeline.SetNow(nowTime);
    // Вкладка остаётся той же: «Плагины» - последняя, страница - по номеру
    const TabKind kind = CurrentKind();
    const int page = PageOf(panelTab);
    for (auto& pg : newPages) {
        for (auto& a : pg.actions) a.width = (host_ ? host_->MeasureText(TextStyle::Pill, a.label) : 60.f) + 24;
        for (auto& r : pg.rows)
            for (auto& a : r.actions) a.width = (host_ ? host_->MeasureText(TextStyle::Pill, a.label) : 60.f) + 20;
    }
    const bool structural = newPages.size() != pages.size();
    pages = std::move(newPages);
    if (kind == TabKind::Plugins) panelTab = PluginsTab();
    else if (kind == TabKind::Page && page >= static_cast<int>(pages.size())) panelTab = 0;
    if (structural) LayoutTabs();
    // Прокрутка не дальше конца списка
    const float maxScroll = std::max(0.f, ContentHeight() - ListViewHeight());
    if (scrollAnim.Target() > maxScroll) scrollAnim.Snap(maxScroll);
    timeline.KeepBusyUntil(nowTime + 0.001);  // перерисовать
}

bool PlayerView::PillAt(Point p, int* row, int* idx) const {
    if (!playlistOpen || mini || !frame[Elem::List].Contains(p.x, p.y)) return false;
    if (CurrentKind() == TabKind::Queue && listCount == 0) {
        for (int i = 0; i < 2; ++i)
            if (EmptyActionRect(i).Contains(p.x, p.y)) {
                *row = -3;
                *idx = i;
                return true;
            }
        return false;
    }
    if (CurrentKind() != TabKind::Page) return false;
    const int pg = PageOf(panelTab);
    if (pg < 0 || pg >= static_cast<int>(pages.size())) return false;
    const PanelPage& page = pages[static_cast<size_t>(pg)];
    for (int i = 0; i < static_cast<int>(page.actions.size()); ++i)
        if (PageActionRect(pg, i).Contains(p.x, p.y)) {
            *row = -1;
            *idx = i;
            return true;
        }
    // Строки - только в видимой части (под шапкой страницы)
    if (p.y < frame[Elem::List].y + PageBarHeight(pg)) return false;
    for (int r = 0; r < static_cast<int>(page.rows.size()); ++r)
        for (int k = 0; k < static_cast<int>(page.rows[static_cast<size_t>(r)].actions.size()); ++k)
            if (PageRowActionRect(pg, r, k).Contains(p.x, p.y)) {
                *row = r;
                *idx = k;
                return true;
            }
    return false;
}

void PlayerView::SetPillHover(int row, int idx) {
    if (row == hoverPillRow && idx == hoverPillIdx) return;
    hoverPillRow = row;
    hoverPillIdx = idx;
    pillHover.Snap(0);
    if (idx >= 0) pillHover.To(1, motion.fast, motion.out, timeline);
}

// ---------------- подсказки ----------------

std::string PlayerView::TooltipFor(Elem e, Point p) const {
    switch (e) {
        case Elem::Shuffle: return snap.shuffle ? "Перемешивание включено" : "Перемешивание";
        case Elem::Prev: return "Предыдущий трек";
        case Elem::Next: return "Следующий трек";
        case Elem::Play:
        case Elem::MiniPlay: return snap.playing ? "Пауза" : "Играть";
        case Elem::Repeat:
            return snap.repeat == pm::Repeat::Off ? "Повтор выключен"
                   : snap.repeat == pm::Repeat::All ? "Повтор плейлиста"
                                                    : "Повтор трека";
        case Elem::VolBtn: return snap.muted ? "Включить звук" : "Без звука";
        case Elem::Volume: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "Громкость %d%%", static_cast<int>(std::lround(SliderValueAt(e, p.x) * 100)));
            return buf;
        }
        case Elem::Seek:
        case Elem::MiniSeek: return snap.duration > 0 ? FormatClock(SliderValueAt(e, p.x) * snap.duration) : "";
        case Elem::PlBtn: return playlistOpen ? "Скрыть плейлист" : "Плейлист";
        case Elem::TbSettings: return "Меню";
        case Elem::TbMini: return "Мини-режим";
        case Elem::TbMinimize: return "Свернуть";
        case Elem::TbClose: return "Закрыть";
        case Elem::MiniExpand: return "Развернуть";
        case Elem::TimeCur:
        case Elem::TimeDur: return showRemaining ? "Показывать длительность" : "Показывать оставшееся время";
        default: return "";
    }
}

void PlayerView::HideTooltip() {
    ++tooltipGen_;  // отменить отложенный показ
    if (tooltipAlpha.Target() > 0) {
        tooltipAlpha.To(0, motion.fast, motion.out, timeline);
        tooltipHiddenAt_ = timeline.Now();
    }
}

void PlayerView::UpdateTooltip(Point p) {
    lastMouse_ = p;
    const Elem e = captureSlider_ ? captureElem_ : hovered_;
    const std::string text = e == Elem::Count ? std::string() : TooltipFor(e, p);
    const bool follows = e == Elem::Seek || e == Elem::Volume || e == Elem::MiniSeek;
    auto anchorFor = [&](Elem el) {
        const Rect& r = frame[el];
        return follows ? Rect{p.x - 1, r.y, 2, r.h} : r;
    };
    if (e == tooltipElem_) {
        // Тот же элемент: у ползунков подпись и место едут за мышью
        if (follows && tooltipAlpha.Target() > 0) {
            tooltipText = text;
            tooltipAnchor = anchorFor(e);
            timeline.KeepBusyUntil(timeline.Now() + 0.001);
        }
        return;
    }
    HideTooltip();
    tooltipElem_ = e;
    if (text.empty()) return;
    const int gen = ++tooltipGen_;
    const Rect anchor = anchorFor(e);
    auto show = [this, gen, e, anchor] {
        if (gen != tooltipGen_) return;
        tooltipText = TooltipFor(e, lastMouse_);
        if (tooltipText.empty()) return;
        tooltipAnchor = anchor;
        tooltipAlpha.To(1, motion.fast, motion.out, timeline);
    };
    // Только что видели подсказку - следующая сразу, иначе после паузы
    if (timeline.Now() - tooltipHiddenAt_ < 0.5) show();
    else ScheduleLazy(timeline.Now() + 0.55, show);
}

// ---------------- уведомления, прокрутка, занятость ----------------

void PlayerView::ShowToast(const std::string& text, double nowTime) {
    timeline.SetNow(nowTime);
    toastText = text;
    toastAlpha.To(1, motion.base, motion.out, timeline);
    const int gen = ++toastGen_;
    // Длиннее текст - дольше висит
    const double hold = std::min(6.0, 2.2 + static_cast<double>(text.size()) * 0.02);
    ScheduleLazy(nowTime + hold, [this, gen] {
        if (gen == toastGen_) toastAlpha.To(0, motion.slow, motion.out, timeline);
    });
}

void PlayerView::ShowScrollbar(bool autoHide) {
    if (ContentHeight() <= ListViewHeight() + 0.5f) return;
    if (scrollbarAlpha.Target() < 1) scrollbarAlpha.To(1, motion.fast, motion.out, timeline);
    const int gen = ++scrollGen_;
    if (autoHide)
        ScheduleLazy(timeline.Now() + 1.2, [this, gen] {
            if (gen == scrollGen_ && !overList_) scrollbarAlpha.To(0, motion.slow, motion.out, timeline);
        });
}

void PlayerView::SetBusy(bool on, const std::string& text) {
    busy = on;
    busyText = text;
}

void PlayerView::ScheduleLazy(double at, std::function<void()> fn) { lazy_.push_back({at, std::move(fn)}); }

double PlayerView::NextLazyWake() const {
    double t = std::numeric_limits<double>::infinity();
    for (const auto& l : lazy_) t = std::min(t, l.at);
    return t;
}

}  // namespace view
