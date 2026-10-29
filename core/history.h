#pragma once
// История прослушиваний, «Итоги» и «Волна» - всё локально, без сети.
//
// История - файл history.jsonl рядом с state.json: одна строка JSON на прослушивание, файл
// только дописывается. Запись появляется, когда трек сменился или плеер закрылся и было
// реально прослушано не меньше 30 секунд (перемотка не считается). «Полным» прослушиванием
// (для счёта и любимого) считается половина трека, но не больше 4 минут.
//
// «Итоги» - топ исполнителей и треков, часы прослушивания, любимое время суток за месяц, год
// или всё время. «Волна» - бесконечная очередь: любимое и давно не слушанное, любимые
// исполнители, музыка этого времени суток; недавно игравшее отодвигается.

#include <array>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace pm {

struct PlayRecord {
    int64_t time = 0;       // начало прослушивания, секунды Unix
    std::string path;       // UTF-8
    std::string title, artist, album;
    double listened = 0;    // реально прослушано, секунды
    double duration = 0;    // длина трека, 0 - неизвестна
    bool Full() const;      // дослушан до половины (или 4 минут)
};

// Учёт прослушанного: Advance зовётся на каждом кадре и тике (и на паузе - не засчитывается)
class ListenTracker {
public:
    void Begin(PlayRecord meta);
    void Advance(double position, bool playing);
    // Закончить трек: true и запись в out, если её стоит сохранить
    bool End(PlayRecord& out);
    bool Active() const { return active_; }

private:
    PlayRecord cur_;
    double lastPos_ = -1;
    bool active_ = false;
};

bool AppendHistory(const std::filesystem::path& file, const PlayRecord& r);
// Битые строки пропускаются (например, оборванная запись при выключении питания)
std::vector<PlayRecord> LoadHistory(const std::filesystem::path& file);

// ----- Итоги -----
enum class StatsPeriod { Month, Year, All };
// Начало текущего месяца или года по местному времени (utcOffset - смещение местного времени, с)
int64_t PeriodStart(int64_t now, int utcOffset, StatsPeriod period);

struct StatItem {
    std::string name, sub;  // исполнитель; или название трека и исполнитель
    int plays = 0;          // полных прослушиваний
    double seconds = 0;
};

struct ListenSummary {
    double seconds = 0;
    int plays = 0;          // полных прослушиваний
    int tracks = 0, artists = 0, activeDays = 0;
    std::array<double, 24> byHour{};  // секунды по часам местного времени
    int topHour = -1;
    std::vector<StatItem> topArtists, topTracks;
};

ListenSummary Summarize(const std::vector<PlayRecord>& history, int64_t from, int64_t to, int utcOffset, size_t top);

// ----- Волна -----
struct WaveCandidate {
    std::string path, artist;
    uint32_t plays = 0;      // счётчик библиотеки
    int64_t lastPlayed = 0;  // секунды Unix, 0 - не играл
};

// Номера кандидатов в порядке игры (не больше count). exclude - пути, которые уже в очереди.
std::vector<size_t> BuildWave(const std::vector<WaveCandidate>& tracks, const std::vector<PlayRecord>& history,
                              int64_t now, int utcOffset, size_t count, uint64_t seed,
                              const std::set<std::string>& exclude = {});

}  // namespace pm
