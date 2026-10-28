#pragma once
// Плейлист: список треков, текущий трек, порядок перемешивания и повтор.
// Чистая логика без звука и окон. Владелец - поток UI (через pm::Player).
//
// Перемешивание хранит порядок обхода (order_). Он же служит историей:
// «назад» идёт по уже сыгранным позициям порядка. Когда порядок кончается
// при повторе «все», строится новый, где первым не стоит только что сыгранный трек.

#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace pm {

enum class Repeat { Off, All, One };

struct TrackMeta {
    std::string title;   // из тегов; пусто - показывать имя файла
    std::string artist;
    std::string album;
    double duration = 0;  // секунды; 0 - неизвестна до открытия декодера
    bool broken = false;  // файл не открылся - пропускаем при переходах
};

struct PlaylistEntry {
    std::filesystem::path path;
    TrackMeta meta;
};

class Playlist {
public:
    void Clear();
    void Add(std::filesystem::path path, TrackMeta meta);
    void Remove(int index);

    int Size() const { return static_cast<int>(entries_.size()); }
    bool Empty() const { return entries_.empty(); }
    const PlaylistEntry& At(int index) const { return entries_[static_cast<size_t>(index)]; }
    PlaylistEntry& At(int index) { return entries_[static_cast<size_t>(index)]; }

    int Current() const { return current_; }
    // Выбор трека пользователем (клик в списке). В перемешивании сохраняет историю.
    void Select(int index);

    void SetRepeat(Repeat r) { repeat_ = r; }
    Repeat GetRepeat() const { return repeat_; }
    void SetShuffle(bool on);
    bool Shuffle() const { return shuffle_; }
    void Seed(uint32_t seed) { rng_.seed(seed); }

    // Следующий трек. automatic = трек доиграл сам (тогда действуют правила повтора).
    // Возвращает новый текущий индекс или -1, если пора остановиться (конец списка без повтора).
    int Advance(bool automatic);
    // Предыдущий трек (без проверки «прошло больше 3 секунд» - это решает Player)
    int Back();

    // Порядок перемешивания (для тестов и отладки)
    const std::vector<int>& Order() const { return order_; }

private:
    void BuildOrder(int first, int avoidFirst);

    std::vector<PlaylistEntry> entries_;
    int current_ = -1;
    Repeat repeat_ = Repeat::Off;
    bool shuffle_ = false;
    std::vector<int> order_;  // порядок обхода при перемешивании
    int orderPos_ = 0;        // позиция текущего трека в order_
    std::mt19937 rng_{std::random_device{}()};
};

}  // namespace pm
