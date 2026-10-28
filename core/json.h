#pragma once
// Небольшой JSON: разбор и запись без исключений.
// Нужен для файлов темы, схемы и state.json. Объекты сохраняют порядок ключей,
// чтобы записанный файл читался человеком так же, как исходный.

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Member = std::pair<std::string, Value>;

    Value() = default;
    Value(bool b) : type_(Type::Bool), bool_(b) {}
    Value(double d) : type_(Type::Number), number_(d) {}
    Value(int i) : type_(Type::Number), number_(i) {}
    Value(const char* s) : type_(Type::String), string_(s) {}
    Value(std::string s) : type_(Type::String), string_(std::move(s)) {}
    static Value MakeArray() { Value v; v.type_ = Type::Array; return v; }
    static Value MakeObject() { Value v; v.type_ = Type::Object; return v; }

    Type GetType() const { return type_; }
    bool IsNull() const { return type_ == Type::Null; }
    bool IsBool() const { return type_ == Type::Bool; }
    bool IsNumber() const { return type_ == Type::Number; }
    bool IsString() const { return type_ == Type::String; }
    bool IsArray() const { return type_ == Type::Array; }
    bool IsObject() const { return type_ == Type::Object; }

    bool AsBool(bool def = false) const { return type_ == Type::Bool ? bool_ : def; }
    double AsNumber(double def = 0) const { return type_ == Type::Number ? number_ : def; }
    const std::string& AsString() const;  // пустая строка, если не строка

    // Массив
    const std::vector<Value>& Items() const { return items_; }
    std::vector<Value>& Items() { return items_; }
    void Push(Value v) { items_.push_back(std::move(v)); }
    size_t Size() const { return type_ == Type::Array ? items_.size() : members_.size(); }

    // Объект
    const std::vector<Member>& Members() const { return members_; }
    std::vector<Member>& Members() { return members_; }
    const Value* Find(std::string_view key) const;
    Value* Find(std::string_view key);
    // Ссылка на член объекта; создаёт его, если нет
    Value& operator[](std::string_view key);
    // Член или null-значение, если нет (удобно для цепочек)
    const Value& Get(std::string_view key) const;
    bool Erase(std::string_view key);

    bool operator==(const Value& other) const;
    bool operator!=(const Value& other) const { return !(*this == other); }

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0;
    std::string string_;
    std::vector<Value> items_;
    std::vector<Member> members_;
};

// Разбор текста. При ошибке возвращает false и пишет в error сообщение с номером строки и столбца.
bool Parse(std::string_view text, Value& out, std::string* error = nullptr);

// Запись. indent > 0 - с переносами строк и отступами, 0 - в одну строку.
std::string Dump(const Value& v, int indent = 2);

}  // namespace json
