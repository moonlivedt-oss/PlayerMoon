// Плейлист: порядок, края, повтор, перемешивание с историей, удаление
#include <algorithm>
#include <set>

#include "core/player.h"
#include "core/playlist.h"
#include "tests/check.h"

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
