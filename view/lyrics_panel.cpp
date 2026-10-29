// Вкладка «Текст»: раскладка строк песни с переносами, подсветка звучащей строки и прокрутка
// за песней. Это часть PlayerView (основная логика - в player_view.cpp).

#include <algorithm>
#include <cmath>

#include "view/player_view.h"

namespace view {

namespace {

constexpr float kLyricPadX = 12;      // отступ строк от краёв списка
constexpr float kLyricGap = 10;       // между строками
constexpr float kLyricBreak = 14;     // пустая строка (между куплетами)
constexpr double kLyricLead = 0.15;   // строка подсвечивается чуть раньше: глаз успевает
constexpr double kManualPause = 4.0;  // после прокрутки колесом автопрокрутка ждёт, с

}  // namespace

void PlayerView::SetLyrics(uint64_t serial, LyricsState state, std::vector<std::string> lines,
                           std::vector<double> times, double nowTime) {
    timeline.SetNow(nowTime);
    const bool sameContent = serial == lyrics.serial && state == lyrics.state && lines == lyrics.lines;
    if (sameContent) return;
    lyrics.serial = serial;
    lyrics.state = state;
    lyrics.lines = std::move(lines);
    lyrics.times = state == LyricsState::Synced ? std::move(times) : std::vector<double>{};
    lyrics.current = -1;
    lyrics.manualUntil = 0;
    lyrics.glow.assign(lyrics.lines.size(), motion::Tween(0));
    lyrics.layoutWidth = 0;
    LayoutLyrics();
    if (static_cast<int>(rowHover.size()) < RowCount()) rowHover.resize(static_cast<size_t>(RowCount()));
    // Звучащая строка - сразу, без анимации
    if (lyrics.state == LyricsState::Synced) {
        UpdateLyrics(snap.position);
        if (lyrics.current >= 0) lyrics.glow[static_cast<size_t>(lyrics.current)].Snap(1);
    }
    if (CurrentKind() == TabKind::Lyrics) {
        if (lyrics.current >= 0) ScrollToLyric(lyrics.current, false);
        else scrollAnim.Snap(0);
        listScroll = scrollAnim.Target();
        // Новый текст проявляется так же, как список после смены вкладки
        if (playlistOpen && !mini) {
            tabAnim.Snap(motion.reduce ? 1.f : 0.f);
            tabAnim.To(1, motion.base, motion.out, timeline);
        }
    }
}

void PlayerView::LayoutLyrics() {
    const float width = std::max(40.f, frame[Elem::List].w - kLyricPadX * 2);
    if (width == lyrics.layoutWidth && lyrics.lineY.size() == lyrics.lines.size()) return;
    lyrics.layoutWidth = width;
    lyrics.lineY.resize(lyrics.lines.size());
    lyrics.lineH.resize(lyrics.lines.size());
    float y = kLyricsPadTop;
    for (size_t i = 0; i < lyrics.lines.size(); ++i) {
        const std::string& t = lyrics.lines[i];
        float h = kLyricBreak;
        if (!t.empty()) h = (host_ ? host_->MeasureWrapped(TextStyle::Lyric, t, width) : 24.f) + kLyricGap;
        lyrics.lineY[i] = y;
        lyrics.lineH[i] = h;
        y += h;
    }
    // Снизу - половина списка: последняя строка тоже доезжает до середины
    lyrics.contentHeight = y + frame[Elem::List].h * 0.5f;
}

void PlayerView::UpdateLyrics(double position) {
    if (lyrics.state != LyricsState::Synced || lyrics.times.empty()) return;
    const auto it = std::upper_bound(lyrics.times.begin(), lyrics.times.end(), position + kLyricLead);
    const int cur = static_cast<int>(it - lyrics.times.begin()) - 1;
    if (cur == lyrics.current) return;
    if (lyrics.current >= 0 && lyrics.current < static_cast<int>(lyrics.glow.size()))
        lyrics.glow[static_cast<size_t>(lyrics.current)].To(0, motion.slow, motion.out, timeline);
    if (cur >= 0 && cur < static_cast<int>(lyrics.glow.size()))
        lyrics.glow[static_cast<size_t>(cur)].To(1, motion.base, motion.out, timeline);
    lyrics.current = cur;
    if (WantsLyrics() && now >= lyrics.manualUntil && cur >= 0) ScrollToLyric(cur, !motion.reduce);
}

void PlayerView::ScrollToLyric(int line, bool animate) {
    if (line < 0 || line >= static_cast<int>(lyrics.lineY.size())) return;
    const float view = ListViewHeight();
    const float maxScroll = std::max(0.f, lyrics.contentHeight - view);
    // Звучащая строка - чуть выше середины: видно, что будет дальше
    const float target = std::clamp(
        lyrics.lineY[static_cast<size_t>(line)] + lyrics.lineH[static_cast<size_t>(line)] / 2 - view * 0.38f, 0.f,
        maxScroll);
    if (animate) scrollAnim.To(target, motion.slow, motion.out, timeline);
    else scrollAnim.Snap(target);
}

}  // namespace view
