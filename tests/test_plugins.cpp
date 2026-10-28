// Плагины: поиск по plugin.json, включение и выключение, события, пункты меню, ошибки.
// Используется настоящий пример plugins/history, собранный рядом с тестами.
#include <algorithm>
#include <fstream>
#include <string>

#include "core/plugins.h"
#include "tests/check.h"
#include "tests/wav_util.h"

namespace {

std::string ReadAll(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

}  // namespace

TEST(PluginsDiscoverEnableNotify) {
    const auto data = tests::TempDir("plugins_data");
    const auto extra = tests::TempDir("plugins_extra");
    // Битый манифест и плагин без библиотеки показываются с причиной, но не загружаются
    std::filesystem::create_directories(extra / "broken");
    tests::WriteBytes(extra / "broken" / "plugin.json", {'{', 'x'});
    std::filesystem::create_directories(extra / "nolib");
    const std::string manifest = R"({"id":"nolib","name":"Без DLL","library":"nolib"})";
    tests::WriteBytes(extra / "nolib" / "plugin.json", {manifest.begin(), manifest.end()});

    pm::PluginManager pm;
    pm.SetDataRoot(data);
    int commands = 0;
    pm.SetHost({[] { return PmPlayback{1, 0, 1, 0, 0, 1}; }, [&](int) { ++commands; }, [](double) {}});
    pm.Discover({PM_PLUGINS_DIR, extra});
    CHECK(pm.Plugins().size() == 3);
    size_t history = 99, nolib = 99, broken = 99;
    for (size_t i = 0; i < pm.Plugins().size(); ++i) {
        const auto& p = pm.Plugins()[i];
        if (p.id == "history") history = i;
        if (p.id == "nolib") nolib = i;
        if (p.id == "broken") broken = i;
    }
    CHECK(history < 3 && nolib < 3 && broken < 3);
    if (history >= 3) return;
    CHECK(pm.Plugins()[history].name == "Журнал прослушиваний");
    // По умолчанию всё выключено
    CHECK(pm.EnabledIds().empty() && !pm.Plugins()[history].loaded);
    CHECK(!pm.Plugins()[broken].error.empty());
    CHECK(!pm.SetEnabled(nolib, true) && !pm.Plugins()[nolib].error.empty() && !pm.Plugins()[nolib].enabled);

    CHECK(pm.SetEnabled(history, true));
    CHECK(pm.Plugins()[history].loaded && pm.EnabledIds() == std::set<std::string>{"history"});
    // Плагин добавил свой пункт меню
    CHECK(pm.MenuItems().size() == 1 && pm.MenuItems()[0].title == "Открыть журнал прослушиваний");
    pm.InvokeMenuItem(0);  // на Linux ничего не открывает, но не должен падать

    PmTrack t{"/music/a.mp3", "Лунный свет", "Северный Ветер", "", 200};
    pm.NotifyTrack(&t);
    pm.NotifyTrack(nullptr);  // пустой плейлист - запись не появляется
    PmTrack u{"/music/b.mp3", "Без исполнителя", "", "", 0};
    pm.NotifyTrack(&u);
    pm.NotifyPlayState(true);
    const std::string log = ReadAll(data / "history" / "history.txt");
    CHECK(log.find("| Северный Ветер - Лунный свет\n") != std::string::npos);
    CHECK(log.find("| Без исполнителя\n") != std::string::npos);
    CHECK(std::count(log.begin(), log.end(), '\n') == 2);

    // Выключение выгружает плагин и убирает его пункты меню; события больше не доходят
    CHECK(pm.SetEnabled(history, false));
    CHECK(!pm.Plugins()[history].loaded && pm.MenuItems().empty());
    pm.NotifyTrack(&t);
    CHECK(ReadAll(data / "history" / "history.txt") == log);

    // Повторный поиск сохраняет выбор пользователя
    pm.SetEnabled(history, true);
    pm.Discover({PM_PLUGINS_DIR, extra});
    CHECK(pm.EnabledIds() == std::set<std::string>{"history"});
    CHECK(ReadAll(data / "plugins.log").find("history: загружен") != std::string::npos);
    pm.UnloadAll();
    CHECK(pm.MenuItems().empty());
    (void)commands;
}
