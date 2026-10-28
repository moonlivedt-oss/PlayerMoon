// JSON: разбор, запись, юникод, ошибки
#include "core/json.h"
#include "tests/check.h"

TEST(JsonParseAndDump) {
    json::Value v;
    std::string err;
    CHECK(json::Parse(R"({"a": 1, "b": [true, false, null], "c": {"d": "текст"}, "e": -1.5e2})", v, &err));
    CHECK(v.Get("a").AsNumber() == 1);
    CHECK(v.Get("b").Items().size() == 3);
    CHECK(v.Get("b").Items()[0].AsBool());
    CHECK(v.Get("b").Items()[2].IsNull());
    CHECK(v.Get("c").Get("d").AsString() == "текст");
    CHECK(v.Get("e").AsNumber() == -150);
    // Запись и повторное чтение дают то же значение
    json::Value back;
    CHECK(json::Parse(json::Dump(v), back));
    CHECK(back == v);
    CHECK(json::Dump(v, 0) == R"({"a":1,"b":[true,false,null],"c":{"d":"текст"},"e":-150})");
    // Порядок ключей сохраняется
    CHECK(v.Members()[0].first == "a" && v.Members()[3].first == "e");
}

TEST(JsonUnicodeEscapes) {
    json::Value v;
    CHECK(json::Parse(R"(["Луна", "🎵", "a\"b\\c\n"])", v));
    CHECK(v.Items()[0].AsString() == "Луна");
    CHECK(v.Items()[1].AsString() == "\xF0\x9F\x8E\xB5");  // U+1F3B5 из суррогатной пары
    CHECK(v.Items()[2].AsString() == "a\"b\\c\n");
    // Числа с дробью записываются кратчайшим точным видом
    json::Value n(0.1);
    CHECK(json::Dump(n, 0) == "0.1");
}

TEST(JsonErrors) {
    json::Value v;
    std::string err;
    CHECK(!json::Parse("{\"a\": }", v, &err));
    CHECK(err.find("строка 1") != std::string::npos);
    CHECK(!json::Parse("[1, 2", v, &err));
    CHECK(!json::Parse("{\"a\": 1} x", v, &err));
    CHECK(!json::Parse("\"незакрытая", v, &err));
    // Слишком глубокая вложенность не роняет программу
    CHECK(!json::Parse(std::string(500, '[') + std::string(500, ']'), v, &err));
}
