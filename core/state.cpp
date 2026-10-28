#include "core/state.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <system_error>

#include "core/text.h"

namespace pm {

namespace {
constexpr int kStateVersion = 1;
}

json::Value StateToJson(const AppState& s) {
    json::Value v = json::Value::MakeObject();
    v["version"] = kStateVersion;
    json::Value list = json::Value::MakeArray();
    for (const auto& p : s.playlist) list.Push(text::PathToUtf8(p));
    v["playlist"] = std::move(list);
    v["index"] = s.index;
    v["position"] = std::round(s.position * 10) / 10;
    v["volume"] = static_cast<double>(s.volume);
    v["muted"] = s.muted;
    v["repeat"] = s.repeat;
    v["shuffle"] = s.shuffle;
    v["theme"] = s.themeId;
    if (!s.userTheme.IsNull()) v["userTheme"] = s.userTheme;
    v["normalize"] = s.normalize;
    v["showRemaining"] = s.showRemaining;
    v["miniOnTop"] = s.miniOnTop;
    v["playlistFromLibrary"] = s.playlistFromLibrary;
    json::Value plugins = json::Value::MakeArray();
    for (const auto& id : s.plugins) plugins.Push(id);
    v["plugins"] = std::move(plugins);
    v["mini"] = s.mini;
    v["playlistOpen"] = s.playlistOpen;
    if (s.hasWindowPos) {
        json::Value pos = json::Value::MakeObject();
        pos["x"] = s.windowX;
        pos["y"] = s.windowY;
        v["window"] = std::move(pos);
    }
    return v;
}

AppState StateFromJson(const json::Value& v) {
    AppState s;
    if (!v.IsObject()) return s;
    for (const auto& item : v.Get("playlist").Items())
        if (item.IsString() && !item.AsString().empty()) s.playlist.push_back(text::Utf8ToPath(item.AsString()));
    const double index = v.Get("index").AsNumber(-1);
    s.index = (index >= 0 && index < static_cast<double>(s.playlist.size())) ? static_cast<int>(index) : -1;
    if (s.index < 0 && !s.playlist.empty()) s.index = 0;
    s.position = std::max(0.0, v.Get("position").AsNumber(0));
    s.volume = static_cast<float>(std::clamp(v.Get("volume").AsNumber(0.7), 0.0, 1.0));
    s.muted = v.Get("muted").AsBool(false);
    const std::string& repeat = v.Get("repeat").AsString();
    s.repeat = (repeat == "all" || repeat == "one") ? repeat : "off";
    s.shuffle = v.Get("shuffle").AsBool(false);
    if (v.Get("theme").IsString() && !v.Get("theme").AsString().empty()) s.themeId = v.Get("theme").AsString();
    if (v.Get("userTheme").IsObject()) s.userTheme = v.Get("userTheme");
    s.normalize = v.Get("normalize").AsBool(true);
    s.showRemaining = v.Get("showRemaining").AsBool(false);
    s.miniOnTop = v.Get("miniOnTop").AsBool(true);
    s.playlistFromLibrary = v.Get("playlistFromLibrary").AsBool(false);
    for (const auto& id : v.Get("plugins").Items())
        if (id.IsString() && !id.AsString().empty()) s.plugins.insert(id.AsString());
    s.mini = v.Get("mini").AsBool(false);
    s.playlistOpen = v.Get("playlistOpen").AsBool(false);
    const json::Value& w = v.Get("window");
    if (w.Get("x").IsNumber() && w.Get("y").IsNumber()) {
        s.hasWindowPos = true;
        s.windowX = static_cast<int>(w.Get("x").AsNumber());
        s.windowY = static_cast<int>(w.Get("y").AsNumber());
    }
    return s;
}

bool SaveState(const std::filesystem::path& file, const AppState& s) {
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    // Пишем во временный файл и переименовываем: сбой посреди записи не портит прошлое состояние
    std::filesystem::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        const std::string data = json::Dump(StateToJson(s));
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!f) return false;
    }
    std::filesystem::rename(tmp, file, ec);
    return !ec;
}

bool LoadState(const std::filesystem::path& file, AppState& s) {
    s = AppState{};
    std::ifstream f(file, std::ios::binary);
    if (!f) return false;
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    json::Value v;
    if (!json::Parse(data, v) || !v.IsObject()) return false;
    s = StateFromJson(v);
    return true;
}

}  // namespace pm
