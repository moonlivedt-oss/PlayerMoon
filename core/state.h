#pragma once
// Состояние плеера между запусками (state.json).
// Где лежит файл, решает приложение (рядом с exe или в %APPDATA%), здесь - только формат.
// Тема хранится как документ playermoon-theme целиком (если своя) или id встроенной.

#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "core/json.h"

namespace pm {

struct AppState {
    std::vector<std::filesystem::path> playlist;
    int index = -1;
    double position = 0;  // секунды
    float volume = 0.7f;
    bool muted = false;
    std::string repeat = "off";  // off | all | one
    bool shuffle = false;

    std::string themeId = "glass";  // встроенная тема
    json::Value userTheme;          // своя тема (документ) или null

    // Настройки улучшений
    bool normalize = true;          // выравнивание громкости по ReplayGain
    bool showRemaining = false;     // справа под прогрессом - оставшееся время
    bool miniOnTop = true;          // мини-плеер поверх окон
    bool playlistFromLibrary = false;  // плейлист - это вся библиотека (новые треки добавляются сами)
    std::set<std::string> plugins;  // id включённых плагинов

    bool mini = false;
    bool playlistOpen = false;
    bool hasWindowPos = false;
    int windowX = 0, windowY = 0;  // левый верхний угол окна, физические пиксели
};

json::Value StateToJson(const AppState& s);
// Неизвестные и неверные поля пропускаются, остальное берётся по умолчанию
AppState StateFromJson(const json::Value& v);

bool SaveState(const std::filesystem::path& file, const AppState& s);
// false - файла нет или он битый; тогда s получает значения по умолчанию
bool LoadState(const std::filesystem::path& file, AppState& s);

}  // namespace pm
