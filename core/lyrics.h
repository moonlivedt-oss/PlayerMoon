#pragma once
// Тексты песен: разбор LRC (синхронный текст с метками времени) и простого текста, поиск строки
// по времени, ответ LRCLIB (lrclib.net, без ключа) и кэш найденного в папке данных.
//
// Где ищется текст (FindLyrics): файл .lrc рядом с треком (то же имя), затем кэш, затем LRCLIB:
// /api/get по названию, исполнителю, альбому и длительности, если не нашлось - /api/search.
// «Не найдено» тоже кэшируется и проверяется заново через две недели.
//
// Сеть подставляется функцией HttpGet (в Windows - WinHTTP, в тестах - подделка), поэтому весь
// код переносимый. Поиск идёт в фоновом потоке LyricsService; результат забирает поток UI.

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace pm {

struct LyricLine {
    double time = -1;  // секунды; -1 - у простого текста
    std::string text;
};

struct Lyrics {
    bool synced = false;
    std::vector<LyricLine> lines;
    bool Empty() const { return lines.empty(); }
};

// LRC: [мм:сс.xx] (несколько меток на строку - повтор строки), [offset:±мс], метаданные [ar:] и т.п.
// пропускаются, метки слов <мм:сс.xx> убираются. Без единой метки времени - простой текст.
Lyrics ParseLrc(std::string_view text);
// Строка, которая звучит в момент seconds; -1 - до первой строки или текст не синхронный
int LyricLineAt(const Lyrics& lyrics, double seconds);
// Обратно в LRC (для кэша); простой текст - строками без меток
std::string ToLrc(const Lyrics& lyrics);

// ----- LRCLIB -----
std::string UrlEncode(std::string_view s);
struct LyricsRequest {
    uint64_t serial = 0;  // номер смены трека (по нему окно отбрасывает устаревшие ответы)
    std::filesystem::path track;
    std::string title, artist, album;
    double duration = 0;
};
// Название «Исполнитель - Название» без исполнителя в тегах (так называет файлы загрузчик)
void SplitArtistTitle(LyricsRequest& r);
std::string LrclibGetQuery(const LyricsRequest& r);     // путь и параметры для /api/get
std::string LrclibSearchQuery(const LyricsRequest& r);  // путь и параметры для /api/search

enum class LyricsStatus { Found, NotFound, Instrumental, NetworkError };

// Ответ /api/get (объект) или /api/search (массив: берётся лучший по длительности, синхронный - лучше)
LyricsStatus ParseLrclibResponse(std::string_view json, double duration, Lyrics& out);

struct LyricsResult {
    uint64_t serial = 0;
    LyricsStatus status = LyricsStatus::NotFound;
    Lyrics lyrics;
};

// HTTP GET к lrclib.net: путь с параметрами -> код ответа (0 - нет сети) и тело
using HttpGet = std::function<int(const std::string& pathAndQuery, std::string& body)>;

// Имя файла кэша для трека: хеш исполнителя и названия без учёта регистра
std::string LyricsCacheName(const LyricsRequest& r);
// Весь поиск: рядом с треком, кэш, сеть (с записью в кэш). now - секунды Unix.
LyricsResult FindLyrics(LyricsRequest r, const std::filesystem::path& cacheDir, const HttpGet& get, int64_t now);

// Фоновый поиск: обрабатывается только последний запрос (быстрое листание треков не копит очередь).
// ready вызывается из фонового потока, когда есть результат (окно отвечает сообщением и зовёт Take).
class LyricsService {
public:
    ~LyricsService() { Stop(); }
    void Start(std::filesystem::path cacheDir, HttpGet get, std::function<void()> ready);
    void Request(LyricsRequest r);
    bool Take(LyricsResult& out);
    // Свободный поток ждём; занятый сетью - отпускаем (он доработает сам и выйдет)
    void Stop();

private:
    struct State {
        std::mutex mu;
        std::condition_variable cv;
        bool stop = false, busy = false, hasRequest = false, hasResult = false;
        LyricsRequest request;
        LyricsResult result;
        std::filesystem::path cacheDir;
        HttpGet get;
        std::function<void()> ready;
    };
    static void Run(std::shared_ptr<State> s);
    std::shared_ptr<State> state_;
    std::thread thread_;
};

}  // namespace pm
