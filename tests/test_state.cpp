// state.json: запись, чтение, битый файл
#include "core/state.h"
#include "core/text.h"
#include "tests/check.h"
#include "tests/wav_util.h"

TEST(StateRoundTrip) {
    const auto dir = tests::TempDir("state");
    pm::AppState s;
    s.playlist = {dir / "Трек 1.mp3", dir / "b.flac"};
    s.index = 1;
    s.position = 42.37;
    s.volume = 0.3f;
    s.muted = true;
    s.repeat = "one";
    s.shuffle = true;
    s.themeId = "neon";
    json::Parse(R"({"format":"playermoon-theme","version":1,"name":"Моя","settings":{"skin":"neon"}})", s.userTheme);
    s.mini = true;
    s.playlistOpen = true;
    s.normalize = false;
    s.showRemaining = true;
    s.miniOnTop = false;
    s.playlistFromLibrary = true;
    s.plugins = {"history", "lyrics"};
    s.hasWindowPos = true;
    s.windowX = -1200;
    s.windowY = 80;
    const auto file = dir / "state.json";
    CHECK(pm::SaveState(file, s));
    pm::AppState back;
    CHECK(pm::LoadState(file, back));
    CHECK(back.playlist == s.playlist);
    CHECK(back.index == 1);
    CHECK_NEAR(back.position, 42.4, 1e-9);  // хранится с точностью 0.1 с
    CHECK_NEAR(back.volume, 0.3, 1e-6);
    CHECK(back.muted && back.repeat == "one" && back.shuffle);
    CHECK(back.themeId == "neon" && back.userTheme == s.userTheme);
    CHECK(!back.normalize && back.showRemaining && !back.miniOnTop && back.playlistFromLibrary);
    CHECK(back.plugins == s.plugins);
    CHECK(back.mini && back.playlistOpen && back.hasWindowPos && back.windowX == -1200 && back.windowY == 80);
}

TEST(StateBrokenFileGivesDefaults) {
    const auto dir = tests::TempDir("state_bad");
    pm::AppState s;
    CHECK(!pm::LoadState(dir / "нет.json", s));
    CHECK(s.index == -1 && s.volume == 0.7f && s.themeId == "glass");
    CHECK(s.normalize && s.miniOnTop && !s.showRemaining && s.plugins.empty());
    tests::WriteBytes(dir / "bad.json", {'{', '"', 'a'});
    CHECK(!pm::LoadState(dir / "bad.json", s));
    // Неверные значения внутри правильного JSON заменяются умолчаниями
    json::Value v;
    json::Parse(R"({"playlist":["a.mp3"],"index":9,"volume":7,"repeat":"maybe","position":-5})", v);
    s = pm::StateFromJson(v);
    CHECK(s.index == 0 && s.volume == 1.0f && s.repeat == "off" && s.position == 0);
}
