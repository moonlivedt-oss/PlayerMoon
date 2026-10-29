// История прослушиваний, страница «Итоги» в панели и «Волна» (часть App, основное - в app.cpp).
// Логика подсчёта и подбора - переносимая, в core/history.*; здесь - время Windows и вид.

#include <cmath>
#include <ctime>
#include <set>

#include "core/text.h"
#include "ui/app.h"

namespace ui {

namespace {

// Смещение местного времени от UTC сейчас, секунды (с учётом летнего времени)
int UtcOffset() {
    const time_t t = time(nullptr);
    tm local{};
    localtime_s(&local, &t);
    return static_cast<int>(_mkgmtime(&local) - t);
}

std::string Hours(double seconds) {
    const long m = std::lround(seconds / 60);
    if (m == 0) return "меньше минуты";
    if (m < 60) return std::to_string(m) + " мин";
    return std::to_string(m / 60) + " ч " + std::to_string(m % 60) + " мин";
}

// «1 трек», «3 трека», «5 треков»
std::string Plural(int n, const char* one, const char* few, const char* many) {
    const int d = n % 10, h = n % 100;
    const char* w = (d == 1 && h != 11) ? one : (d >= 2 && d <= 4 && (h < 12 || h > 14)) ? few : many;
    return std::to_string(n) + " " + w;
}

std::string Plays(int n) { return Plural(n, "прослушивание", "прослушивания", "прослушиваний"); }

const char* const kMonths[] = {"Январь", "Февраль", "Март",     "Апрель",  "Май",    "Июнь",
                               "Июль",   "Август",  "Сентябрь", "Октябрь", "Ноябрь", "Декабрь"};

enum StatsActions { kStatsMonth = 1, kStatsYear, kStatsAll, kStatsWave, kStatsClose };

}  // namespace

void App::EnsureHistory() {
    if (historyLoaded_) return;
    history_ = pm::LoadHistory(historyPath_);
    historyLoaded_ = true;
}

void App::BeginListen() {
    const pm::NowPlaying& np = player_.Current();
    pm::PlayRecord r;
    r.time = static_cast<int64_t>(time(nullptr));
    r.path = ::text::PathToUtf8(np.path);
    r.title = np.title;
    r.artist = np.tags.artist;
    r.album = np.tags.album;
    r.duration = player_.GetState().duration > 0 ? player_.GetState().duration : np.tags.duration;
    listen_.Begin(std::move(r));
}

void App::FinishListen() {
    pm::PlayRecord r;
    if (!listen_.End(r) || historyPath_.empty()) return;
    pm::AppendHistory(historyPath_, r);
    if (historyLoaded_) history_.push_back(std::move(r));
    if (statsOpen_) ++statsRevision_;  // «Итоги» открыты - обновить цифры
}

void App::ShowStats() {
    EnsureHistory();
    statsOpen_ = true;
    ++statsRevision_;
    SyncPages();
    view_.ShowPanelTab(view_.PageTab(static_cast<int>(plugins_.Pages().size())), Now());
    Invalidate();
}

view::PanelPage App::BuildStatsPage() {
    view::PanelPage page;
    page.title = "Итоги";
    page.actions = {{kStatsMonth, "Месяц", 0}, {kStatsYear, "Год", 0}, {kStatsAll, "Всё время", 0},
                    {kStatsWave, "Волна", 0}, {kStatsClose, "Закрыть", 0}};
    page.emptyText = "Пока нечего считать: история копится, когда трек звучит дольше 30 секунд";
    const int64_t now = static_cast<int64_t>(time(nullptr));
    const int offset = UtcOffset();
    const pm::ListenSummary s =
        pm::Summarize(history_, pm::PeriodStart(now, offset, statsPeriod_), now + 1, offset, 5);
    if (s.seconds <= 0) return page;

    const time_t t = static_cast<time_t>(now);
    tm local{};
    localtime_s(&local, &t);
    std::string period = statsPeriod_ == pm::StatsPeriod::Month ? std::string(kMonths[local.tm_mon]) + " " +
                                                                      std::to_string(local.tm_year + 1900)
                         : statsPeriod_ == pm::StatsPeriod::Year ? std::to_string(local.tm_year + 1900) + " год"
                                                                 : std::string("Всё время");
    int id = 1;
    view::PanelRow head;
    head.id = id++;
    head.title = "Музыка: " + Hours(s.seconds);
    head.status = period;
    head.subtitle = Plays(s.plays) + ", " + Plural(s.tracks, "трек", "трека", "треков") + ", " +
                    Plural(s.artists, "исполнитель", "исполнителя", "исполнителей");
    page.rows.push_back(std::move(head));
    if (s.topHour >= 0) {
        view::PanelRow hour;
        hour.id = id++;
        hour.title = "Любимое время: " + std::to_string(s.topHour) + ":00-" + std::to_string((s.topHour + 1) % 24) + ":00";
        hour.subtitle = "Дней с музыкой: " + std::to_string(s.activeDays);
        page.rows.push_back(std::move(hour));
    }

    const double maxArtist = s.topArtists.empty() ? 1 : std::max(1.0, s.topArtists[0].seconds);
    for (size_t i = 0; i < s.topArtists.size(); ++i) {
        const pm::StatItem& a = s.topArtists[i];
        view::PanelRow row;
        row.id = id++;
        row.title = std::to_string(i + 1) + ". " + a.name;
        row.subtitle = i == 0 ? "Исполнитель периода" : "Исполнитель";
        row.status = Hours(a.seconds) + ", " + Plays(a.plays);
        row.progress = static_cast<float>(a.seconds / maxArtist);
        page.rows.push_back(std::move(row));
    }
    const int maxPlays = s.topTracks.empty() ? 1 : std::max(1, s.topTracks[0].plays);
    for (size_t i = 0; i < s.topTracks.size(); ++i) {
        const pm::StatItem& tr = s.topTracks[i];
        view::PanelRow row;
        row.id = id++;
        row.title = std::to_string(i + 1) + ". " + tr.name;
        row.subtitle = tr.sub;
        row.status = Plays(tr.plays) + ", " + Hours(tr.seconds);
        row.progress = static_cast<float>(tr.plays) / static_cast<float>(maxPlays);
        page.rows.push_back(std::move(row));
    }
    return page;
}

void App::StatsAction(int actionId) {
    switch (actionId) {
        case kStatsMonth: statsPeriod_ = pm::StatsPeriod::Month; break;
        case kStatsYear: statsPeriod_ = pm::StatsPeriod::Year; break;
        case kStatsAll: statsPeriod_ = pm::StatsPeriod::All; break;
        case kStatsWave: StartWave(); break;
        case kStatsClose: statsOpen_ = false; break;
        default: return;
    }
    ++statsRevision_;
    SyncPages();
    Invalidate();
}

void App::StartWave() {
    const auto& lib = library_.Tracks();
    if (lib.empty()) {
        view_.ShowToast("Для Волны нужна библиотека: добавьте папку с музыкой", Now());
        Invalidate();
        return;
    }
    EnsureHistory();
    std::vector<pm::WaveCandidate> cands;
    cands.reserve(lib.size());
    for (const auto& t : lib) cands.push_back({::text::PathToUtf8(t.path), t.meta.artist, t.plays, t.lastPlayed});
    const int64_t now = static_cast<int64_t>(time(nullptr));
    const auto picks = pm::BuildWave(cands, history_, now, UtcOffset(), 25, static_cast<uint64_t>(now) ^ GetTickCount64());
    if (picks.empty()) return;
    std::vector<pm::PlaylistEntry> entries;
    for (size_t i : picks) entries.push_back({lib[i].path, lib[i].meta});
    player_.LoadTracks(entries);
    playlistFromLibrary_ = false;
    waveMode_ = true;
    RebuildRows();
    player_.PlayIndex(0);
    view_.ShowToast("Волна: любимое, забытое и музыка этого часа", Now());
    MarkDirty();
    Invalidate();
}

void App::ExtendWave() {
    const auto& lib = library_.Tracks();
    if (!waveMode_ || lib.empty()) return;
    const pm::Playlist& pl = player_.GetPlaylist();
    // Не повторяем то, что уже в очереди; если библиотека кончилась - только последние 30
    std::set<std::string> exclude, recent;
    for (int i = 0; i < pl.Size(); ++i) {
        const std::string p = ::text::PathToUtf8(pl.At(i).path);
        exclude.insert(p);
        if (i >= pl.Size() - 30) recent.insert(p);
    }
    std::vector<pm::WaveCandidate> cands;
    for (const auto& t : lib) cands.push_back({::text::PathToUtf8(t.path), t.meta.artist, t.plays, t.lastPlayed});
    const int64_t now = static_cast<int64_t>(time(nullptr));
    const uint64_t seed = static_cast<uint64_t>(now) ^ GetTickCount64();
    auto picks = pm::BuildWave(cands, history_, now, UtcOffset(), 20, seed, exclude);
    if (picks.empty()) picks = pm::BuildWave(cands, history_, now, UtcOffset(), 20, seed, recent);
    if (picks.empty()) return;
    std::vector<pm::PlaylistEntry> entries;
    for (size_t i : picks) entries.push_back({lib[i].path, lib[i].meta});
    player_.LoadTracks(entries, true);
    RebuildRows();
    MarkDirty();
}

}  // namespace ui
