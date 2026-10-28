// Движок и плеер на устройстве-заглушке miniaudio: декодирование WAV, длительность, перемотка,
// конец трека и переход к следующему, остановка устройства на паузе.
#include <atomic>
#include <chrono>
#include <thread>

#include "core/decoder.h"
#include "core/player.h"
#include "tests/check.h"
#include "tests/wav_util.h"

namespace {

// Ждёт условия, вызывая PumpEvents, не дольше timeout секунд
template <typename F>
bool WaitFor(pm::Player& p, F cond, double timeout) {
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < timeout) {
        p.PumpEvents();
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

}  // namespace

TEST(DecoderReadsWav) {
    const auto dir = tests::TempDir("decoder");
    tests::WriteBytes(dir / "a.wav", tests::MakeWav(0.5, 44100));
    std::string err;
    auto d = pm::OpenDecoder(dir / "a.wav", 2, 48000, &err);
    CHECK(d != nullptr);
    if (!d) return;
    // Длина пересчитана в частоту движка
    CHECK(d->LengthFrames() >= 23990 && d->LengthFrames() <= 24010);
    std::vector<float> buf(2 * 1000);
    CHECK(d->Read(buf.data(), 1000) == 1000);
    CHECK(d->Seek(20000));
    uint64_t total = 0, got = 0;
    while ((got = d->Read(buf.data(), 1000)) > 0) total += got;
    CHECK(total >= 3990 && total <= 4010);
    CHECK(pm::OpenDecoder(dir / "нет.wav", 2, 48000) == nullptr);
}

TEST(PlayerPlaysToEndAndAdvances) {
    const auto dir = tests::TempDir("player");
    tests::WriteBytes(dir / "1.wav", tests::MakeWav(0.25));
    tests::WriteBytes(dir / "2.wav", tests::MakeWav(0.25));
    tests::WriteBytes(dir / "10.wav", tests::MakeWav(0.25));
    tests::WriteBytes(dir / "notes.txt", {'x'});

    std::atomic<int> wakes{0};
    pm::Player p;
    std::string err;
    CHECK(p.Init({true}, [&] { ++wakes; }, &err));
    std::vector<std::pair<int, int>> changes;
    p.SetOnTrackChanged([&](int index, int direction) { changes.push_back({index, direction}); });

    CHECK(p.LoadFolder(dir) == 3);
    // Сортировка «как человек»: 1, 2, 10
    CHECK(p.GetPlaylist().At(2).path.filename() == "10.wav");
    CHECK_NEAR(p.GetPlaylist().At(0).meta.duration, 0.25, 0.01);

    p.Play();
    CHECK(p.GetState().playing && p.GetState().hasTrack && p.GetState().index == 0);
    CHECK_NEAR(p.GetState().duration, 0.25, 0.01);
    // Первый трек доигрывает, плеер сам переходит ко второму
    CHECK(WaitFor(p, [&] { return p.GetState().index == 1; }, 3.0));
    CHECK(wakes.load() > 0);
    CHECK(changes.size() >= 2 && changes[1] == std::make_pair(1, 1));
    // Пауза: после затухания устройство останавливается - в покое аудиопоток не работает
    p.Pause();
    CHECK(WaitFor(p, [&] { return !p.AudioDeviceRunning(); }, 1.0));
    // Перемотка на остановленном устройстве применяется сразу
    p.Seek(0.2);
    CHECK(p.GetState().position >= 0.19);
    // Позиция на паузе не растёт
    const double pos = p.GetState().position;
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    p.PumpEvents();
    CHECK_NEAR(p.GetState().position, pos, 1e-9);
    // Назад: позиция меньше 3 с - предыдущий трек
    p.Prev();
    CHECK(p.GetState().index == 0);
    // Конец списка без повтора: встаём на первый трек и паузу
    p.PlayIndex(2);
    CHECK(WaitFor(p, [&] { return !p.GetState().playing; }, 3.0));
    CHECK(p.GetState().index == 0);
    // Повтор одного: трек начинается заново, индекс не меняется
    p.SetRepeat(pm::Repeat::One);
    p.PlayIndex(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    p.PumpEvents();
    CHECK(p.GetState().index == 1 && p.GetState().playing);
    p.Shutdown();
}

TEST(PlayerSkipsBrokenFiles) {
    const auto dir = tests::TempDir("player_broken");
    tests::WriteBytes(dir / "1.wav", {'R', 'I', 'F', 'F', 0, 0});
    tests::WriteBytes(dir / "2.wav", tests::MakeWav(0.2));
    pm::Player p;
    CHECK(p.Init({true}, [] {}));
    CHECK(p.LoadFolder(dir) == 2);
    p.Play();
    CHECK(p.GetState().index == 1);
    CHECK(p.GetPlaylist().At(0).meta.broken);
    p.Shutdown();
}

TEST(EngineGaplessSwitch) {
    const auto dir = tests::TempDir("gapless");
    tests::WriteBytes(dir / "a.wav", tests::MakeWav(0.2, 48000));
    tests::WriteBytes(dir / "b.wav", tests::MakeWav(0.4, 48000));
    pm::Engine e;
    std::atomic<int> wakes{0};
    CHECK(e.Start({true, 48000}, [&] { ++wakes; }));
    e.SetTrack(pm::OpenDecoder(dir / "a.wav", 2, 48000), 0);
    e.QueueNext(pm::OpenDecoder(dir / "b.wav", 2, 48000), 0.5f);
    e.Play();
    // Первый трек кончается - движок сам переходит на второй, без события «трек доиграл»
    bool switched = false, ended = false;
    const auto start = std::chrono::steady_clock::now();
    while (!switched && std::chrono::steady_clock::now() - start < std::chrono::seconds(3)) {
        ended = e.Service() || ended;
        switched = e.TakeSwitched();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(switched && !ended);
    CHECK(e.LengthFrames() >= 19190 && e.LengthFrames() <= 19210);
    CHECK(e.PositionFrames() < 19200);
    // Новый трек вручную отменяет очередь: после него - обычный конец
    e.SetTrack(pm::OpenDecoder(dir / "a.wav", 2, 48000), 0);
    e.QueueNext(pm::OpenDecoder(dir / "b.wav", 2, 48000));
    e.ClearNext();
    ended = false;
    const auto start2 = std::chrono::steady_clock::now();
    while (!ended && std::chrono::steady_clock::now() - start2 < std::chrono::seconds(3)) {
        ended = e.Service();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(ended && !e.TakeSwitched());
    e.Shutdown();
}

TEST(PlayerGaplessAndQueueEdits) {
    const auto dir = tests::TempDir("player_gapless");
    for (const char* n : {"1.wav", "2.wav", "3.wav", "4.wav"}) tests::WriteBytes(dir / n, tests::MakeWav(0.15));
    pm::Player p;
    CHECK(p.Init({true}, [] {}));
    std::vector<int> changes;
    p.SetOnTrackChanged([&](int index, int) { changes.push_back(index); });
    CHECK(p.LoadFolder(dir) == 4);
    // «Играть следующим»: 4-й трек после 1-го
    p.OpenAt(0, 0, false);
    p.PlayNext(3);
    CHECK(p.GetPlaylist().At(1).path.filename() == "4.wav");
    p.Play();
    CHECK(WaitFor(p, [&] { return p.GetState().index == 1; }, 3.0));
    CHECK(p.Current().path.filename() == "4.wav");
    // Удаление играющего трека: начинается следующий
    p.Remove(1);
    CHECK(p.GetPlaylist().Size() == 3 && p.Current().path.filename() == "2.wav" && p.GetState().playing);
    // Удаление не играющего: индекс текущего сдвигается
    p.Remove(0);
    CHECK(p.GetState().index == 0 && p.Current().path.filename() == "2.wav");
    p.Remove(0);
    p.Remove(0);
    CHECK(p.GetPlaylist().Empty() && changes.back() == -1);
    CHECK(WaitFor(p, [&] { return !p.GetState().hasTrack; }, 1.0));  // команда аудиопотоку - не мгновенно
    p.Shutdown();
}
