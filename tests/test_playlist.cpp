// Плейлист: порядок, края, повтор, перемешивание с историей, удаление; библиотека: папки, сканирование
#include <algorithm>
#include <atomic>
#include <chrono>
#include <set>
#include <thread>

#include "core/library.h"
#include "core/player.h"
#include "core/playlist.h"
#include "tests/check.h"
#include "tests/wav_util.h"

namespace {

pm::Playlist Make(int n) {
    pm::Playlist p;
    for (int i = 0; i < n; ++i) p.Add("t" + std::to_string(i) + ".mp3", {});
    return p;
}

}  // namespace

TEST(PlaylistOrderAndRepeat) {
    pm::Playlist p = Make(3);
    CHECK(p.Current() == 0);
    CHECK(p.Advance(false) == 1);
    CHECK(p.Advance(false) == 2);
    // Конец списка: кнопка «следующий» идёт по кругу, а сам по себе трек без повтора - стоп
    CHECK(p.Advance(true) == -1);
    CHECK(p.Advance(false) == 0);
    p.Select(2);
    p.SetRepeat(pm::Repeat::All);
    CHECK(p.Advance(true) == 0);
    p.SetRepeat(pm::Repeat::One);
    CHECK(p.Advance(true) == 0);   // повтор одного - тот же трек
    CHECK(p.Advance(false) == 1);  // а кнопка всё равно листает
    // «Назад» без перемешивания идёт по кругу
    p.Select(0);
    CHECK(p.Back() == 2);
}

TEST(PlaylistShuffleIsPermutationWithHistory) {
    pm::Playlist p = Make(10);
    p.Seed(42);
    p.SetShuffle(true);
    std::vector<int> seen{p.Current()};
    for (int i = 0; i < 9; ++i) seen.push_back(p.Advance(false));
    // За один круг каждый трек ровно один раз
    std::set<int> unique(seen.begin(), seen.end());
    CHECK(unique.size() == 10);
    // «Назад» возвращает по истории в обратном порядке
    CHECK(p.Back() == seen[8]);
    CHECK(p.Back() == seen[7]);
    // Конец круга без повтора - стоп; с повтором - новый круг, не начинающийся с последнего трека
    p.Select(seen[9]);
    pm::Playlist q = Make(10);
    q.Seed(7);
    q.SetShuffle(true);
    int last = q.Current();
    for (int i = 0; i < 9; ++i) last = q.Advance(true);
    CHECK(q.Advance(true) == -1);
    q.SetRepeat(pm::Repeat::All);
    const int next = q.Advance(true);
    CHECK(next >= 0 && next != last);
}

TEST(PlaylistShuffleDeterministicWithSeed) {
    pm::Playlist a = Make(8), b = Make(8);
    a.Seed(123);
    b.Seed(123);
    a.SetShuffle(true);
    b.SetShuffle(true);
    CHECK(a.Order() == b.Order());
}

TEST(PlaylistSelectInShuffleKeepsHistory) {
    pm::Playlist p = Make(6);
    p.Seed(1);
    p.SetShuffle(true);
    const int first = p.Current();
    const int second = p.Advance(false);
    // Пользователь выбрал трек мышью: назад - снова second, затем first
    int pick = 0;
    while (pick == first || pick == second) ++pick;
    p.Select(pick);
    CHECK(p.Current() == pick);
    CHECK(p.Back() == second);
    CHECK(p.Back() == first);
    // В порядке по-прежнему все треки ровно по разу
    std::vector<int> order = p.Order();
    std::sort(order.begin(), order.end());
    CHECK((order == std::vector<int>{0, 1, 2, 3, 4, 5}));
}

TEST(PlaylistRemove) {
    pm::Playlist p = Make(4);
    p.Select(2);
    p.Remove(0);
    CHECK(p.Size() == 3 && p.Current() == 1);
    p.Remove(1);  // удалили текущий - текущим становится следующий
    CHECK(p.Size() == 2 && p.Current() == 1);
    p.Remove(1);
    CHECK(p.Current() == 0);
    p.Remove(0);
    CHECK(p.Empty() && p.Current() == -1);
}

TEST(NaturalSort) {
    CHECK(pm::NaturalLess("Трек 2.mp3", "Трек 10.mp3"));
    CHECK(!pm::NaturalLess("Трек 10.mp3", "Трек 2.mp3"));
    CHECK(pm::NaturalLess("a.mp3", "B.mp3"));
    CHECK(pm::NaturalLess("01 x", "2 x"));
}

// ---------------- библиотека ----------------

TEST(LibraryFoldersLearn) {
    const auto root = tests::TempDir("library_folders");
    std::filesystem::create_directories(root / "Музыка" / "Альбом");
    std::filesystem::create_directories(root / "Загрузки");
    pm::Library lib;
    CHECK(lib.AddFolder(root / "Музыка" / "Альбом"));
    // Охватывающая папка заменяет вложенную, вложенная в известную не добавляется
    CHECK(lib.AddFolder(root / "Музыка"));
    CHECK(lib.Folders().size() == 1);
    CHECK(!lib.AddFolder(root / "Музыка" / "Альбом"));
    // Файл, открытый пользователем, учит библиотеку своей папке
    tests::WriteBytes(root / "Загрузки" / "песня.wav", tests::MakeWav(0.1));
    CHECK(lib.LearnFrom(root / "Загрузки" / "песня.wav"));
    CHECK(lib.Folders().size() == 2);
    CHECK(lib.Contains(root / "Загрузки" / "песня.wav"));
    CHECK(!lib.Contains(root / "Другое" / "x.wav"));
    CHECK(lib.RemoveFolder(root / "Загрузки"));
    CHECK(lib.Folders().size() == 1 && !lib.RemoveFolder(root / "Загрузки"));
}

TEST(LibraryScanIncremental) {
    const auto root = tests::TempDir("library_scan");
    std::filesystem::create_directories(root / "B");
    std::filesystem::create_directories(root / "A");
    std::filesystem::create_directories(root / ".hidden");
    tests::WriteBytes(root / "B" / "10 x.wav", tests::MakeWav(0.1));
    tests::WriteBytes(root / "B" / "2 y.wav", tests::MakeWav(0.2));
    tests::WriteBytes(root / "A" / "z.wav", tests::MakeWav(0.3));
    tests::WriteBytes(root / ".hidden" / "skip.wav", tests::MakeWav(0.1));
    tests::WriteBytes(root / "A" / "cover.jpg", {1});
    pm::Library lib;
    lib.AddFolder(root);
    auto tracks = pm::Library::Scan(lib.Folders(), {});
    CHECK(tracks.size() == 3);
    // Порядок: папка A, затем B; внутри папки - «2» раньше «10»
    CHECK(tracks[0].path.filename() == "z.wav");
    CHECK(tracks[1].path.filename() == "2 y.wav" && tracks[2].path.filename() == "10 x.wav");
    CHECK_NEAR(tracks[0].meta.duration, 0.3, 0.01);
    lib.SetTracks(tracks);
    lib.NotePlayed(root / "A" / "z.wav", 1700000000);
    CHECK(lib.Tracks()[0].plays == 1);

    // Запись и чтение без потерь (счётчики и время изменения тоже)
    CHECK(lib.Save(root / "library.json"));
    pm::Library again;
    CHECK(again.Load(root / "library.json"));
    CHECK(again.Folders().size() == 1 && again.Tracks().size() == 3);
    CHECK(again.Tracks()[0].plays == 1 && again.Tracks()[0].mtime == tracks[0].mtime);

    // Повтор: неизменённые файлы берутся из кэша (подменённое название сохраняется), новый находится,
    // удалённый пропадает, счётчики переносятся
    auto cached = again.Tracks();
    cached[1].meta.title = "из кэша";
    std::filesystem::remove(root / "B" / "10 x.wav");
    tests::WriteBytes(root / "B" / "3 new.wav", tests::MakeWav(0.1));
    auto rescanned = pm::Library::Scan(again.Folders(), cached);
    CHECK(rescanned.size() == 3);
    CHECK(rescanned[1].meta.title == "из кэша");
    CHECK(rescanned[2].path.filename() == "3 new.wav");
    again.SetTracks(rescanned);
    CHECK(again.Tracks()[0].plays == 1);
    CHECK(again.Entries().size() == 3);
}

TEST(LibraryScannerInBackground) {
    const auto root = tests::TempDir("library_bg");
    for (int i = 0; i < 5; ++i) tests::WriteBytes(root / (std::to_string(i) + ".wav"), tests::MakeWav(0.05));
    pm::LibraryScanner scanner;
    std::atomic<bool> done{false};
    CHECK(scanner.Start({root}, {}, [&] { done = true; }));
    const auto start = std::chrono::steady_clock::now();
    while (!done && std::chrono::steady_clock::now() - start < std::chrono::seconds(5))
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    std::vector<pm::LibraryTrack> out;
    CHECK(done && scanner.Take(out) && out.size() == 5);
    CHECK(!scanner.Take(out));  // результат забирается один раз
}
