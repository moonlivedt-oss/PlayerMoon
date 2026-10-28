#include "theme/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/text.h"
#include "theme/assets.h"
#include "theme/schema.h"

namespace theme {

namespace {

constexpr const char* kFormat = "playermoon-theme";
constexpr int kVersion = 1;
// Порядок встроенных тем - как в макете
constexpr const char* kBuiltinIds[] = {"glass", "frost", "neon", "sunset", "minimal"};

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Чтение значений из JSON с запасным значением
double Num(const json::Value& obj, std::string_view key, double def) { return obj.Get(key).AsNumber(def); }
const std::string& Str(const json::Value& obj, std::string_view key) { return obj.Get(key).AsString(); }

void ReadString(const json::Value& v, std::string_view key, std::string& out) {
    if (v.Get(key).IsString()) out = v.Get(key).AsString();
}
void ReadNumber(const json::Value& v, std::string_view key, double& out) {
    if (v.Get(key).IsNumber()) out = v.Get(key).AsNumber();
}
void ReadBool(const json::Value& v, std::string_view key, bool& out) {
    if (v.Get(key).IsBool()) out = v.Get(key).AsBool();
}
void ReadColor(const json::Value& v, std::string_view key, Rgba& out) {
    Rgba c;
    if (ParseHexColor(v.Get(key).AsString(), c)) out = c;
}

ThemeColor ColorFromJson(const json::Value& v) {
    ThemeColor c;
    if (v.IsString()) {
        ParseHexColor(v.AsString(), c.plain);
        return c;
    }
    const std::string& ref = Str(v, "ref");
    if (ref == "accent") c.ref = ThemeColor::Ref::Accent;
    else if (ref == "accent2") c.ref = ThemeColor::Ref::Accent2;
    else if (ref == "text") c.ref = ThemeColor::Ref::Text;
    else if (ref == "glow") c.ref = ThemeColor::Ref::Glow;
    else if (ref == "windowSolid") c.ref = ThemeColor::Ref::WindowSolid;
    c.alpha = static_cast<float>(Num(v, "alpha", 1));
    return c;
}

Shadow ShadowFromJson(const json::Value& v) {
    Shadow s;
    for (const auto& layer : v.Items()) {
        ShadowLayer l;
        l.x = static_cast<float>(Num(layer, "x", 0));
        l.y = static_cast<float>(Num(layer, "y", 0));
        l.blur = static_cast<float>(Num(layer, "blur", 0));
        l.spread = static_cast<float>(Num(layer, "spread", 0));
        l.color = ColorFromJson(layer.Get("color"));
        l.inset = layer.Get("inset").AsBool();
        // Полностью прозрачные слои (заглушки "0 0 0 transparent") не рисуем
        if (l.color.ref == ThemeColor::Ref::Plain && l.color.plain.a <= 0) continue;
        s.push_back(l);
    }
    return s;
}

motion::CubicBezier BezierFromJson(const json::Value& v, motion::CubicBezier def) {
    if (!v.IsArray() || v.Items().size() != 4) return def;
    return {v.Items()[0].AsNumber(), v.Items()[1].AsNumber(), v.Items()[2].AsNumber(), v.Items()[3].AsNumber()};
}

}  // namespace

const std::vector<MotionCharacter>& MotionCharacters() {
    // Значения - mockup/ANIMATIONS.md, таблица «Характеры движения»
    static const std::vector<MotionCharacter> kList = {
        {"soft", 420, 26, 1.0, {0.22, 1, 0.36, 1}},   {"bouncy", 300, 14, 1.0, {0.16, 1, 0.3, 1}},
        {"calm", 300, 30, 1.0, {0.25, 0.8, 0.25, 1}}, {"snappy", 700, 46, 0.7, {0.2, 0.9, 0.1, 1}},
        {"slow", 160, 22, 1.6, {0.33, 1, 0.68, 1}},   {"none", 700, 60, 0.0, {0, 0, 1, 1}},
    };
    return kList;
}

bool ParseHexColor(std::string_view s, Rgba& out) {
    if (s.size() != 7 && s.size() != 9) return false;
    if (s[0] != '#') return false;
    int v[4] = {0, 0, 0, 255};
    for (size_t i = 0; i < (s.size() - 1) / 2; ++i) {
        const int hi = HexDigit(s[1 + i * 2]);
        const int lo = HexDigit(s[2 + i * 2]);
        if (hi < 0 || lo < 0) return false;
        v[i] = hi * 16 + lo;
    }
    out = {v[0] / 255.f, v[1] / 255.f, v[2] / 255.f, v[3] / 255.f};
    return true;
}

std::string HexColor(const Rgba& c, bool withAlpha) {
    auto b = [](float x) { return static_cast<int>(std::lround(std::clamp(x, 0.f, 1.f) * 255)); };
    char buf[16];
    if (withAlpha) std::snprintf(buf, sizeof buf, "#%02X%02X%02X%02X", b(c.r), b(c.g), b(c.b), b(c.a));
    else std::snprintf(buf, sizeof buf, "#%02x%02x%02x", b(c.r), b(c.g), b(c.b));
    return buf;
}

Rgba Resolve(const ThemeColor& c, const ColorContext& ctx) {
    Rgba base;
    switch (c.ref) {
        case ThemeColor::Ref::Plain: return c.plain;
        case ThemeColor::Ref::Accent: base = ctx.accent; break;
        case ThemeColor::Ref::Accent2: base = ctx.accent2; break;
        case ThemeColor::Ref::Text: base = ctx.text; break;
        case ThemeColor::Ref::WindowSolid: base = ctx.windowSolid; break;
        case ThemeColor::Ref::Glow:
            // glow = акцент с прозрачностью settings.glow (как --c-glow в макете)
            base = ctx.accent;
            base.a *= ctx.glow;
            break;
    }
    base.a *= c.alpha;
    return base;
}

void SettingsFromJson(const json::Value& v, Settings& s) {
    ReadString(v, "skin", s.skin);
    ReadString(v, "layout", s.layout);
    ReadString(v, "playlist", s.playlist);
    ReadString(v, "coverShape", s.coverShape);
    ReadNumber(v, "spin", s.spin);
    ReadColor(v, "accent", s.accent);
    ReadColor(v, "accent2", s.accent2);
    ReadBool(v, "accentFromCover", s.accentFromCover);
    ReadColor(v, "text", s.text);
    ReadNumber(v, "textScale", s.textScale);
    ReadNumber(v, "titleWeight", s.titleWeight);
    ReadNumber(v, "mqSpeed", s.mqSpeed);
    ReadString(v, "backdrop", s.backdrop);
    ReadColor(v, "winColor", s.winColor);
    ReadNumber(v, "winDensity", s.winDensity);
    ReadNumber(v, "tint", s.tint);
    ReadNumber(v, "blur", s.blur);
    ReadNumber(v, "noise", s.noise);
    ReadNumber(v, "grid", s.grid);
    ReadNumber(v, "radius", s.radius);
    ReadString(v, "shadow", s.shadow);
    ReadNumber(v, "ambient", s.ambient);
    ReadNumber(v, "glow", s.glow);
    ReadString(v, "desk", s.desk);
    ReadColor(v, "deskColor", s.deskColor);
    ReadString(v, "motion", s.motion);
    ReadNumber(v, "speed", s.speed);
    ReadNumber(v, "stiffness", s.stiffness);
    ReadNumber(v, "damping", s.damping);
    ReadString(v, "coverFx", s.coverFx);
    ReadString(v, "textFx", s.textFx);
    ReadString(v, "btnFx", s.btnFx);
    ReadString(v, "playFx", s.playFx);
    ReadString(v, "vizStyle", s.vizStyle);
    ReadString(v, "vizColor", s.vizColor);
    ReadNumber(v, "vizBars", s.vizBars);
    ReadNumber(v, "vizAttack", s.vizAttack);
    ReadNumber(v, "vizFall", s.vizFall);
    ReadNumber(v, "vizGlow", s.vizGlow);
}

json::Value SettingsToJson(const Settings& s) {
    // Порядок ключей - как в схеме и файлах макета
    json::Value v = json::Value::MakeObject();
    v["skin"] = s.skin;
    v["layout"] = s.layout;
    v["playlist"] = s.playlist;
    v["coverShape"] = s.coverShape;
    v["spin"] = s.spin;
    v["accent"] = HexColor(s.accent, false);
    v["accent2"] = HexColor(s.accent2, false);
    v["accentFromCover"] = s.accentFromCover;
    v["text"] = HexColor(s.text, false);
    v["textScale"] = s.textScale;
    v["titleWeight"] = s.titleWeight;
    v["mqSpeed"] = s.mqSpeed;
    v["backdrop"] = s.backdrop;
    v["winColor"] = HexColor(s.winColor, false);
    v["winDensity"] = s.winDensity;
    v["tint"] = s.tint;
    v["blur"] = s.blur;
    v["noise"] = s.noise;
    v["grid"] = s.grid;
    v["radius"] = s.radius;
    v["shadow"] = s.shadow;
    v["ambient"] = s.ambient;
    v["glow"] = s.glow;
    v["desk"] = s.desk;
    v["deskColor"] = HexColor(s.deskColor, false);
    v["motion"] = s.motion;
    v["speed"] = s.speed;
    v["stiffness"] = s.stiffness;
    v["damping"] = s.damping;
    v["coverFx"] = s.coverFx;
    v["textFx"] = s.textFx;
    v["btnFx"] = s.btnFx;
    v["playFx"] = s.playFx;
    v["vizStyle"] = s.vizStyle;
    v["vizColor"] = s.vizColor;
    v["vizBars"] = s.vizBars;
    v["vizAttack"] = s.vizAttack;
    v["vizFall"] = s.vizFall;
    v["vizGlow"] = s.vizGlow;
    return v;
}

bool MaterialFromJson(const json::Value& v, Material& m) {
    if (!v.IsObject()) return false;
    const json::Value& col = v.Get("color");
    auto c = [&](std::string_view key, ThemeColor& out) {
        if (const json::Value* x = col.Find(key)) out = ColorFromJson(*x);
    };
    c("winBorder", m.color.winBorder);
    c("text2", m.color.text2);
    c("text3", m.color.text3);
    c("btn", m.color.btn);
    c("btnHover", m.color.btnHover);
    c("btnPress", m.color.btnPress);
    c("play", m.color.play);
    c("onPlay", m.color.onPlay);
    c("playRing", m.color.playRing);
    c("track", m.color.track);
    c("thumb", m.color.thumb);
    c("panel", m.color.panel);
    c("panelBorder", m.color.panelBorder);
    c("rowHover", m.color.rowHover);
    c("rowActive", m.color.rowActive);
    c("rowLine", m.color.rowLine);
    c("scrim", m.color.scrim);
    c("closeHover", m.color.closeHover);
    c("titleGlow", m.color.titleGlow);

    auto f = [](const json::Value& obj, std::string_view key, float& out) {
        if (obj.Get(key).IsNumber()) out = static_cast<float>(obj.Get(key).AsNumber());
    };
    const json::Value& fx = v.Get("effect");
    f(fx, "saturate", m.effect.saturate);
    f(fx, "coverAmbientBlur", m.effect.coverAmbientBlur);
    f(fx, "coverSheen", m.effect.coverSheen);
    f(fx, "thumbIdle", m.effect.thumbIdle);
    f(fx, "thumbHover", m.effect.thumbHover);
    f(fx, "dot", m.effect.dot);

    const json::Value& r = v.Get("radius");
    f(r, "btn", m.radius.btn);
    f(r, "play", m.radius.play);
    f(r, "row", m.radius.row);
    f(r, "track", m.radius.track);
    f(r, "panel", m.radius.panel);
    f(r, "thumb", m.radius.thumb);
    f(r, "dot", m.radius.dot);

    const json::Value& sh = v.Get("shadow");
    m.shadow.cover = ShadowFromJson(sh.Get("cover"));
    m.shadow.play = ShadowFromJson(sh.Get("play"));
    m.shadow.playHover = ShadowFromJson(sh.Get("playHover"));
    m.shadow.btnActive = ShadowFromJson(sh.Get("btnActive"));
    m.shadow.thumb = ShadowFromJson(sh.Get("thumb"));

    const json::Value& sp = v.Get("spacing");
    f(sp, "section", m.spacing.section);
    f(sp, "controls", m.spacing.controls);
    f(sp, "row", m.spacing.row);

    const json::Value& fo = v.Get("font");
    f(fo, "lsTitle", m.font.lsTitle);
    f(fo, "fsArtist", m.font.fsArtist);
    f(fo, "fwArtist", m.font.fwArtist);
    f(fo, "lsArtist", m.font.lsArtist);
    f(fo, "fsAlbum", m.font.fsAlbum);
    f(fo, "fsTime", m.font.fsTime);
    f(fo, "fsRow", m.font.fsRow);
    f(fo, "fsRowSub", m.font.fsRowSub);
    f(fo, "fsCaps", m.font.fsCaps);
    f(fo, "fsMiniTitle", m.font.fsMiniTitle);
    f(fo, "fsHeading", m.font.fsHeading);
    f(fo, "lsHeading", m.font.lsHeading);
    m.font.artistUpper = Str(fo, "ttArtist") == "uppercase";
    m.font.headingUpper = Str(fo, "ttHeading") == "uppercase";
    m.font.family = text::Utf8ToWide(Str(fo, "family"));
    m.font.familyDisplay = text::Utf8ToWide(Str(fo, "familyDisplay"));

    const json::Value& sz = v.Get("size");
    f(sz, "titlebar", m.size.titlebar);
    f(sz, "icon", m.size.icon);
    f(sz, "iconStroke", m.size.iconStroke);
    f(sz, "track", m.size.track);
    f(sz, "trackHover", m.size.trackHover);
    f(sz, "thumbW", m.size.thumbW);
    f(sz, "thumbH", m.size.thumbH);
    f(sz, "row", m.size.row);
    f(sz, "miniCover", m.size.miniCover);
    f(sz, "dotW", m.size.dotW);
    f(sz, "dotH", m.size.dotH);

    // Длительности в файле - миллисекунды, в коде - секунды
    const json::Value& d = v.Get("duration");
    auto ms = [&](std::string_view key, double& out) {
        if (d.Get(key).IsNumber()) out = d.Get(key).AsNumber() / 1000.0;
    };
    ms("fast", m.duration.fast);
    ms("base", m.duration.base);
    ms("slow", m.duration.slow);
    ms("cover", m.duration.cover);
    ms("text", m.duration.text);
    ms("bg", m.duration.bg);
    ms("resize", m.duration.resize);
    ms("press", m.duration.press);
    ms("stagger", m.duration.stagger);

    m.easeOut = BezierFromJson(v.Get("easing").Get("out"), m.easeOut);
    m.easeIn = BezierFromJson(v.Get("easing").Get("in"), m.easeIn);

    const json::Value& mq = v.Get("marquee");
    if (mq.Get("pause").IsNumber()) m.marquee.pause = mq.Get("pause").AsNumber() / 1000.0;
    f(mq, "gap", m.marquee.gap);
    return true;
}

bool ThemeLibrary::Load(const std::filesystem::path& assetsDir, std::vector<std::string>* log) {
    auto note = [&](const std::string& s) {
        if (log) log->push_back(s);
    };
    std::string textBuf, err;
    bool fromDisk = false;

    // Схема: без неё нельзя проверять темы
    if (!ReadAsset(assetsDir, "assets/theme-schema.json", textBuf, &fromDisk) || !json::Parse(textBuf, schema_, &err)) {
        note("theme-schema.json: " + (err.empty() ? std::string("не найдена") : err) + ", беру встроенную");
        ReadAsset({}, "assets/theme-schema.json", textBuf);
        json::Parse(textBuf, schema_);
    } else {
        note(std::string("theme-schema.json: ") + (fromDisk ? "с диска" : "встроенная"));
    }

    if (!ReadAsset(assetsDir, "assets/layouts.json", textBuf, &fromDisk) || !json::Parse(textBuf, layouts_, &err)) {
        note("layouts.json: ошибка, беру встроенный");
        ReadAsset({}, "assets/layouts.json", textBuf);
        json::Parse(textBuf, layouts_);
    }

    builtins_.clear();
    order_.clear();
    for (const char* id : kBuiltinIds) {
        const std::string rel = std::string("assets/themes/") + id + ".json";
        json::Value doc;
        bool ok = ReadAsset(assetsDir, rel, textBuf, &fromDisk) && json::Parse(textBuf, doc, &err) &&
                  LoadBuiltin(doc, &err);
        if (!ok && fromDisk) {
            // Файл на диске испорчен - откатываемся на встроенную копию, чтобы плеер не остался без темы
            note(rel + ": " + err + ", беру встроенную копию");
            ok = ReadAsset({}, rel, textBuf) && json::Parse(textBuf, doc, &err) && LoadBuiltin(doc, &err);
        }
        if (!ok) {
            note(rel + ": не загружена: " + err);
            continue;
        }
        order_.push_back(id);
    }
    return !order_.empty();
}

bool ThemeLibrary::LoadBuiltin(const json::Value& doc, std::string* error) {
    std::vector<std::string> errs;
    if (!ValidateSchema(schema_, doc, &errs) || Str(doc, "kind") != "builtin") {
        if (error) *error = errs.empty() ? "не встроенная тема" : errs.front();
        return false;
    }
    Theme t;
    t.id = Str(doc, "id");
    t.name = Str(doc, "name");
    t.builtin = true;
    SettingsFromJson(doc.Get("settings"), t.settings);
    t.materialJson = doc.Get("material");
    MaterialFromJson(t.materialJson, t.material);
    builtins_[t.id] = std::move(t);
    return true;
}

const Theme* ThemeLibrary::Builtin(std::string_view id) const {
    auto it = builtins_.find(id);
    return it == builtins_.end() ? nullptr : &it->second;
}

LayoutGeometry ThemeLibrary::Layout(std::string_view id) const {
    LayoutGeometry g;
    const json::Value& l = layouts_.Get("layouts").Get(id);
    if (!l.IsObject()) return g;
    const json::Value& sz = l.Get("size");
    auto f = [&](std::string_view key, float& out) {
        if (sz.Get(key).IsNumber()) out = static_cast<float>(sz.Get(key).AsNumber());
    };
    f("winW", g.winW);
    f("winH", g.winH);
    f("playlistW", g.playlistW);
    f("cover", g.cover);
    f("art", g.art);
    f("vizH", g.vizH);
    f("play", g.play);
    f("btn", g.btn);
    g.padding = static_cast<float>(Num(l.Get("spacing"), "win", g.padding));
    g.fsTitle = static_cast<float>(Num(l.Get("font"), "fsTitle", g.fsTitle));
    return g;
}

// Как sanitize() в макете: неизвестные ключи - прочь, неверные варианты и цвета - прочь,
// числа обрезаются по диапазону. Правила берутся из схемы ($defs.settings.properties).
json::Value ThemeLibrary::SanitizeSettings(const json::Value& in) const {
    json::Value out = json::Value::MakeObject();
    if (!in.IsObject()) return out;
    const json::Value& defs = schema_.Get("$defs");
    const json::Value& props = defs.Get("settings").Get("properties");
    for (const auto& [key, value] : in.Members()) {
        const json::Value* prop = props.Find(key);
        if (!prop) continue;
        // Отдельная схема для одного значения, с теми же $defs для ссылок
        json::Value one = *prop;
        one["$defs"] = defs;
        json::Value candidate = value;
        if (candidate.IsNumber()) {
            const json::Value* mn = prop->Find("minimum");
            const json::Value* mx = prop->Find("maximum");
            double x = candidate.AsNumber();
            if (!std::isfinite(x)) continue;
            if (mn) x = std::max(x, mn->AsNumber());
            if (mx) x = std::min(x, mx->AsNumber());
            candidate = json::Value(x);
        }
        if (candidate.IsString() && candidate.AsString().size() == 7 && candidate.AsString()[0] == '#')
            candidate = json::Value(text::AsciiLower(candidate.AsString()));
        if (ValidateSchema(one, candidate)) out[key] = candidate;
    }
    return out;
}

bool ThemeLibrary::FromDocument(const json::Value& doc, Theme& out, std::string* error) const {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (!doc.IsObject() || Str(doc, "format") != kFormat) return fail("это не файл темы PlayerMoon");
    if (Num(doc, "version", 0) != kVersion) return fail("неподдерживаемая версия формата");
    if (!doc.Get("settings").IsObject()) return fail("в файле нет settings");

    if (Str(doc, "kind") == "builtin") {
        // Полная тема (например, отредактированная копия встроенной): строгая проверка
        std::vector<std::string> errs;
        if (!ValidateSchema(schema_, doc, &errs)) return fail(errs.empty() ? "файл не прошёл схему" : errs.front());
        out = Theme{};
        out.id = Str(doc, "id");
        out.name = Str(doc, "name");
        out.builtin = false;
        SettingsFromJson(doc.Get("settings"), out.settings);
        out.materialJson = doc.Get("material");
        MaterialFromJson(out.materialJson, out.material);
        return true;
    }

    const json::Value clean = SanitizeSettings(doc.Get("settings"));
    const Theme* base = Builtin(clean.Get("skin").AsString());
    if (!base) base = Builtin(order_.empty() ? "glass" : order_.front());
    if (!base) return fail("нет встроенных тем");
    out = *base;
    out.builtin = false;
    std::string name = Str(doc, "name");
    const auto cps = text::CodePoints(name);
    if (cps.size() > 40) {
        name.clear();
        for (size_t i = 0; i < 40; ++i) text::AppendUtf8(name, cps[i]);
    }
    out.name = name.empty() ? base->name : name;
    SettingsFromJson(clean, out.settings);
    out.settings.skin = base->id;
    return true;
}

json::Value ThemeLibrary::ToUserDocument(const Theme& t, const std::string& name) const {
    json::Value doc = json::Value::MakeObject();
    doc["format"] = kFormat;
    doc["version"] = kVersion;
    doc["kind"] = "user";
    doc["name"] = name;
    doc["settings"] = SettingsToJson(t.settings);
    return doc;
}

Theme ThemeLibrary::Make(std::string_view id) const {
    const Theme* t = Builtin(id);
    if (!t && !order_.empty()) t = Builtin(order_.front());
    return t ? *t : Theme{};
}

}  // namespace theme
