#pragma once
// Модель темы PlayerMoon (формат playermoon-theme v1, см. mockup/SETTINGS.md).
//
// Итоговая тема = material встроенной темы settings.skin + её settings
// + переопределения пользователя. Встроенные темы, схема и раскладки
// читаются из папки assets рядом с exe (можно править без пересборки),
// а если файлов нет - из копии, зашитой в exe при сборке.
//
// Владение: ThemeLibrary создаётся и живёт в потоке UI; ядро тему не видит.

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.h"
#include "motion/easing.h"
#include "motion/tween.h"

namespace theme {

using motion::Rgba;

// Цвет материала: обычный или ссылка на цвет, который меняется (акцент из обложки и т.п.)
struct ThemeColor {
    enum class Ref { Plain, Accent, Accent2, Text, Glow, WindowSolid };
    Ref ref = Ref::Plain;
    Rgba plain;
    float alpha = 1;  // множитель прозрачности для ссылок
};

struct ShadowLayer {
    float x = 0, y = 0, blur = 0, spread = 0;
    ThemeColor color;
    bool inset = false;
};
using Shadow = std::vector<ShadowLayer>;

// Все 39 ключей settings. Значения по умолчанию - как у «Стекла».
struct Settings {
    std::string skin = "glass";
    std::string layout = "classic";
    std::string playlist = "side";
    std::string coverShape = "rounded";
    double spin = 0;
    Rgba accent{1, 1, 1, 1};
    Rgba accent2{1, 1, 1, 1};
    bool accentFromCover = false;
    Rgba text{1, 1, 1, 1};
    double textScale = 1;
    double titleWeight = 600;
    double mqSpeed = 30;
    std::string backdrop = "acrylic";
    Rgba winColor{0.078f, 0.078f, 0.118f, 1};
    double winDensity = 0.55;
    double tint = 0.42;
    double blur = 40;
    double noise = 0.05;
    double grid = 0;
    double radius = 14;
    std::string shadow = "deep";
    double ambient = 0.6;
    double glow = 0;
    std::string desk = "aurora";
    Rgba deskColor{0.169f, 0.176f, 0.259f, 1};
    std::string motion = "soft";
    double speed = 1;
    double stiffness = 420;
    double damping = 26;
    std::string coverFx = "zoom";
    std::string textFx = "rise";
    std::string btnFx = "spring";
    std::string playFx = "morph";
    std::string vizStyle = "mirror";
    std::string vizColor = "accent";
    double vizBars = 48;
    double vizAttack = 18;
    double vizFall = 1.5;
    double vizGlow = 0;
};

// Значения material, разобранные для отрисовки (px в DIP, длительности в секундах)
struct Material {
    struct {
        ThemeColor winBorder, text2, text3, btn, btnHover, btnPress, play, onPlay, playRing, track, thumb, panel,
            panelBorder, rowHover, rowActive, rowLine, scrim, closeHover, titleGlow;
    } color;
    struct {
        float saturate = 1, coverAmbientBlur = 0, coverSheen = 0, thumbIdle = 1, thumbHover = 1.4f, dot = 1;
    } effect;
    struct {
        float btn = 0, play = 0, row = 0, track = 0, panel = 0, thumb = 0, dot = 0;
    } radius;
    struct {
        Shadow cover, play, playHover, btnActive, thumb;
    } shadow;
    struct {
        float section = 0, controls = 0, row = 0;
    } spacing;
    struct {
        float lsTitle = 0, fsArtist = 14, fwArtist = 500, lsArtist = 0, fsAlbum = 12, fsTime = 12, fsRow = 14,
              fsRowSub = 12, fsCaps = 11, fsMiniTitle = 14, fsHeading = 15, lsHeading = 0;
        bool artistUpper = false, headingUpper = false;
        std::wstring family;         // необязательный ключ font.family (расширение схемы); пусто - системный
        std::wstring familyDisplay;  // font.familyDisplay
    } font;
    struct {
        float titlebar = 40, icon = 22, iconStroke = 2, track = 4, trackHover = 6, thumbW = 12, thumbH = 12, row = 54,
              miniCover = 60, dotW = 4, dotH = 4;
    } size;
    struct {
        double fast = 0.15, base = 0.22, slow = 0.35, cover = 0.35, text = 0.26, bg = 0.35, resize = 0.32,
               press = 0.09, stagger = 0.022;
    } duration;
    motion::CubicBezier easeOut{0.22, 1, 0.36, 1};
    motion::CubicBezier easeIn{0.4, 0, 1, 1};
    struct {
        double pause = 1.6;
        float gap = 48;
    } marquee;
};

// Геометрия раскладки из assets/layouts.json
struct LayoutGeometry {
    float winW = 420, winH = 680, playlistW = 360, cover = 292, art = 292, vizH = 40, play = 64, btn = 42;
    float padding = 26, fsTitle = 21;
};

struct Theme {
    std::string id;    // id встроенной темы-основы
    std::string name;  // имя для меню и экспорта
    bool builtin = true;
    Settings settings;
    Material material;
    json::Value materialJson;  // проверенный исходник material (для экспорта и отладки)
};

// Характер движения (mockup/ANIMATIONS.md): пружина, ease-out и множитель длительностей
struct MotionCharacter {
    std::string id;
    double stiffness, damping, speed;
    motion::CubicBezier easeOut;
};
const std::vector<MotionCharacter>& MotionCharacters();

// Разбор цвета "#RRGGBB" или "#RRGGBBAA"
bool ParseHexColor(std::string_view s, Rgba& out);
std::string HexColor(const Rgba& c, bool withAlpha);

// Значения, на которые ссылаются цвета material
struct ColorContext {
    Rgba accent, accent2, text, windowSolid;
    float glow = 0;  // settings.glow
};
Rgba Resolve(const ThemeColor& c, const ColorContext& ctx);

class ThemeLibrary {
public:
    // Читает схему, раскладки и встроенные темы из assetsDir; недостающие файлы - из копии в exe.
    // log получает строки о том, что взято откуда и какие файлы не прошли проверку.
    bool Load(const std::filesystem::path& assetsDir, std::vector<std::string>* log = nullptr);

    const json::Value& Schema() const { return schema_; }
    const std::vector<std::string>& BuiltinIds() const { return order_; }
    const Theme* Builtin(std::string_view id) const;
    LayoutGeometry Layout(std::string_view id) const;

    // Документ темы -> итоговая тема. Встроенная проверяется строго по схеме; пользовательская -
    // как в макете: неизвестные ключи игнорируются, неверные варианты отбрасываются, числа обрезаются.
    bool FromDocument(const json::Value& doc, Theme& out, std::string* error = nullptr) const;
    // Пользовательский документ (как экспорт в макете): все ключи settings, входящие в тему
    json::Value ToUserDocument(const Theme& t, const std::string& name) const;

    // Тема по имени встроенной; при неизвестном имени - первая («Стекло»)
    Theme Make(std::string_view id) const;

private:
    bool LoadBuiltin(const json::Value& doc, std::string* error);
    json::Value SanitizeSettings(const json::Value& in) const;

    json::Value schema_;
    json::Value layouts_;
    std::map<std::string, Theme, std::less<>> builtins_;
    std::vector<std::string> order_;
};

// Перевод settings JSON <-> структура (ключи без проверки; проверку делает ThemeLibrary)
void SettingsFromJson(const json::Value& v, Settings& s);
json::Value SettingsToJson(const Settings& s);
// material JSON -> Material. false, если чего-то не хватает (тогда остаются значения по умолчанию)
bool MaterialFromJson(const json::Value& v, Material& m);

}  // namespace theme
