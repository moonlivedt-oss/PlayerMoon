// История прослушиваний, «Итоги» и «Волна»
#include <map>

#include "core/history.h"
#include "tests/check.h"
#include "tests/wav_util.h"

namespace {

pm::PlayRecord Rec(int64_t t, const std::string& path, const std::string& artist, double listened,
                   double duration = 200) {
    pm::PlayRecord r;
    r.time = t;
    r.path = path;
    r.title = path;
    r.artist = artist;
    r.listened = listened;
    r.duration = duration;
    return r;
}

// 2025-10-15 12:00 UTC
constexpr int64_t kNow = 1760529600;

}  // namespace

TEST(ListenTrackerCountsRealListening) {
    pm::ListenTracker t;
    pm::PlayRecord meta;
    meta.path = "a.mp3";
    meta.duration = 100;
    t.Begin(meta);
    // 40 секунд игры, перемотка вперёд на 30 с не считается, пауза - тоже
    for (int i = 0; i <= 40; ++i) t.Advance(i, true);
    t.Advance(70, true);
    for (int i = 70; i <= 75; ++i) t.Advance(i, false);
    for (int i = 75; i <= 80; ++i) t.Advance(i, true);
    pm::PlayRecord out;
    CHECK(t.End(out));
    CHECK_NEAR(out.listened, 45, 1e-9);
    CHECK(!out.Full());  // половина от 100 с - 50 с
    // Пролистал за 10 секунд - записи нет
    t.Begin(meta);
    for (int i = 0; i <= 10; ++i) t.Advance(i, true);
    CHECK(!t.End(out) && !t.Active());
    // Длинный трек: полное прослушивание - 4 минуты, а не половина часа
    pm::PlayRecord longOne = Rec(0, "x", "y", 241, 3600);
    CHECK(longOne.Full());
    CHECK(!Rec(0, "x", "y", 99, 200).Full() && Rec(0, "x", "y", 100, 200).Full());
}

TEST(HistoryFileAppendAndLoad) {
    const auto dir = tests::TempDir("history");
    const auto file = dir / "history.jsonl";
    pm::PlayRecord r = Rec(kNow, "D:/Музыка/песня.mp3", "Кварц", 123.45);
    r.album = "Окна";
    CHECK(pm::AppendHistory(file, r));
    CHECK(pm::AppendHistory(file, Rec(kNow + 10, "b.flac", "", 50)));
    // Оборванная последняя строка (выключили питание) пропускается
    {
        std::ofstream f(file, std::ios::binary | std::ios::app);
        f << "{\"t\":17605";
    }
    const auto h = pm::LoadHistory(file);
    CHECK(h.size() == 2);
    if (h.size() != 2) return;
    CHECK(h[0].path == r.path && h[0].artist == "Кварц" && h[0].album == "Окна" && h[0].time == kNow);
    CHECK_NEAR(h[0].listened, 123.5, 1e-9);
    CHECK(h[1].artist.empty() && h[1].path == "b.flac");
    CHECK(pm::LoadHistory(dir / "нет.jsonl").empty());
}

TEST(SummaryPeriodsAndTops) {
    // Начало месяца и года по местному времени (UTC+10, Владивосток)
    const int vl = 10 * 3600;
    CHECK(pm::PeriodStart(kNow, vl, pm::StatsPeriod::Month) == 1759240800);  // 2025-10-01 00:00 +10
    CHECK(pm::PeriodStart(kNow, vl, pm::StatsPeriod::Year) == 1767189600 - 365 * 86400);  // 2025-01-01 00:00 +10
    CHECK(pm::PeriodStart(kNow, 0, pm::StatsPeriod::All) == 0);
    // 31 декабря 23:30 по местному - ещё старый год
    const int64_t nye = 1767189600 - 1800;
    CHECK(pm::PeriodStart(nye, vl, pm::StatsPeriod::Year) == 1767189600 - 365 * 86400);
    CHECK(pm::PeriodStart(nye + 3600, vl, pm::StatsPeriod::Year) == 1767189600);

    std::vector<pm::PlayRecord> h;
    // Кварц: 3 полных прослушивания одного трека; Море: один длинный; одно - в прошлом месяце
    for (int i = 0; i < 3; ++i) h.push_back(Rec(kNow - 3600 * i, "q1", "Кварц", 150));
    h.push_back(Rec(kNow - 86400, "m1", "Море", 600, 600));
    h.push_back(Rec(kNow - 86400 + 700, "m2", "Море", 40));  // не полное
    h.push_back(Rec(kNow - 40 * 86400, "old", "Старое", 200));
    const auto month = pm::Summarize(h, pm::PeriodStart(kNow, vl, pm::StatsPeriod::Month), kNow + 1, vl, 10);
    CHECK_NEAR(month.seconds, 450 + 640, 1e-9);
    CHECK(month.plays == 4 && month.tracks == 3 && month.artists == 2 && month.activeDays == 2);
    CHECK(month.topArtists.size() == 2 && month.topArtists[0].name == "Море");  // по времени
    CHECK(month.topTracks.size() == 3 && month.topTracks[0].name == "q1" && month.topTracks[0].plays == 3);
    CHECK(month.topTracks[0].sub == "Кварц");
    // Любимый час - по местному времени: 12:00 UTC + 10 = 22 часа (600 с вчера в то же время)
    CHECK(month.topHour == 22);
    const auto all = pm::Summarize(h, 0, kNow + 1, vl, 1);
    CHECK(all.artists == 3 && all.topArtists.size() == 1);
    CHECK(pm::Summarize({}, 0, kNow, 0, 5).topHour == -1);
}

TEST(WavePrefersForgottenFavorites) {
    std::vector<pm::WaveCandidate> lib;
    // 0: любимый, давно не слушал; 1: любимый, звучал час назад; 2-9: остальное, слушалось пару раз месяц назад
    lib.push_back({"fav_old", "A", 30, kNow - 60 * 86400});
    lib.push_back({"fav_now", "B", 30, kNow - 3600});
    for (int i = 2; i < 10; ++i) lib.push_back({"t" + std::to_string(i), "C" + std::to_string(i), 2, kNow - 30 * 86400});
    std::vector<pm::PlayRecord> h;
    std::map<size_t, int> firsts, rank;
    for (uint64_t seed = 1; seed <= 400; ++seed) {
        const auto w = pm::BuildWave(lib, h, kNow, 0, 5, seed);
        CHECK(w.size() == 5);
        if (w.empty()) continue;
        ++firsts[w[0]];
        for (size_t k = 0; k < w.size(); ++k)
            if (w[k] < 2) rank[w[k]] += static_cast<int>(k);
    }
    // Забытое любимое чаще всех открывает волну; только что игравшее - почти никогда
    for (size_t i = 1; i < 10; ++i) CHECK(firsts[0] > firsts[i]);
    CHECK(firsts[1] < 10);
    // Без повторов и в пределах списка; exclude убирает уже стоящие в очереди
    const auto w = pm::BuildWave(lib, h, kNow, 0, 100, 7, {"fav_old"});
    CHECK(w.size() == 9);
    std::set<size_t> uniq(w.begin(), w.end());
    CHECK(uniq.size() == w.size() && !uniq.count(0));
    // Одинаковый seed - одинаковая волна
    CHECK(pm::BuildWave(lib, h, kNow, 0, 5, 42) == pm::BuildWave(lib, h, kNow, 0, 5, 42));
    CHECK(pm::BuildWave({}, h, kNow, 0, 5, 1).empty());
}

TEST(WaveFollowsTimeOfDayAndArtists) {
    // Ночью слушают «Ночь», днём - «День»; сейчас 23:00 - волна тянется к «Ночи»
    std::vector<pm::WaveCandidate> lib;
    for (int i = 0; i < 6; ++i) lib.push_back({"n" + std::to_string(i), "Ночь", 0, 0});
    for (int i = 0; i < 6; ++i) lib.push_back({"d" + std::to_string(i), "День", 0, 0});
    std::vector<pm::PlayRecord> h;
    const int64_t midnight = kNow - 12 * 3600;  // 00:00 UTC сегодня
    for (int d = 1; d <= 10; ++d) {
        h.push_back(Rec(midnight - d * 86400 + 23 * 3600, "old_n", "Ночь", 200));
        h.push_back(Rec(midnight - d * 86400 + 13 * 3600, "old_d", "День", 200));
    }
    // Первый трек волны (дальше исполнители чередуются, и доля выравнивается)
    int night = 0, total = 0;
    for (uint64_t seed = 1; seed <= 400; ++seed) {
        const auto w = pm::BuildWave(lib, h, midnight + 23 * 3600, 0, 1, seed);
        for (size_t k : w) {
            night += lib[k].artist == "Ночь";
            ++total;
        }
    }
    CHECK(total == 400 && night > total * 0.58);
    // Подряд один исполнитель не идёт, пока есть другие
    const auto w = pm::BuildWave(lib, h, midnight + 23 * 3600, 0, 12, 3);
    int same = 0;
    for (size_t k = 1; k < w.size(); ++k) same += lib[w[k]].artist == lib[w[k - 1]].artist;
    CHECK(same <= 2);
}
