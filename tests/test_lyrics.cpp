// Тексты песен: разбор LRC, строка по времени, ответ LRCLIB, кэш и порядок поиска (сеть - подделка)
#include <atomic>
#include <chrono>
#include <thread>

#include "core/lyrics.h"
#include "core/text.h"
#include "tests/check.h"
#include "tests/wav_util.h"

TEST(LrcParseAndLineAt) {
    const pm::Lyrics l = pm::ParseLrc(
        "\xEF\xBB\xBF[ar:Кварц]\r\n[ti:Синий час]\r\n[offset:+500]\r\n"
        "[00:12.50]Первая строка\r\n"
        "[00:05.00][01:02.250]Припев <00:05.50>со <00:06.00>словами\r\n"
        "[00:20]\r\n"
        "[00:21.1]Последняя\r\n[03:00.00]\r\n");
    CHECK(l.synced && l.lines.size() == 5);
    if (l.lines.size() != 5) return;
    // offset +500 мс - всё на полсекунды раньше; строки отсортированы, метки слов убраны
    CHECK_NEAR(l.lines[0].time, 4.5, 1e-9);
    CHECK(l.lines[0].text == "Припев со словами");
    CHECK_NEAR(l.lines[1].time, 12.0, 1e-9);
    CHECK(l.lines[1].text == "Первая строка");
    CHECK(l.lines[2].text.empty());  // пауза между куплетами остаётся
    CHECK_NEAR(l.lines[3].time, 20.6, 1e-9);
    CHECK_NEAR(l.lines[4].time, 61.75, 1e-9);
    CHECK(pm::LyricLineAt(l, 1.0) == -1);
    CHECK(pm::LyricLineAt(l, 4.5) == 0);
    CHECK(pm::LyricLineAt(l, 15) == 1);
    CHECK(pm::LyricLineAt(l, 1000) == 4);
    // Обратно в LRC и снова - то же
    const pm::Lyrics back = pm::ParseLrc(pm::ToLrc(l));
    CHECK(back.synced && back.lines.size() == l.lines.size() && back.lines[0].text == l.lines[0].text);
    CHECK_NEAR(back.lines[4].time, 61.75, 0.006);

    // Простой текст: без меток, пустые строки по краям убраны
    const pm::Lyrics p = pm::ParseLrc("\n\nРаз\n\nДва [не метка]\n\n");
    CHECK(!p.synced && p.lines.size() == 3 && p.lines[0].text == "Раз" && p.lines[1].text.empty() &&
          p.lines[2].text == "Два [не метка]");
    CHECK(pm::LyricLineAt(p, 10) == -1);
    // Мусор не роняет
    CHECK(pm::ParseLrc("[").lines.size() == 1 && !pm::ParseLrc("[99:99.99]x").synced);
    CHECK(pm::ParseLrc("").Empty());
}

TEST(LrclibQueryAndResponse) {
    pm::LyricsRequest r;
    r.title = "Кварц - Синий час";
    r.album = "Окна & Двери";
    r.duration = 215.4;
    pm::SplitArtistTitle(r);
    CHECK(r.artist == "Кварц" && r.title == "Синий час");
    CHECK(pm::LrclibGetQuery(r) ==
          "/api/get?track_name=%D0%A1%D0%B8%D0%BD%D0%B8%D0%B9%20%D1%87%D0%B0%D1%81&artist_name=%D0%9A%D0%B2%D0%B0%D1%80%D1%86"
          "&album_name=%D0%9E%D0%BA%D0%BD%D0%B0%20%26%20%D0%94%D0%B2%D0%B5%D1%80%D0%B8&duration=215");

    pm::Lyrics l;
    CHECK(pm::ParseLrclibResponse(R"({"id":1,"instrumental":false,"plainLyrics":"a\nb",
        "syncedLyrics":"[00:01.00]a\n[00:02.00]b"})", 0, l) == pm::LyricsStatus::Found);
    CHECK(l.synced && l.lines.size() == 2);
    CHECK(pm::ParseLrclibResponse(R"({"instrumental":true,"plainLyrics":null,"syncedLyrics":null})", 0, l) ==
          pm::LyricsStatus::Instrumental);
    CHECK(pm::ParseLrclibResponse(R"({"plainLyrics":"Только текст","syncedLyrics":null})", 0, l) ==
          pm::LyricsStatus::Found);
    CHECK(!l.synced && l.lines.size() == 1);
    // Поиск: другой трек по длительности отбрасывается, синхронный лучше простого
    const char* search = R"([
        {"duration":100,"syncedLyrics":"[00:01.00]чужой"},
        {"duration":214,"plainLyrics":"простой","syncedLyrics":null},
        {"duration":217,"plainLyrics":"x","syncedLyrics":"[00:01.00]наш"}])";
    CHECK(pm::ParseLrclibResponse(search, 215.4, l) == pm::LyricsStatus::Found);
    CHECK(l.synced && l.lines.size() == 1 && l.lines[0].text == "наш");
    CHECK(pm::ParseLrclibResponse("[]", 215, l) == pm::LyricsStatus::NotFound);
    CHECK(pm::ParseLrclibResponse("<html>", 215, l) == pm::LyricsStatus::NetworkError);
}

TEST(LyricsFindOrderAndCache) {
    const auto dir = tests::TempDir("lyrics");
    const auto cache = dir / "cache";
    pm::LyricsRequest r;
    r.serial = 7;
    r.track = dir / "Кварц - Синий час.mp3";
    r.title = "Кварц - Синий час";
    r.duration = 200;
    std::vector<std::string> asked;
    int code = 404;
    std::string answer;
    pm::HttpGet get = [&](const std::string& q, std::string& body) {
        asked.push_back(q);
        if (q.rfind("/api/search", 0) == 0) {
            body = answer;
            return answer.empty() ? 200 : 200;
        }
        body = code == 200 ? answer : R"({"code":404})";
        return code;
    };

    // Нет нигде: get, затем search, «не найдено» в кэше
    answer = "[]";
    pm::LyricsResult res = pm::FindLyrics(r, cache, get, 1000);
    CHECK(res.serial == 7 && res.status == pm::LyricsStatus::NotFound && asked.size() == 2);
    CHECK(asked.size() == 2 && asked[0].rfind("/api/get?", 0) == 0 && asked[1].rfind("/api/search?", 0) == 0);
    // Повтор через день - сеть не трогаем; через три недели - спрашиваем снова
    asked.clear();
    res = pm::FindLyrics(r, cache, get, 1000 + 86400);
    CHECK(res.status == pm::LyricsStatus::NotFound && asked.empty());
    code = 200;
    answer = R"({"syncedLyrics":"[00:01.00]Из сети\n[00:03.00]Вторая"})";
    res = pm::FindLyrics(r, cache, get, 1000 + 21 * 86400);
    CHECK(res.status == pm::LyricsStatus::Found && res.lyrics.synced && asked.size() == 1);
    // Теперь из кэша, без сети
    asked.clear();
    res = pm::FindLyrics(r, cache, get, 5000000);
    CHECK(res.status == pm::LyricsStatus::Found && res.lyrics.lines.size() == 2 && asked.empty());
    CHECK(res.lyrics.lines.size() == 2 && res.lyrics.lines[0].text == "Из сети");
    // Файл рядом с треком главнее кэша
    tests::WriteBytes(dir / "Кварц - Синий час.lrc", std::vector<uint8_t>{'[', '0', '0', ':', '0', '2', ']', 'M'});
    res = pm::FindLyrics(r, cache, get, 5000000);
    CHECK(res.status == pm::LyricsStatus::Found && res.lyrics.lines.size() == 1 && res.lyrics.lines[0].text == "M");
    // Нет сети - ошибка и без записи в кэш
    pm::LyricsRequest other = r;
    other.track.clear();
    other.title = "Другая";
    other.artist = "Кто-то";
    asked.clear();
    pm::HttpGet offline = [&](const std::string& q, std::string&) {
        asked.push_back(q);
        return 0;
    };
    res = pm::FindLyrics(other, cache, offline, 1000);
    CHECK(res.status == pm::LyricsStatus::NetworkError && asked.size() == 1);
    std::error_code ec;
    CHECK(!std::filesystem::exists(cache / pm::LyricsCacheName(other), ec));
    // Ключ кэша без учёта регистра
    pm::LyricsRequest upper = other;
    upper.title = "ДРУГАЯ";
    upper.artist = "КТО-ТО";
    CHECK(pm::LyricsCacheName(upper) == pm::LyricsCacheName(other));
}

TEST(LyricsServiceLatestOnly) {
    const auto dir = tests::TempDir("lyrics_service");
    std::atomic<int> calls{0}, ready{0};
    std::atomic<bool> release{false};
    pm::LyricsService svc;
    svc.Start(dir, [&](const std::string&, std::string& body) {
        ++calls;
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        body = R"({"syncedLyrics":"[00:01.00]x"})";
        return 200;
    }, [&] { ++ready; });
    pm::LyricsRequest r;
    r.artist = "A";
    for (uint64_t i = 1; i <= 3; ++i) {
        r.serial = i;
        r.title = "T" + std::to_string(i);
        svc.Request(r);
        if (i == 1) {  // первый уже в работе, пока шлём остальные
            while (calls.load() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    release = true;
    pm::LyricsResult res;
    bool got = false;
    for (int i = 0; i < 2000 && !got; ++i) {
        if (ready.load() > 0 && svc.Take(res) && res.serial == 3) got = true;
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // Первый (устаревший) результат отброшен, второй пропущен - сразу третий
    CHECK(got && res.status == pm::LyricsStatus::Found);
    CHECK(calls.load() == 2);
    svc.Stop();
}
