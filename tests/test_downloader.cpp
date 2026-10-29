// Плагин загрузки: какие ссылки понимаем, аргументы yt-dlp, разбор его вывода.
#include <algorithm>
#include <cmath>
#include <string>

#include "plugins/downloader/ytdlp.h"
#include "tests/check.h"

TEST(DownloaderSourcesAndUrls) {
    using dl::Source;
    CHECK(dl::DetectSource("https://music.youtube.com/watch?v=abc&list=x") == Source::YouTubeMusic);
    CHECK(dl::DetectSource("https://www.youtube.com/watch?v=abc") == Source::YouTube);
    CHECK(dl::DetectSource("https://youtu.be/abc") == Source::YouTube);
    CHECK(dl::DetectSource("https://music.yandex.ru/album/1/track/2") == Source::Yandex);
    CHECK(dl::DetectSource("https://music.yandex.com/album/1") == Source::Yandex);
    CHECK(dl::DetectSource("https://soundcloud.com/a/b") == Source::Other);
    CHECK(dl::DetectSource("ftp://x") == Source::Invalid && dl::DetectSource("музыка") == Source::Invalid);
    CHECK(std::string(dl::SourceName(Source::Yandex)) == "Яндекс Музыка");

    // Из текста буфера: несколько ссылок, хвостовая пунктуация, повторы
    const auto urls = dl::ExtractUrls("Послушай: https://music.yandex.ru/album/9/track/7, и ещё\n"
                                      "\"https://youtu.be/xyz\" (https://youtu.be/xyz).");
    CHECK(urls.size() == 2);
    CHECK(urls.size() == 2 && urls[0] == "https://music.yandex.ru/album/9/track/7" && urls[1] == "https://youtu.be/xyz");
    CHECK(dl::ExtractUrls("просто текст").empty());
}

TEST(DownloaderArgsAndQuoting) {
    dl::Options o;
    o.outDir = "C:/Users/Музыка/PlayerMoon";
    o.browser = "firefox";
    auto a = dl::BuildArgs("https://youtu.be/x", o);
    auto has = [&](const std::string& v) { return std::find(a.begin(), a.end(), v) != a.end(); };
    CHECK(has("--no-simulate") && has("--progress") && has("-x") && has("mp3") && has("firefox") && has("bestaudio/best"));
    CHECK(a.back() == "https://youtu.be/x" && a[a.size() - 2] == "--");  // ссылка не примется за ключ
    o.mp3 = false;
    o.browser.clear();
    a = dl::BuildArgs("https://youtu.be/x", o);
    CHECK(!has("-x") && !has("--cookies-from-browser") && has("bestaudio[ext=m4a]/bestaudio/best"));

    // Кавычки по правилам Windows
    CHECK(dl::QuoteArgs({"a", "b c"}) == "a \"b c\"");
    CHECK(dl::QuoteArgs({"say \"hi\""}) == "\"say \\\"hi\\\"\"");
    CHECK(dl::QuoteArgs({"C:\\dir with space\\"}) == "\"C:\\dir with space\\\\\"");
    CHECK(dl::QuoteArgs({""}) == "\"\"");
}

TEST(DownloaderParseOutput) {
    using T = dl::Event::Type;
    auto e = dl::ParseLine("PMITEM|3|12|Песня | с чертой|Исполнитель\r\n");
    CHECK(e.type == T::Item && e.item == 3 && e.items == 12);
    CHECK(e.title == "Песня | с чертой" && e.artist == "Исполнитель");
    e = dl::ParseLine("PMITEM|NA|NA|Одиночный трек|NA");
    CHECK(e.type == T::Item && e.item == 0 && e.items == 0 && e.artist.empty());
    // Проценты с цветом терминала и пробелами
    e = dl::ParseLine("PMPROG|\x1b[0;94m 42.5%\x1b[0m|  2.10MiB/s|00:05");
    CHECK(e.type == T::Progress && std::abs(e.percent - 42.5) < 1e-9 && e.speed == "2.10MiB/s" && e.eta == "00:05");
    e = dl::ParseLine("PMPROG|100.0%|NA|NA");
    CHECK(e.type == T::Progress && e.speed.empty());
    e = dl::ParseLine("PMFILE|C:\\Музыка\\Артист - Трек.mp3");
    CHECK(e.type == T::File && e.text == "C:\\Музыка\\Артист - Трек.mp3");
    e = dl::ParseLine("ERROR: [youtube] abc: Video unavailable");
    CHECK(e.type == T::Error && e.text == "abc: Video unavailable");
    CHECK(dl::ParseLine("WARNING: something").type == T::Warning);
    CHECK(dl::ParseLine("[ExtractAudio] Destination: x.mp3").type == T::Converting);
    CHECK(dl::ParseLine("[youtube] Extracting URL").type == T::None);

    CHECK(dl::HumanSpeed("2.10MiB/s") == "2.1 МБ/с");
    CHECK(dl::HumanSpeed("512.00KiB/s") == "512 КБ/с");
    CHECK(dl::HumanSpeed("?") == "?");
}
