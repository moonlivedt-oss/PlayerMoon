#include "view/player_view.h"

#include <algorithm>
#include <cmath>

namespace view {

namespace {

// Элементы-кнопки (для наведения и нажатия)
bool IsButton(Elem e) {
    switch (e) {
        case Elem::TbSettings:
        case Elem::TbMini:
        case Elem::TbMinimize:
        case Elem::TbClose:
        case Elem::Shuffle:
        case Elem::Prev:
        case Elem::Play:
        case Elem::Next:
        case Elem::Repeat:
        case Elem::VolBtn:
        case Elem::PlBtn:
        case Elem::MiniPlay:
        case Elem::MiniExpand: return true;
        default: return false;
    }
}

constexpr double kPanelDelay = 0.06;  // задержка появления панели плейлиста (60 мс в макете)
constexpr float kRowShift = 16;       // сдвиг строк в лесенке
constexpr float kPanelShift = -28;    // сдвиг панели плейлиста при закрытии

}  // namespace

PlayerView::PlayerView() = default;

void PlayerView::Schedule(double at, std::function<void()> fn) {
    pending_.push_back({at, std::move(fn)});
    timeline.KeepBusyUntil(at);
}

Size PlayerView::FullWindowSize(bool withPanel) const {
    return layout ? layout->WindowSize(layoutInput, withPanel) : Size{420, 680};
}

bool PlayerView::DarkUi() const {
    const motion::Rgba& t = theme.settings.text;
    return 0.2126f * t.r + 0.7152f * t.g + 0.0722f * t.b > 0.5f;
}

void PlayerView::Rearrange() {
    layoutInput.theme = &theme;
    layout->Arrange(layoutInput, windowWithPanel_, frame);
    ArrangeMini(theme.material, layoutInput.textScale, frame);
}

void PlayerView::Resize(bool withPanel) {
    windowWithPanel_ = withPanel;
    if (layout) Rearrange();
    if (host_) host_->ResizeWindow(FullWindowSize(withPanel));
}

void PlayerView::ApplyThemeNow(const theme::Theme& t, const theme::LayoutGeometry& geometry, bool systemReduce) {
    theme = t;
    hasTheme = true;
    systemReduce_ = systemReduce;
    motion = MakeMotionParams(theme, systemReduce);
    layout = MakeLayout(theme.settings.layout);
    layoutInput.geometry = geometry;
    layoutInput.textScale = static_cast<float>(theme.settings.textScale);
    Rearrange();
    viz.SetBars(static_cast<int>(theme.settings.vizBars));
    seek.Init(motion.thumbIdle, motion.thumbHover);
    volume.Init(motion.thumbIdle, motion.thumbHover);
    miniSeek.Init(0, 1);  // в мини-режиме ручка прогресса только при наведении
    ApplyCoverColors(currentPalette_, false);
    MeasureMarquees();
    windowWithPanel_ = playlistOpen && !mini;
    Rearrange();
    if (host_) host_->ResizeWindow(mini ? Size{kMiniWidth, kMiniHeight} : FullWindowSize(windowWithPanel_));
}

void PlayerView::ApplyTheme(const theme::Theme& t, const theme::LayoutGeometry& geometry, bool systemReduce,
                            double nowTime, bool animate) {
    timeline.SetNow(nowTime);
    if (!animate || !hasTheme || motion.reduce) {
        ApplyThemeNow(t, geometry, systemReduce);
        return;
    }
    // Как смена пресета в макете: окно гаснет (140 мс), тема применяется, окно проявляется
    windowAlpha.To(0, 0.14, motion.in, timeline);
    windowScale.To(0.975f, 0.14, motion.in, timeline);
    Schedule(nowTime + 0.15, [this, t, geometry, systemReduce] {
        ApplyThemeNow(t, geometry, systemReduce);
        windowAlpha.To(1, 0.24, motion::Easing::Bezier(0, 0, 0.58, 1), timeline);
        windowScale.To(1, 0.3, motion::Easing::Bezier(0.22, 1, 0.36, 1), timeline);
    });
}

void PlayerView::RestoreModes(bool miniMode, bool open) {
    mini = miniMode;
    playlistOpen = open && !miniMode;
    fullAlpha.Snap(mini ? 0.f : 1.f);
    miniAlpha.Snap(mini ? 1.f : 0.f);
    panelAlpha.Snap(playlistOpen ? 1.f : 0.f);
    panelShift.Snap(playlistOpen ? 0.f : kPanelShift);
    playlistToggle.dot.Snap(playlistOpen ? 1.f : 0.f);
    playlistToggle.on = playlistOpen;
    windowWithPanel_ = playlistOpen;
    if (layout) Rearrange();
}

void PlayerView::ApplyCoverColors(const theme::CoverPalette& pal, bool animate) {
    currentPalette_ = pal;
    const double d = animate ? motion.bg : 0;
    c1.To(pal.bg1, d, motion.linear, timeline);
    c2.To(pal.bg2, d, motion.linear, timeline);
    c3.To(pal.bg3, d, motion.linear, timeline);
    const motion::Rgba acc =
        theme.settings.accentFromCover ? theme::CoverAccent(pal, DarkUi()) : theme.settings.accent;
    accent.To(acc, d, motion.linear, timeline);
    if (!animate) {
        c1.Snap(pal.bg1);
        c2.Snap(pal.bg2);
        c3.Snap(pal.bg3);
        accent.Snap(acc);
    }
}

void PlayerView::SwapLine(LineAnim& line, const std::string& value, int order, bool animate) {
    if (!animate) {
        line.shown = value;
        line.hasPending = false;
        line.alpha.Snap(1);
        line.dy.Snap(0);
        return;
    }
    // Подъём (rise): уход вверх на 6 px за 0.45 * text, приход снизу с 10 px за text; строки по очереди через 30 мс
    const double delay = order * 0.03;
    const double outDur = motion.text * 0.45;
    line.alpha.To(0, outDur, motion.in, timeline, delay);
    line.dy.To(motion.reduce ? 0.f : -6.f, outDur, motion.in, timeline, delay);
    line.pending = value;
    line.hasPending = true;
    line.switchAt = timeline.Now() + delay + outDur;
    timeline.KeepBusyUntil(line.switchAt + motion.text);
}

void PlayerView::OnTrackChanged(const TrackText& text, int direction, double nowTime, bool animate) {
    timeline.SetNow(nowTime);
    now = nowTime;
    const int dir = direction < 0 ? -1 : 1;
    animate = animate && hasTheme;
    ApplyCoverColors(text.palette, animate);

    // Обложка «Наплыв» (zoom): старая уходит со сдвигом и уменьшением, новая приходит с увеличением
    const double coverDur = motion.cover;
    const bool fadeOnly = motion.reduce;
    for (CoverSlot& c : covers) {
        if (c.leaving) continue;
        c.leaving = true;
        if (!animate) {
            c.removeAt = nowTime;
            continue;
        }
        c.alpha.To(0, coverDur * 0.8, motion.in, timeline);
        c.dx.To(fadeOnly ? 0.f : -dir * 24.f, coverDur * 0.8, motion.in, timeline);
        c.scale.To(fadeOnly ? 1.f : 0.9f, coverDur * 0.8, motion.in, timeline);
        c.removeAt = nowTime + coverDur * 0.8;
    }
    covers.erase(std::remove_if(covers.begin(), covers.end(),
                                [&](const CoverSlot& c) { return c.leaving && c.removeAt <= nowTime; }),
                 covers.end());
    CoverSlot slot;
    slot.serial = text.serial;
    slot.palette = text.palette;
    slot.hasImage = text.hasImage;
    if (animate) {
        slot.alpha.Snap(0);
        slot.dx.Snap(fadeOnly ? 0.f : dir * 24.f);
        slot.scale.Snap(fadeOnly ? 1.f : 1.06f);
        slot.alpha.To(1, coverDur, motion.out, timeline);
        slot.dx.To(0, coverDur, motion.out, timeline);
        slot.scale.To(1, coverDur, motion.out, timeline);
    }
    covers.push_back(slot);

    // Текст меняется анимированно только там, где его видно (как в макете)
    SwapLine(title, text.title, 0, animate && !mini);
    SwapLine(artist, text.artist, 1, animate && !mini);
    SwapLine(album, text.album, 2, animate && !mini);
    SwapLine(miniTitle, text.title, 0, animate && mini);
    SwapLine(miniArtist, text.artist, 1, animate && mini);
    MeasureMarquees();
}

void PlayerView::MeasureMarquees() {
    if (!host_ || !hasTheme) return;
    const theme::Material& m = theme.material;
    titleTextWidth = host_->MeasureText(TextStyle::Title, title.shown);
    miniTextWidth = host_->MeasureText(TextStyle::MiniTitle, miniTitle.shown);
    titleMarquee.Measure(titleTextWidth, frame[Elem::Title].w, m.marquee.gap, m.marquee.pause, motion.reduce);
    miniMarquee.Measure(miniTextWidth, frame[Elem::MiniTitle].w, m.marquee.gap, m.marquee.pause, motion.reduce);
}

// ---------------- ввод ----------------

Elem PlayerView::HitElement(Point p) const {
    auto in = [&](Elem e) { return frame[e].Contains(p.x, p.y); };
    if (mini) {
        for (Elem e : {Elem::MiniPlay, Elem::MiniExpand, Elem::MiniSeek})
            if (in(e)) return e;
        return Elem::Count;
    }
    for (Elem e : {Elem::TbSettings, Elem::TbMini, Elem::TbMinimize, Elem::TbClose})
        if (in(e)) return e;
    if (playlistOpen && frame[Elem::Panel].Contains(p.x, p.y)) return Elem::Count;
    for (Elem e : {Elem::Shuffle, Elem::Prev, Elem::Play, Elem::Next, Elem::Repeat, Elem::VolBtn, Elem::PlBtn,
                   Elem::Seek, Elem::Volume})
        if (in(e)) return e;
    return Elem::Count;
}

SliderState* PlayerView::SliderAt(Point p, Elem* which) {
    const Elem e = HitElement(p);
    if (which) *which = e;
    if (e == Elem::Seek) return &seek;
    if (e == Elem::Volume) return &volume;
    if (e == Elem::MiniSeek) return &miniSeek;
    return nullptr;
}

float PlayerView::SliderValueAt(Elem e, float x) const {
    const Rect& r = frame[e];
    return r.w > 0 ? Clamp01((x - r.x) / r.w) : 0.f;
}

int PlayerView::RowAt(Point p) const {
    if (mini || !playlistOpen) return -1;
    const Rect& list = frame[Elem::List];
    if (!list.Contains(p.x, p.y)) return -1;
    const int i = static_cast<int>(std::floor((p.y - list.y + listScroll) / frame.rowHeight));
    return (i >= 0 && i < listCount) ? i : -1;
}

void PlayerView::SetHovered(Elem e) {
    if (e == hovered_) return;
    if (IsButton(hovered_)) Button(hovered_).SetHover(false, motion, timeline);
    if (hovered_ == Elem::Seek) seek.SetHover(false, motion, timeline);
    if (hovered_ == Elem::Volume) volume.SetHover(false, motion, timeline);
    if (hovered_ == Elem::MiniSeek) miniSeek.SetHover(false, motion, timeline);
    hovered_ = e;
    if (IsButton(e)) Button(e).SetHover(true, motion, timeline);
    if (e == Elem::Seek) seek.SetHover(true, motion, timeline);
    if (e == Elem::Volume) volume.SetHover(true, motion, timeline);
    if (e == Elem::MiniSeek) miniSeek.SetHover(true, motion, timeline);
}

void PlayerView::OnMouseMove(Point p, double nowTime) {
    timeline.SetNow(nowTime);
    if (captureSlider_) {
        const float v = SliderValueAt(captureElem_, p.x);
        captureSlider_->dragging = true;
        captureSlider_->dragValue = v;
        if (captureElem_ == Elem::Volume && host_) host_->SetVolume(v);
        timeline.KeepBusyUntil(nowTime + 0.001);
        return;
    }
    SetHovered(HitElement(p));
    const int row = RowAt(p);
    if (row != hoverRow) {
        if (hoverRow >= 0 && hoverRow < static_cast<int>(rowHover.size()))
            rowHover[static_cast<size_t>(hoverRow)].To(0, motion.fast, motion.out, timeline);
        hoverRow = row;
        if (row >= 0 && row < static_cast<int>(rowHover.size()))
            rowHover[static_cast<size_t>(row)].To(1, motion.fast, motion.out, timeline);
    }
}

void PlayerView::OnMouseDown(Point p, double nowTime) {
    timeline.SetNow(nowTime);
    Elem e = Elem::Count;
    if (SliderState* s = SliderAt(p, &e)) {
        captureSlider_ = s;
        captureElem_ = e;
        const float v = SliderValueAt(e, p.x);
        const float from = e == Elem::Volume ? VolumeDisplay() : SeekDisplay();
        s->JumpTo(from, v, motion, timeline);
        s->dragValue = v;
        s->dragging = false;
        if (e == Elem::Volume && host_) host_->SetVolume(v);
        s->thumb.To(s->hoverScale, motion.spring, motion.springCurve, timeline);
        return;
    }
    if (IsButton(e)) {
        pressed_ = e;
        Button(e).SetPressed(true, motion, timeline);
    }
}

void PlayerView::OnMouseUp(Point p, double nowTime) {
    timeline.SetNow(nowTime);
    if (captureSlider_) {
        SliderState* s = captureSlider_;
        const float v = s->dragging ? s->dragValue : SliderValueAt(captureElem_, p.x);
        if ((captureElem_ == Elem::Seek || captureElem_ == Elem::MiniSeek) && host_ && snap.duration > 0)
            host_->Seek(v * snap.duration);
        if (captureElem_ == Elem::Volume && host_) host_->SetVolume(v);
        // До прихода нового живого значения показываем отпущенное, без скачка назад
        s->jump.Snap(v);
        s->jumpUntil = nowTime + 0.12;
        s->dragging = false;
        captureSlider_ = nullptr;
        captureElem_ = Elem::Count;
        hovered_ = Elem::Count;
        s->hover = false;
        SetHovered(HitElement(p));
        if (!s->hover) s->thumb.To(s->idleScale, motion.spring, motion.springCurve, timeline);
        return;
    }
    if (pressed_ != Elem::Count) {
        const Elem e = pressed_;
        pressed_ = Elem::Count;
        Button(e).SetPressed(false, motion, timeline);
        if (HitElement(p) == e) Activate(e);
        return;
    }
    const int row = RowAt(p);
    if (row >= 0 && host_) {
        if (row == snap.index) host_->TogglePlay();
        else host_->PlayIndex(row);
    }
}

void PlayerView::OnMouseLeave(double nowTime) {
    timeline.SetNow(nowTime);
    if (captureSlider_) return;
    SetHovered(Elem::Count);
    if (pressed_ != Elem::Count) {
        Button(pressed_).SetPressed(false, motion, timeline);
        pressed_ = Elem::Count;
    }
    if (hoverRow >= 0 && hoverRow < static_cast<int>(rowHover.size()))
        rowHover[static_cast<size_t>(hoverRow)].To(0, motion.fast, motion.out, timeline);
    hoverRow = -1;
}

void PlayerView::OnRightClick(Point p) {
    if (host_) host_->ShowMenu(p);
}

void PlayerView::OnWheel(Point p, float delta, double nowTime) {
    timeline.SetNow(nowTime);
    if (!playlistOpen || mini || !frame[Elem::Panel].Contains(p.x, p.y)) return;
    const float maxScroll = std::max(0.f, listCount * frame.rowHeight - frame[Elem::List].h);
    // delta - доли «щелчка» колеса (120 единиц Windows = 1); щелчок = три строки
    const float target = std::clamp(scrollAnim.Target() - delta * frame.rowHeight * 3, 0.f, maxScroll);
    scrollAnim.To(target, motion.base, motion.out, timeline);
}

PlayerView::Hit PlayerView::HitTest(Point p) const {
    if (mini) return HitElement(p) == Elem::Count ? Hit::Caption : Hit::Client;
    if (frame[Elem::TitleBar].Contains(p.x, p.y) && HitElement(p) == Elem::Count) return Hit::Caption;
    return Hit::Client;
}

void PlayerView::Activate(Elem e) {
    if (!host_) return;
    switch (e) {
        case Elem::TbSettings: {
            const Rect& r = frame[Elem::TbSettings];
            host_->ShowMenu({r.x, r.Bottom()});
            break;
        }
        case Elem::TbMini:
        case Elem::MiniExpand: ToggleMini(timeline.Now()); break;
        case Elem::TbMinimize: host_->MinimizeWindow(); break;
        case Elem::TbClose: host_->CloseWindow(); break;
        case Elem::Shuffle: host_->ToggleShuffle(); break;
        case Elem::Prev: host_->Prev(); break;
        case Elem::Next: host_->Next(); break;
        case Elem::Play:
        case Elem::MiniPlay: host_->TogglePlay(); break;
        case Elem::Repeat: host_->CycleRepeat(); break;
        case Elem::VolBtn: host_->ToggleMute(); break;
        case Elem::PlBtn: TogglePlaylist(timeline.Now()); break;
        default: break;
    }
}

// ---------------- режимы ----------------

void PlayerView::TogglePlaylist(double nowTime) {
    if (mini || !host_) return;
    timeline.SetNow(nowTime);
    playlistOpen = !playlistOpen;
    playlistToggle.Set(playlistOpen, false, motion, timeline);
    if (playlistOpen) {
        // Окно сразу нужного размера (решение для системного акрила), панель въезжает внутри
        Resize(true);
        panelAlpha.To(1, motion.base, motion.out, timeline, motion.reduce ? 0 : kPanelDelay);
        panelShift.To(0, motion.slow, motion.out, timeline, motion.reduce ? 0 : kPanelDelay);
        // Текущий трек в поле зрения
        const float listH = frame[Elem::List].h;
        const float rowTop = std::max(0, snap.index) * frame.rowHeight;
        float scroll = scrollAnim.Target();
        if (rowTop < scroll) scroll = rowTop;
        if (rowTop + frame.rowHeight > scroll + listH) scroll = rowTop + frame.rowHeight - listH;
        scrollAnim.Snap(std::max(0.f, scroll));
        // Лесенка строк
        const int first = static_cast<int>(scrollAnim.Target() / frame.rowHeight);
        for (int i = 0; i < listCount && i < static_cast<int>(rowAlpha.size()); ++i) {
            const double delay = motion.reduce ? 0 : kPanelDelay + std::max(0, i - first) * motion.stagger;
            rowAlpha[static_cast<size_t>(i)].Snap(motion.reduce ? 1.f : 0.f);
            rowShift[static_cast<size_t>(i)].Snap(motion.reduce ? 0.f : kRowShift);
            rowAlpha[static_cast<size_t>(i)].To(1, motion.slow, motion.out, timeline, delay);
            rowShift[static_cast<size_t>(i)].To(0, motion.slow, motion.out, timeline, delay);
        }
    } else {
        panelAlpha.To(0, motion.base, motion.out, timeline);
        panelShift.To(kPanelShift, motion.slow, motion.out, timeline);
        // Окно сужается, когда панель уже скрылась
        Schedule(nowTime + std::max(motion.base, motion.slow), [this] {
            if (!playlistOpen && !mini) Resize(false);
        });
    }
}

void PlayerView::ToggleMini(double nowTime) {
    if (!host_) return;
    timeline.SetNow(nowTime);
    if (!mini) {
        if (playlistOpen) {
            playlistOpen = false;
            playlistToggle.Set(false, false, motion, timeline);
            panelAlpha.To(0, motion.fast, motion.out, timeline);
        }
        mini = true;
        // Вход: полное содержимое гаснет, затем окно сжимается и проявляется полоска
        miniTitle.shown = title.hasPending ? title.pending : title.shown;
        miniArtist.shown = artist.hasPending ? artist.pending : artist.shown;
        fullAlpha.To(0, motion.fast, motion.out, timeline);
        Schedule(nowTime + motion.fast, [this] {
            if (!mini) return;
            panelShift.Snap(kPanelShift);
            panelAlpha.Snap(0);
            windowWithPanel_ = false;
            Rearrange();
            host_->ResizeWindow({kMiniWidth, kMiniHeight});
            miniAlpha.To(1, motion.fast, motion.out, timeline);
            MeasureMarquees();
        });
    } else {
        mini = false;
        title.shown = miniTitle.hasPending ? miniTitle.pending : miniTitle.shown;
        artist.shown = miniArtist.hasPending ? miniArtist.pending : miniArtist.shown;
        miniAlpha.To(0, motion.fast, motion.out, timeline);
        Schedule(nowTime + motion.fast, [this] {
            if (mini) return;
            Resize(false);
            fullAlpha.To(1, motion.fast, motion.out, timeline);
            MeasureMarquees();
        });
    }
}

// ---------------- кадр ----------------

float PlayerView::SeekDisplay() const {
    const float live = snap.duration > 0 ? static_cast<float>(snap.position / snap.duration) : 0.f;
    return Clamp01(seek.Display(Clamp01(live), now));
}

float PlayerView::VolumeDisplay() const {
    return Clamp01(volume.Display(snap.muted ? 0.f : snap.volume, now));
}

double PlayerView::DisplayPosition() const {
    if (seek.dragging) return seek.dragValue * snap.duration;
    if (miniSeek.dragging) return miniSeek.dragValue * snap.duration;
    return snap.position;
}

void PlayerView::Update(double nowTime, const PlaybackSnapshot& s, const std::vector<float>* spectrum, bool visible) {
    const double dt = std::clamp(nowTime - lastUpdate_, 0.0, 0.05);
    lastUpdate_ = nowTime;
    now = nowTime;
    timeline.SetNow(nowTime);
    visible_ = visible;

    // Отложенные действия (сжатие окна после закрытия панели, смена темы и т.п.)
    if (!pending_.empty()) {
        std::vector<Pending> due;
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->at <= nowTime) {
                due.push_back(std::move(*it));
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto& d : due) d.fn();
    }

    // Подмена текста посередине анимации смены
    bool remeasure = false;
    for (LineAnim* line : {&title, &artist, &album, &miniTitle, &miniArtist}) {
        if (!line->hasPending || nowTime < line->switchAt) continue;
        line->shown = line->pending;
        line->hasPending = false;
        line->alpha.Snap(0);
        line->dy.Snap(motion.reduce ? 0.f : 10.f);
        line->alpha.To(1, motion.text, motion.out, timeline);
        line->dy.To(0, motion.text, motion.out, timeline);
        remeasure = remeasure || line == &title || line == &miniTitle;
    }
    if (remeasure) MeasureMarquees();

    // Изменения состояния плеера -> анимации кнопок
    const bool first = firstSnapshot_;
    firstSnapshot_ = false;
    auto snapOr = [&](motion::Tween& t, float target, double d, const motion::Easing& e) {
        if (first) t.Snap(target);
        else t.To(target, d, e, timeline);
    };
    if (first || s.playing != snap.playing) {
        snapOr(playMorph, s.playing ? 1.f : 0.f, motion.base, motion.out);
        if (s.playing) {
            titleMarquee.OnPlay();
            miniMarquee.OnPlay();
        }
    }
    if (first || s.shuffle != snap.shuffle) {
        if (first) { shuffleToggle.on = s.shuffle; shuffleToggle.dot.Snap(s.shuffle ? 1.f : 0.f); }
        else shuffleToggle.Set(s.shuffle, false, motion, timeline);
    }
    if (first || s.repeat != snap.repeat) {
        const bool on = s.repeat != pm::Repeat::Off, one = s.repeat == pm::Repeat::One;
        if (first) {
            repeatToggle.on = on;
            repeatToggle.dot.Snap(on ? 1.f : 0.f);
            repeatToggle.one.Snap(one ? 1.f : 0.f);
        } else {
            repeatToggle.Set(on, one, motion, timeline);
        }
    }
    if (first || s.volume != snap.volume || s.muted != snap.muted) {
        const float v = s.muted ? 0.f : s.volume;
        snapOr(wave1, v > 0.02f ? 1.f : 0.15f, motion.fast, motion.out);
        snapOr(wave2, v > 0.5f ? 1.f : 0.15f, motion.fast, motion.out);
        snapOr(muteCross, (s.muted || v <= 0) ? 1.f : 0.f, motion.spring, motion.springCurve);
    }
    snap = s;

    // Строки плейлиста: состояния по числу треков
    listCount = s.count;
    if (static_cast<int>(rowHover.size()) != listCount) {
        rowHover.resize(static_cast<size_t>(listCount));
        rowAlpha.resize(static_cast<size_t>(listCount), motion::Tween(1));
        rowShift.resize(static_cast<size_t>(listCount), motion::Tween(0));
    }
    listScroll = scrollAnim.Value(nowTime);

    // Ушедшие обложки убираем
    covers.erase(std::remove_if(covers.begin(), covers.end(),
                                [&](const CoverSlot& c) { return c.leaving && c.removeAt <= nowTime; }),
                 covers.end());

    // Непрерывные анимации: бегущая строка и визуализатор
    const float mqSpeed = static_cast<float>(theme.settings.mqSpeed);
    bool continuous = false;
    if (visible) {
        if (!mini) continuous = titleMarquee.Step(dt, s.playing, mqSpeed) || continuous;
        else continuous = miniMarquee.Step(dt, s.playing, mqSpeed) || continuous;
    }
    static const std::vector<float> kEmpty;
    if (!mini && visible) {
        const bool vizBusy = viz.Step(dt, spectrum ? *spectrum : kEmpty, s.playing && spectrum != nullptr,
                                      static_cast<float>(theme.settings.vizAttack),
                                      static_cast<float>(theme.settings.vizFall));
        continuous = continuous || vizBusy;
    } else if (mini) {
        viz.Step(1.0, kEmpty, false, 0, 100);  // в мини-режиме визуализатора нет - сразу в ноль
    }

    bool linesPending = false;
    for (LineAnim* line : {&title, &artist, &album, &miniTitle, &miniArtist}) linesPending |= line->hasPending;
    // Во время игры в полном режиме прогресс и визуализатор обновляются каждый кадр
    const bool playingFull = s.playing && visible && !mini;
    needsFrames_ = timeline.Busy() || !pending_.empty() || linesPending || continuous || playingFull ||
                   captureSlider_ != nullptr;
    if (!visible) needsFrames_ = !pending_.empty();
    // В мини-режиме во время игры полоса прогресса двигается на 1-2 px в секунду - хватает раза в секунду
    wantsTicks_ = s.playing && visible && mini && !needsFrames_;
}

}  // namespace view
