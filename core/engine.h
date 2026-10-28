#pragma once
// Аудиодвижок: устройство вывода miniaudio, голоса, громкость, плавные пуск и пауза.
//
// Кто чем владеет:
// - Поток UI вызывает все публичные методы, кроме CopyRecentMono (его тоже UI, но только чтение).
// - Аудиопоток (колбэк miniaudio) владеет голосами и декодерами, пока устройство запущено.
//   Команды к нему идут через SPSC-очередь (без замков), ответы - через атомарные поля
//   и очередь отработавших декодеров, которые освобождает поток UI в Service().
// - Пока устройство остановлено (пауза), колбэк не вызывается, и голосами владеет поток UI:
//   команды применяются сразу.
// - На паузе устройство останавливается после короткого затухания: в покое аудиопоток не крутится.
//
// Задел на будущее: голосов два (второй - для кроссфейда), а в цепочке голоса есть место
// для обработки (сохранение высоты тона при смене скорости).

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "core/decoder.h"

namespace pm {

class Engine {
public:
    static constexpr uint32_t kChannels = 2;

    struct Options {
        bool nullBackend = false;  // устройство-заглушка без звука (тесты, CI)
        uint32_t sampleRate = 0;   // 0 - родная частота устройства
    };
    // Вызывается из аудиопотока, когда потоку UI пора вызвать Service(). Должна быть потокобезопасной.
    using WakeFn = std::function<void()>;

    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    bool Start(const Options& options, WakeFn wake, std::string* error = nullptr);
    void Shutdown();
    uint32_t SampleRate() const;

    // ----- поток UI -----
    void SetTrack(std::unique_ptr<IDecoder> decoder, uint64_t startFrame);
    void ClearTrack();
    void Play();
    void Pause();
    void Seek(uint64_t frame);
    void SetVolume(float volume);  // 0..1, применяется плавно (без щелчков)

    bool IsPlaying() const;  // намерение: нажат ли «пуск»
    bool DeviceRunning() const;  // работает ли аудиопоток (на паузе после затухания - нет)
    bool HasTrack() const;
    uint64_t PositionFrames() const;
    uint64_t LengthFrames() const;

    // Обслуживание в потоке UI (по сигналу wake и при обновлении окна): освобождает отработавшие
    // декодеры, останавливает устройство после затухания паузы. true - текущий трек доиграл.
    bool Service();

    // Последние n моно-сэмплов (для визуализатора). Возвращает сколько скопировано.
    size_t CopyRecentMono(float* dst, size_t n) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pm
