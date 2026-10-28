#include "core/engine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <vector>

#include "core/spsc.h"
#include "miniaudio.h"

namespace pm {

namespace {

constexpr size_t kTapSize = 8192;          // кольцо моно-сэмплов для визуализатора
constexpr double kFadeSeconds = 0.010;     // затухание при паузе и нарастание при пуске
constexpr double kVolumeSeconds = 0.005;   // сглаживание громкости
constexpr uint32_t kScratchFrames = 4096;  // кусок чтения из декодера

struct Command {
    enum class Type { SetTrack, Clear, Seek, Play, Pause, SetNext, ClearNext } type = Type::Play;
    IDecoder* decoder = nullptr;  // для SetTrack и SetNext: владение переходит получателю
    uint64_t frame = 0;
    float gain = 1.f;
};

// Голос: декодер и его позиция. Звучит голос 0; голос 1 - следующий трек для бесшовного перехода.
struct Voice {
    IDecoder* decoder = nullptr;
    uint64_t position = 0;
    bool active = false;
    float gain = 1.f;  // ReplayGain
};

}  // namespace

struct Engine::Impl {
    ma_context context{};
    bool contextReady = false;
    ma_device device{};
    bool deviceReady = false;
    bool deviceRunning = false;  // меняет только поток UI
    uint32_t sampleRate = 48000;
    WakeFn wake;

    // Очереди между потоками
    SpscQueue<Command, 64> commands;    // UI -> аудио
    SpscQueue<IDecoder*, 256> retired;  // аудио -> UI: декодеры на удаление

    // Публикуемое состояние (пишет аудиопоток или UI при остановленном устройстве)
    std::atomic<bool> playing{false};  // намерение пользователя, пишет UI
    std::atomic<float> volumeTarget{0.7f};
    std::atomic<uint64_t> position{0};
    std::atomic<uint64_t> length{0};
    std::atomic<bool> hasTrack{false};
    std::atomic<bool> ended{false};
    std::atomic<bool> silent{false};  // затухание паузы закончилось - устройство можно остановить
    std::atomic<bool> switched{false};  // был бесшовный переход на следующий трек

    // Кольцо сэмплов для визуализатора: атомарные числа, чтобы чтение из UI не было гонкой данных
    std::array<std::atomic<float>, kTapSize> tap{};
    std::atomic<uint64_t> tapWritten{0};

    // Состояние, которым владеет тот, кто сейчас рендерит (аудиопоток или UI при остановке)
    std::array<Voice, 2> voices{};
    bool wantPlay = false;
    float fade = 0;
    float volume = 0.7f;
    std::vector<float> scratch;

    static void Callback(ma_device* dev, void* out, const void*, ma_uint32 frames) {
        static_cast<Impl*>(dev->pUserData)->Render(static_cast<float*>(out), frames);
    }

    // onAudioThread: где выполняется команда - от этого зависит, кто освобождает старый декодер
    void Apply(const Command& c, bool onAudioThread) {
        Voice& v = voices[0];
        Voice& next = voices[1];
        switch (c.type) {
            case Command::Type::SetTrack:
            case Command::Type::Clear:
                Retire(v.decoder, onAudioThread);
                Retire(next.decoder, onAudioThread);  // очередь относилась к прежнему треку
                next = Voice{};
                v.decoder = c.type == Command::Type::SetTrack ? c.decoder : nullptr;
                v.position = c.frame;
                v.gain = c.gain;
                v.active = v.decoder != nullptr;
                if (v.decoder && c.frame > 0 && !v.decoder->Seek(c.frame)) v.position = 0;
                position.store(v.position);
                length.store(v.decoder ? v.decoder->LengthFrames() : 0);
                hasTrack.store(v.decoder != nullptr);
                ended.store(false);
                break;
            case Command::Type::Seek:
                if (v.decoder && v.decoder->Seek(c.frame)) {
                    v.position = c.frame;
                    v.active = true;
                    position.store(v.position);
                    ended.store(false);
                }
                break;
            case Command::Type::Play: wantPlay = true; break;
            case Command::Type::Pause: wantPlay = false; break;
            case Command::Type::SetNext:
            case Command::Type::ClearNext:
                Retire(next.decoder, onAudioThread);
                next = Voice{};
                if (c.type == Command::Type::SetNext && c.decoder) {
                    next.decoder = c.decoder;
                    next.gain = c.gain;
                    next.active = true;
                }
                break;
        }
    }

    // Текущий трек кончился: если следующий открыт заранее - он становится текущим без паузы
    bool SwitchToNext(bool onAudioThread) {
        Voice& next = voices[1];
        if (!next.decoder) return false;
        Retire(voices[0].decoder, onAudioThread);
        voices[0] = next;
        voices[0].position = 0;
        next = Voice{};
        length.store(voices[0].decoder->LengthFrames());
        switched.store(true);
        return true;
    }

    void Retire(IDecoder* d, bool onAudioThread) {
        if (!d) return;
        // В аудиопотоке не освобождаем память: отдаём потоку UI.
        // Если очередь вдруг полна, декодер остаётся висеть до выхода - лучше утечка, чем щелчок.
        if (onAudioThread) retired.Push(d);
        else delete d;
    }

    void Render(float* out, uint32_t frames) {
        Command c;
        while (commands.Pop(c)) Apply(c, true);

        Voice& v = voices[0];
        const float fadeStep = static_cast<float>(1.0 / (kFadeSeconds * sampleRate));
        const float volStep = static_cast<float>(1.0 / (kVolumeSeconds * sampleRate));
        const float volTarget = volumeTarget.load(std::memory_order_relaxed);

        // Пауза, затухание закончилось: тишина без чтения декодера; потоку UI - сигнал остановить устройство
        if (!wantPlay && fade <= 0) {
            std::fill(out, out + static_cast<size_t>(frames) * kChannels, 0.f);
            if (!silent.exchange(true) && wake) wake();
            return;
        }
        silent.store(false);

        uint64_t tapPos = tapWritten.load(std::memory_order_relaxed);
        uint32_t done = 0;
        while (done < frames) {
            const uint32_t n = std::min(frames - done, kScratchFrames);
            float* buf = scratch.data();
            uint64_t got = 0;
            // gainAt: до какого кадра действует множитель прежнего трека (при переходе он меняется)
            float gainBefore = v.gain;
            uint64_t gainAt = 0;
            while (v.active && v.decoder && got < n) {
                const uint64_t r = v.decoder->Read(buf + got * kChannels, n - got);
                v.position += r;
                got += r;
                if (got < n) {
                    // Трек доиграл: бесшовно на следующий или стоп
                    gainBefore = v.gain;
                    gainAt = got;
                    if (SwitchToNext(true)) {
                        if (wake) wake();
                        continue;
                    }
                    v.active = false;
                    ended.store(true);
                    if (wake) wake();
                }
            }
            std::fill(buf + got * kChannels, buf + static_cast<size_t>(n) * kChannels, 0.f);
            // Здесь будет обработка голоса (time-stretch) и кроссфейд
            float* dst = out + static_cast<size_t>(done) * kChannels;
            for (uint32_t i = 0; i < n; ++i) {
                fade = wantPlay ? std::min(1.f, fade + fadeStep) : std::max(0.f, fade - fadeStep);
                volume += std::clamp(volTarget - volume, -volStep, volStep);
                const float g = (i < gainAt ? gainBefore : v.gain) * fade;
                const float l = buf[i * 2] * g;
                const float r = buf[i * 2 + 1] * g;
                dst[i * 2] = l * volume;
                dst[i * 2 + 1] = r * volume;
                // Визуализатор видит сигнал до громкости: полосы не зависят от ползунка
                tap[tapPos % kTapSize].store((l + r) * 0.5f, std::memory_order_relaxed);
                ++tapPos;
            }
            done += n;
        }
        tapWritten.store(tapPos, std::memory_order_release);
        position.store(v.position, std::memory_order_relaxed);
    }

    void Submit(const Command& c) {
        if (deviceRunning) {
            // Очередь на 64 команды. Переполниться она может только при залпе команд быстрее одного
            // периода устройства (~10 мс); тогда ждём, пока аудиопоток её разберёт.
            while (!commands.Push(c)) Drain();
        } else {
            Apply(c, false);  // колбэк не вызывается - голосами владеет поток UI
        }
    }

    void Drain() {
        IDecoder* d = nullptr;
        while (retired.Pop(d)) delete d;
    }
};

Engine::Engine() : impl_(std::make_unique<Impl>()) {}

Engine::~Engine() { Shutdown(); }

bool Engine::Start(const Options& options, WakeFn wake, std::string* error) {
    Impl& m = *impl_;
    // Повторный запуск (например, после неудачи - уже с null-бэкендом): сначала освобождаем прежнее
    if (m.contextReady || m.deviceReady) Shutdown();
    m.wake = std::move(wake);
    m.scratch.assign(static_cast<size_t>(kScratchFrames) * kChannels, 0.f);

    ma_context_config ctxCfg = ma_context_config_init();
    ma_backend nullBackend[] = {ma_backend_null};
    const ma_result rc = options.nullBackend ? ma_context_init(nullBackend, 1, &ctxCfg, &m.context)
                                             : ma_context_init(nullptr, 0, &ctxCfg, &m.context);
    if (rc != MA_SUCCESS) {
        if (error) *error = std::string("аудиоконтекст: ") + ma_result_description(rc);
        return false;
    }
    m.contextReady = true;

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = kChannels;
    cfg.sampleRate = options.sampleRate;
    cfg.dataCallback = &Impl::Callback;
    cfg.pUserData = &m;
    cfg.noPreSilencedOutputBuffer = MA_TRUE;  // буфер всегда заполняем сами
    const ma_result rd = ma_device_init(&m.context, &cfg, &m.device);
    if (rd != MA_SUCCESS) {
        if (error) *error = std::string("аудиоустройство: ") + ma_result_description(rd);
        return false;
    }
    m.deviceReady = true;
    m.sampleRate = m.device.sampleRate;
    return true;
}

void Engine::Shutdown() {
    Impl& m = *impl_;
    if (m.deviceReady) {
        ma_device_uninit(&m.device);  // останавливает колбэк и ждёт его завершения
        m.deviceReady = false;
        m.deviceRunning = false;
    }
    if (m.contextReady) {
        ma_context_uninit(&m.context);
        m.contextReady = false;
    }
    Command c;
    while (m.commands.Pop(c))
        if (c.decoder) delete c.decoder;
    m.Drain();
    for (Voice& v : m.voices) {
        delete v.decoder;
        v = Voice{};
    }
}

uint32_t Engine::SampleRate() const { return impl_->sampleRate; }

void Engine::SetTrack(std::unique_ptr<IDecoder> decoder, uint64_t startFrame, float gain) {
    Command c;
    c.type = Command::Type::SetTrack;
    c.decoder = decoder.release();
    c.frame = startFrame;
    c.gain = gain;
    impl_->Submit(c);
}

void Engine::QueueNext(std::unique_ptr<IDecoder> decoder, float gain) {
    Command c;
    c.type = Command::Type::SetNext;
    c.decoder = decoder.release();
    c.gain = gain;
    impl_->Submit(c);
}

void Engine::ClearNext() {
    Command c;
    c.type = Command::Type::ClearNext;
    impl_->Submit(c);
}

bool Engine::TakeSwitched() { return impl_->switched.exchange(false); }

void Engine::ClearTrack() {
    Command c;
    c.type = Command::Type::Clear;
    impl_->Submit(c);
}

void Engine::Play() {
    Impl& m = *impl_;
    m.playing.store(true);
    Command c;
    c.type = Command::Type::Play;
    m.Submit(c);
    if (m.deviceReady && !m.deviceRunning) {
        m.silent.store(false);
        if (ma_device_start(&m.device) == MA_SUCCESS) m.deviceRunning = true;
    }
}

void Engine::Pause() {
    Impl& m = *impl_;
    m.playing.store(false);
    Command c;
    c.type = Command::Type::Pause;
    m.Submit(c);
    // Само устройство остановит Service() после затухания
}

void Engine::Seek(uint64_t frame) {
    Command c;
    c.type = Command::Type::Seek;
    c.frame = frame;
    impl_->Submit(c);
}

void Engine::SetVolume(float volume) { impl_->volumeTarget.store(std::clamp(volume, 0.f, 1.f)); }

bool Engine::IsPlaying() const { return impl_->playing.load(); }
bool Engine::DeviceRunning() const { return impl_->deviceRunning; }
bool Engine::HasTrack() const { return impl_->hasTrack.load(); }
uint64_t Engine::PositionFrames() const { return impl_->position.load(); }
uint64_t Engine::LengthFrames() const { return impl_->length.load(); }

bool Engine::Service() {
    Impl& m = *impl_;
    m.Drain();
    if (m.deviceRunning && !m.playing.load() && m.silent.load()) {
        // ma_device_stop ждёт завершения колбэка: после него голосами снова владеет поток UI
        ma_device_stop(&m.device);
        m.deviceRunning = false;
        m.Drain();
        // Команды, которые аудиопоток не успел забрать, применяем сами
        Command c;
        while (m.commands.Pop(c)) m.Apply(c, false);
    }
    return m.ended.exchange(false);
}

size_t Engine::CopyRecentMono(float* dst, size_t n) const {
    const Impl& m = *impl_;
    n = std::min(n, kTapSize / 2);
    const uint64_t end = m.tapWritten.load(std::memory_order_acquire);
    const uint64_t start = end >= n ? end - n : 0;
    size_t i = 0;
    for (uint64_t p = start; p < end; ++p) dst[i++] = m.tap[p % kTapSize].load(std::memory_order_relaxed);
    std::fill(dst + i, dst + n, 0.f);
    return n;
}

}  // namespace pm
