#include "core/history.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>

#include "core/json.h"
#include "core/text.h"

namespace pm {

namespace {

constexpr double kMinRecord = 30;     // короче - не пишем (пролистал)
constexpr double kFullCap = 240;      // полное прослушивание: половина трека, но не больше 4 минут
constexpr double kMaxStep = 3;        // прирост позиции больше - перемотка, не считается
constexpr int64_t kDay = 86400;

int64_t FloorDiv(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

int LocalHour(int64_t t, int utcOffset) {
    return static_cast<int>(FloorDiv(t + utcOffset, 3600) - FloorDiv(t + utcOffset, kDay) * 24);
}

// Дни от 1970-01-01 <-> дата (алгоритм Говарда Хиннанта)
int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = FloorDiv(y, 400);
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void CivilFromDays(int64_t z, int64_t& y, unsigned& m, unsigned& d) {
    z += 719468;
    const int64_t era = FloorDiv(z, 146097);
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<int64_t>(yoe) + era * 400 + (m <= 2);
}

std::string ArtistName(const std::string& a) { return a.empty() ? std::string("Неизвестный исполнитель") : a; }

uint64_t SplitMix(uint64_t& s) {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

}  // namespace

bool PlayRecord::Full() const {
    if (duration <= 0) return listened >= kFullCap / 2;
    return listened >= std::min(duration * 0.5, kFullCap);
}

void ListenTracker::Begin(PlayRecord meta) {
    cur_ = std::move(meta);
    cur_.listened = 0;
    lastPos_ = -1;
    active_ = true;
}

void ListenTracker::Advance(double position, bool playing) {
    if (!active_) return;
    const double step = position - lastPos_;
    if (playing && lastPos_ >= 0 && step > 0 && step < kMaxStep) cur_.listened += step;
    lastPos_ = position;
}

bool ListenTracker::End(PlayRecord& out) {
    if (!active_) return false;
    active_ = false;
    if (cur_.listened < kMinRecord && !cur_.Full()) return false;
    out = cur_;
    return true;
}

bool AppendHistory(const std::filesystem::path& file, const PlayRecord& r) {
    json::Value v = json::Value::MakeObject();
    v["t"] = static_cast<double>(r.time);
    v["path"] = r.path;
    v["title"] = r.title;
    v["artist"] = r.artist;
    v["album"] = r.album;
    v["listened"] = std::round(r.listened * 10) / 10;
    v["duration"] = std::round(r.duration * 10) / 10;
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream f(file, std::ios::binary | std::ios::app);
    if (!f) return false;
    f << json::Dump(v, 0) << '\n';
    return static_cast<bool>(f);
}

std::vector<PlayRecord> LoadHistory(const std::filesystem::path& file) {
    std::vector<PlayRecord> out;
    std::ifstream f(file, std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        json::Value v;
        if (line.empty() || !json::Parse(line, v) || !v.IsObject()) continue;
        PlayRecord r;
        r.time = static_cast<int64_t>(v.Get("t").AsNumber(0));
        r.path = v.Get("path").AsString();
        r.title = v.Get("title").AsString();
        r.artist = v.Get("artist").AsString();
        r.album = v.Get("album").AsString();
        r.listened = v.Get("listened").AsNumber(0);
        r.duration = v.Get("duration").AsNumber(0);
        if (r.time > 0 && !r.path.empty()) out.push_back(std::move(r));
    }
    return out;
}

int64_t PeriodStart(int64_t now, int utcOffset, StatsPeriod period) {
    if (period == StatsPeriod::All) return 0;
    int64_t y;
    unsigned m, d;
    CivilFromDays(FloorDiv(now + utcOffset, kDay), y, m, d);
    const int64_t day = period == StatsPeriod::Month ? DaysFromCivil(y, m, 1) : DaysFromCivil(y, 1, 1);
    return day * kDay - utcOffset;
}

ListenSummary Summarize(const std::vector<PlayRecord>& history, int64_t from, int64_t to, int utcOffset,
                        size_t top) {
    ListenSummary s;
    std::map<std::string, StatItem> artists, tracks;
    std::set<int64_t> days;
    for (const PlayRecord& r : history) {
        if (r.time < from || r.time >= to) continue;
        const bool full = r.Full();
        s.seconds += r.listened;
        s.plays += full;
        s.byHour[static_cast<size_t>(LocalHour(r.time, utcOffset))] += r.listened;
        days.insert(FloorDiv(r.time + utcOffset, kDay));
        StatItem& a = artists[ArtistName(r.artist)];
        a.name = ArtistName(r.artist);
        a.plays += full;
        a.seconds += r.listened;
        StatItem& t = tracks[r.path];
        if (t.name.empty()) {
            // Без тегов - имя файла
            t.name = r.title.empty() ? text::PathToUtf8(text::Utf8ToPath(r.path).stem()) : r.title;
            t.sub = ArtistName(r.artist);
        }
        t.plays += full;
        t.seconds += r.listened;
    }
    s.tracks = static_cast<int>(tracks.size());
    s.artists = static_cast<int>(artists.size());
    s.activeDays = static_cast<int>(days.size());
    double best = 0;
    for (int h = 0; h < 24; ++h)
        if (s.byHour[static_cast<size_t>(h)] > best) {
            best = s.byHour[static_cast<size_t>(h)];
            s.topHour = h;
        }
    auto take = [top](std::map<std::string, StatItem>& m, bool byPlays) {
        std::vector<StatItem> v;
        for (auto& kv : m) v.push_back(std::move(kv.second));
        std::stable_sort(v.begin(), v.end(), [byPlays](const StatItem& a, const StatItem& b) {
            if (byPlays && a.plays != b.plays) return a.plays > b.plays;
            if (a.seconds != b.seconds) return a.seconds > b.seconds;
            return a.plays > b.plays;
        });
        if (v.size() > top) v.resize(top);
        return v;
    };
    s.topArtists = take(artists, false);  // исполнители - по времени
    s.topTracks = take(tracks, true);     // треки - по числу полных прослушиваний
    return s;
}

std::vector<size_t> BuildWave(const std::vector<WaveCandidate>& tracks, const std::vector<PlayRecord>& history,
                              int64_t now, int utcOffset, size_t count, uint64_t seed,
                              const std::set<std::string>& exclude) {
    // Что известно из истории: полные прослушивания и последнее время по треку, время по исполнителю,
    // сколько раз исполнитель звучал в это же время суток (±2 часа)
    struct PathInfo {
        int full = 0;
        int64_t last = 0;
    };
    struct ArtistInfo {
        double seconds = 0;
        int records = 0, nearHour = 0;
    };
    std::map<std::string, PathInfo> byPath;
    std::map<std::string, ArtistInfo> byArtist;
    const int hourNow = LocalHour(now, utcOffset);
    double maxArtist = 0;
    for (const PlayRecord& r : history) {
        PathInfo& p = byPath[r.path];
        p.full += r.Full();
        p.last = std::max(p.last, r.time);
        ArtistInfo& a = byArtist[r.artist];
        a.seconds += r.listened;
        ++a.records;
        const int dh = std::abs(LocalHour(r.time, utcOffset) - hourNow);
        if (std::min(dh, 24 - dh) <= 2) ++a.nearHour;
        maxArtist = std::max(maxArtist, a.seconds);
    }

    std::vector<double> weight(tracks.size(), 0);
    for (size_t i = 0; i < tracks.size(); ++i) {
        const WaveCandidate& c = tracks[i];
        if (exclude.count(c.path)) continue;
        const auto pit = byPath.find(c.path);
        const auto ait = byArtist.find(c.artist);
        const int full = pit != byPath.end() ? pit->second.full : 0;
        const int64_t last = std::max(c.lastPlayed, pit != byPath.end() ? pit->second.last : 0);
        double artist = 0, hour = 0;
        if (ait != byArtist.end() && !c.artist.empty()) {
            artist = maxArtist > 0 ? ait->second.seconds / maxArtist : 0;
            // Доля прослушиваний исполнителя в это время суток против равномерной (5 часов из 24)
            if (ait->second.records >= 3) {
                const double frac = double(ait->second.nearHour) / ait->second.records;
                hour = std::clamp((frac - 5.0 / 24) / (1 - 5.0 / 24), 0.0, 1.0);
            }
        }
        double w;
        if (last == 0 && c.plays == 0 && full == 0) {
            w = 0.35 + 0.6 * artist + 0.8 * hour;  // ещё не слушал: открытие нового у любимых исполнителей
        } else {
            const double days = last > 0 ? double(now - last) / kDay : 365;
            const double forgotten = std::clamp(days / 30, 0.0, 1.0);
            const double love = std::log2(1 + c.plays) + 0.5 * std::log2(1 + full);
            w = 0.2 + 0.6 * love * (0.4 + 0.6 * forgotten) + 0.8 * artist + 0.8 * hour;
            if (days < 1) w *= 0.05;  // только что звучал
            else if (days < 3) w *= 0.3;
        }
        weight[i] = w;
    }

    std::vector<size_t> out;
    uint64_t rng = seed ^ 0x5049434B57415645ull;
    const std::string* prevArtist = nullptr;
    while (out.size() < count) {
        double total = 0;
        for (double w : weight) total += w;
        if (total <= 0) break;
        size_t pick = 0;
        for (int attempt = 0; attempt < 6; ++attempt) {
            double r = double(SplitMix(rng) >> 11) / double(1ull << 53) * total;
            pick = weight.size() - 1;
            for (size_t i = 0; i < weight.size(); ++i) {
                if (weight[i] <= 0) continue;
                if (r < weight[i]) {
                    pick = i;
                    break;
                }
                r -= weight[i];
            }
            while (weight[pick] <= 0 && pick > 0) --pick;  // округление в конце суммы
            // Один исполнитель два раза подряд - только если иначе никак
            if (!prevArtist || tracks[pick].artist.empty() || tracks[pick].artist != *prevArtist) break;
        }
        out.push_back(pick);
        weight[pick] = 0;
        prevArtist = &tracks[pick].artist;
    }
    return out;
}

}  // namespace pm
