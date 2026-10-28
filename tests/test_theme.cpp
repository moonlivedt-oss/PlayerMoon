// Темы: все 5 встроенных проходят схему, слияние пользовательской темы, очистка как в макете, палитра обложки.
#include <cmath>

#include "core/json.h"
#include "tests/check.h"
#include "theme/assets.h"
#include "theme/palette.h"
#include "theme/schema.h"
#include "theme/theme.h"

namespace {

std::filesystem::path AssetsDir() { return std::filesystem::path(tests::SourceDir()) / "assets"; }

json::Value LoadJson(const std::string& rel) {
    std::string text;
    json::Value v;
    theme::ReadAsset(AssetsDir(), rel, text);
    json::Parse(text, v);
    return v;
}

bool SameColor(const motion::Rgba& c, int r, int g, int b) {
    auto q = [](float x) { return static_cast<int>(std::lround(x * 255)); };
    return std::abs(q(c.r) - r) <= 1 && std::abs(q(c.g) - g) <= 1 && std::abs(q(c.b) - b) <= 1;
}

}  // namespace

TEST(BuiltinThemesPassSchema) {
    const json::Value schema = LoadJson("assets/theme-schema.json");
    CHECK(schema.IsObject());
    for (const char* id : {"glass", "frost", "neon", "sunset", "minimal"}) {
        const json::Value doc = LoadJson(std::string("assets/themes/") + id + ".json");
        std::vector<std::string> errs;
        const bool ok = theme::ValidateSchema(schema, doc, &errs);
        CHECK(ok);
        if (!ok) std::printf("    %s: %s\n", id, errs.empty() ? "" : errs.front().c_str());
    }
}

TEST(SchemaRejectsBadThemes) {
    const json::Value schema = LoadJson("assets/theme-schema.json");
    json::Value doc;
    // Число вне диапазона, неверный вариант, лишний ключ - всё должно быть отклонено
    json::Parse(R"({"format":"playermoon-theme","version":1,"name":"x","settings":{"skin":"neon","blur":500}})", doc);
    CHECK(!theme::ValidateSchema(schema, doc));
    json::Parse(R"({"format":"playermoon-theme","version":1,"name":"x","settings":{"skin":"neon","layout":"spiral"}})",
                doc);
    CHECK(!theme::ValidateSchema(schema, doc));
    json::Parse(R"({"format":"playermoon-theme","version":1,"name":"x","settings":{"skin":"neon"},"extra":1})", doc);
    CHECK(!theme::ValidateSchema(schema, doc));
    // А правильная пользовательская тема (как экспорт макета) проходит
    json::Parse(R"({"format":"playermoon-theme","version":1,"name":"x","settings":{"skin":"neon","accent":"#a6ff3d"}})",
                doc);
    CHECK(theme::ValidateSchema(schema, doc));
}

TEST(LibraryLoadsAndResolves) {
    theme::ThemeLibrary lib;
    std::vector<std::string> log;
    CHECK(lib.Load(AssetsDir(), &log));
    CHECK(lib.BuiltinIds().size() == 5);
    const theme::Theme* neon = lib.Builtin("neon");
    CHECK(neon != nullptr);
    if (!neon) return;
    CHECK(neon->settings.layout == "stage");
    CHECK(neon->settings.stiffness == 300 && neon->settings.damping == 14);
    CHECK(std::abs(neon->material.duration.base - 0.24) < 1e-9);
    // Производный цвет: play = акцент с прозрачностью 10%
    theme::ColorContext ctx;
    ctx.accent = neon->settings.accent;
    const motion::Rgba play = theme::Resolve(neon->material.color.play, ctx);
    CHECK(std::abs(play.a - 0.1f) < 1e-4);
    CHECK(SameColor(play, 0x2e, 0xf2, 0xff));
    // Тени со ссылкой на glow берут прозрачность из settings.glow
    ctx.glow = 0.6f;
    CHECK(!neon->material.shadow.play.empty());
    const motion::Rgba glow = theme::Resolve(neon->material.shadow.play[0].color, ctx);
    CHECK(std::abs(glow.a - 0.6f) < 1e-4);
    // Геометрия раскладок
    CHECK(lib.Layout("horizon").winW == 700 && lib.Layout("horizon").winH == 340);
    CHECK(lib.Layout("classic").cover == 292);
}

TEST(UserThemeMergesAndSanitizes) {
    theme::ThemeLibrary lib;
    CHECK(lib.Load(AssetsDir()));
    json::Value doc;
    // Как импорт в макете: blur обрезается до 80, неизвестный ключ и неверный вариант отбрасываются
    json::Parse(R"({"format":"playermoon-theme","version":1,"name":"Проверка",
        "settings":{"skin":"neon","layout":"horizon","accent":"#FF8800","blur":9999,"evil":"<x>","vizStyle":"nope"}})",
                doc);
    theme::Theme t;
    std::string err;
    CHECK(lib.FromDocument(doc, t, &err));
    CHECK(!t.builtin);
    CHECK(t.name == "Проверка");
    CHECK(t.settings.skin == "neon");
    CHECK(t.settings.layout == "horizon");
    CHECK(t.settings.blur == 80);
    CHECK(t.settings.vizStyle == "bars");  // значение неона, неверное отброшено
    CHECK(SameColor(t.settings.accent, 255, 136, 0));
    CHECK(t.settings.stiffness == 300);     // остальное - от неона
    // Чужой файл отклоняется
    json::Parse(R"({"format":"other","version":1,"settings":{}})", doc);
    CHECK(!lib.FromDocument(doc, t, &err));
}

TEST(ThemeRoundTrip) {
    theme::ThemeLibrary lib;
    CHECK(lib.Load(AssetsDir()));
    theme::Theme t = lib.Make("sunset");
    t.settings.accent = {0.2f, 0.4f, 0.6f, 1};
    t.settings.textScale = 1.1;
    const json::Value doc = lib.ToUserDocument(t, "Моя");
    // Экспорт проходит схему как пользовательская тема
    CHECK(theme::ValidateSchema(lib.Schema(), doc));
    theme::Theme back;
    CHECK(lib.FromDocument(doc, back));
    CHECK(theme::SettingsToJson(back.settings) == theme::SettingsToJson(t.settings));
    CHECK(back.settings.skin == "sunset");
}

TEST(PaletteFromCoverPixels) {
    // Картинка 2x2: три тёмно-синих пикселя и один ярко-оранжевый
    const uint8_t px[] = {120, 40, 20, 255, 120, 40, 20, 255, 120, 40, 20, 255, 20, 140, 250, 255};
    const auto p = theme::PaletteFromPixels(px, 4, "x");
    // Самый насыщенный - оранжевый: оттенок около 30 градусов
    CHECK(p.h3 >= 25 && p.h3 <= 35);
    CHECK(p.bg3.r > p.bg3.b);
    // Средний цвет - синеватый: bg1 с перевесом синего
    CHECK(p.bg1.b > p.bg1.r);
    // Без пикселей - палитра из названия, как у заглушки
    const auto q = theme::PaletteFromPixels(nullptr, 0, "Neon Rain");
    CHECK(q.h3 == theme::MakeCoverPalette("Neon Rain").h3);
}

TEST(CoverPaletteMatchesMockup) {
    // Эталоны посчитаны в браузере из palette() макета (цвета - getComputedStyle)
    CHECK(theme::Fnv1a("Neon Rain") == 4184706477u);
    CHECK(theme::Fnv1a("Лунный свет над Амуром") == 341209029u);
    auto p = theme::MakeCoverPalette("Лунный свет над Амуром");
    CHECK(SameColor(p.a, 233, 89, 32));
    CHECK(SameColor(p.b, 36, 144, 20));
    CHECK(SameColor(p.c, 78, 197, 244));
    CHECK(SameColor(p.bg1, 169, 73, 35));
    CHECK(SameColor(p.bg3, 46, 181, 234));
    CHECK(p.h3 == 197 && p.angle == 128 && p.x1 == 66 && p.y1 == 42 && p.x2 == 48 && p.y2 == 93);
    CHECK(p.disc == 26 && p.shape == 0);
    p = theme::MakeCoverPalette("Neon Rain");
    CHECK(SameColor(p.a, 226, 44, 96));
    CHECK(SameColor(p.c, 97, 239, 225));
    CHECK(p.h3 == 174 && p.angle == 173 && p.shape == 1 && p.disc == 24);
    p = theme::MakeCoverPalette("Последний трамвай до рассвета над спящим заливом");
    CHECK(SameColor(p.a, 225, 35, 152));
    CHECK(SameColor(p.bg1, 164, 40, 117));
    CHECK(p.h3 == 182 && p.angle == 41 && p.x2 == 2 && p.disc == 16 && p.shape == 1);
}
