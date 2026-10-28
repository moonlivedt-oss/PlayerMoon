#pragma once
// Узкий API плеера. Через него работает окно, через него же потом будет управлять RadialMenu
// (в отдельном процессе - через канал, см. PLAN.md). Заголовок совместим с C++17.
//
// Все методы вызываются из одного потока (потока UI). Звук играет отдельный аудиопоток
// внутри Engine; он будит поток UI функцией wake, после чего UI вызывает PumpEvents().

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/engine.h"
#include "core/playlist.h"
#include "core/tags.h"

namespace pm {

struct PlayerState {
    bool playing = false;
    bool hasTrack = false;
    int index = -1;
    int count = 0;
    double position = 0;  // секунды
    double duration = 0;  // секунды
    float volume = 0.7f;
    bool muted = false;
    Repeat repeat = Repeat::Off;
    bool shuffle = false;
};

// Текущий трек: теги с обложкой (байты картинки) и название для показа
struct NowPlaying {
    int index = -1;
    std::filesystem::path path;
    TagInfo tags;
    std::string title;  // из тегов или имя файла
    uint64_t serial = 0;  // растёт при каждой смене трека
};

class Player {
public:
    struct Options {
        bool nullBackend = false;
    };
    using WakeFn = std::function<void()>;
    // index - новый трек, direction: 1 вперёд, -1 назад, 0 - без направления (открытие, восстановление)
    using TrackChangedFn = std::function<void(int index, int direction)>;

    Player();
    ~Player();

    bool Init(const Options& options, WakeFn wake, std::string* error = nullptr);
    void Shutdown();

    // ----- плейлист -----
    // Папка сканируется вглубь, файлы сортируются по имени (с учётом чисел). Возвращает число добавленных.
    int LoadFolder(const std::filesystem::path& dir, bool append = false);
    int LoadFiles(const std::vector<std::filesystem::path>& files, bool append = false);
    const Playlist& GetPlaylist() const { return playlist_; }

    // ----- управление -----
    void Play();
    void Pause();
    void Toggle();
    void Next();
    void Prev();  // в начало трека, если прошло больше 3 секунд, иначе предыдущий
    void PlayIndex(int index);
    void Seek(double seconds);
    void SetVolume(float volume);
    void SetMuted(bool muted);
    void SetRepeat(Repeat r);
    void SetShuffle(bool on);
    // Восстановление после запуска: открыть трек и встать на позицию без звука
    bool OpenAt(int index, double position, bool play);

    PlayerState GetState() const;
    const NowPlaying& Current() const { return now_; }
    void SetOnTrackChanged(TrackChangedFn fn) { onTrackChanged_ = std::move(fn); }

    // В потоке UI после сигнала wake: конец трека, повтор, переход к следующему
    void PumpEvents();
    // Последние n моно-сэмплов для визуализатора
    size_t ReadPcm(float* dst, size_t n) const;
    uint32_t SampleRate() const;
    // Работает ли аудиопоток. На паузе устройство останавливается - ноль активных циклов в покое.
    bool AudioDeviceRunning() const { return engine_.DeviceRunning(); }

private:
    bool OpenTrack(int index, double position, int direction);
    void ApplyVolume();

    Engine engine_;
    Playlist playlist_;
    NowPlaying now_;
    TrackChangedFn onTrackChanged_;
    float volume_ = 0.7f;
    bool muted_ = false;
    bool ready_ = false;
};

// Сравнение имён «как человек»: «Трек 2» раньше «Трек 10»
bool NaturalLess(const std::string& a, const std::string& b);

}  // namespace pm
