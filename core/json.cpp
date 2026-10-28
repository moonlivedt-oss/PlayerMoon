#include "core/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace json {

namespace {

const std::string kEmptyString;
const Value kNullValue;

// Добавляет кодовую точку в строку UTF-8
void AppendUtf8(std::string& s, unsigned cp) {
    if (cp < 0x80) {
        s += static_cast<char>(cp);
    } else if (cp < 0x800) {
        s += static_cast<char>(0xC0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        s += static_cast<char>(0xF0 | (cp >> 18));
        s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    bool Run(Value& out, std::string* error) {
        SkipSpace();
        if (s_.substr(0, 3) == "\xEF\xBB\xBF") pos_ += 3;  // BOM
        SkipSpace();
        if (!ParseValue(out, 0)) return Fail(error);
        SkipSpace();
        if (pos_ != s_.size()) {
            err_ = "лишние символы после значения";
            return Fail(error);
        }
        return true;
    }

private:
    static constexpr int kMaxDepth = 64;  // защита от переполнения стека на злонамеренных файлах

    bool Fail(std::string* error) {
        if (error) {
            int line = 1, col = 1;
            for (size_t i = 0; i < pos_ && i < s_.size(); ++i) {
                if (s_[i] == '\n') { ++line; col = 1; } else { ++col; }
            }
            *error = err_ + " (строка " + std::to_string(line) + ", столбец " + std::to_string(col) + ")";
        }
        return false;
    }

    void SkipSpace() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r'))
            ++pos_;
    }

    bool Literal(std::string_view word) {
        if (s_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }

    bool ParseValue(Value& out, int depth) {
        if (depth > kMaxDepth) { err_ = "слишком глубокая вложенность"; return false; }
        if (pos_ >= s_.size()) { err_ = "неожиданный конец"; return false; }
        const char c = s_[pos_];
        if (c == '{') return ParseObject(out, depth);
        if (c == '[') return ParseArray(out, depth);
        if (c == '"') {
            std::string str;
            if (!ParseString(str)) return false;
            out = Value(std::move(str));
            return true;
        }
        if (Literal("true")) { out = Value(true); return true; }
        if (Literal("false")) { out = Value(false); return true; }
        if (Literal("null")) { out = Value(); return true; }
        if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber(out);
        err_ = "неожиданный символ";
        return false;
    }

    bool ParseNumber(Value& out) {
        const size_t start = pos_;
        if (s_[pos_] == '-') ++pos_;
        auto digits = [&] {
            const size_t d = pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
            return pos_ > d;
        };
        if (!digits()) { err_ = "ожидалась цифра"; return false; }
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            if (!digits()) { err_ = "ожидалась цифра после точки"; return false; }
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            if (!digits()) { err_ = "ожидалась цифра в степени"; return false; }
        }
        const std::string num(s_.substr(start, pos_ - start));
        out = Value(std::strtod(num.c_str(), nullptr));
        return true;
    }

    bool Hex4(unsigned& v) {
        if (pos_ + 4 > s_.size()) { err_ = "неполная \\u-последовательность"; return false; }
        v = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = s_[pos_++];
            v <<= 4;
            if (h >= '0' && h <= '9') v |= static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f') v |= static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') v |= static_cast<unsigned>(h - 'A' + 10);
            else { err_ = "неверная \\u-последовательность"; return false; }
        }
        return true;
    }

    bool ParseString(std::string& out) {
        ++pos_;  // открывающая кавычка
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) { err_ = "управляющий символ в строке"; return false; }
            if (c != '\\') { out += c; continue; }
            if (pos_ >= s_.size()) break;
            const char e = s_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!Hex4(cp)) return false;
                    // Суррогатная пара UTF-16 -> одна кодовая точка
                    if (cp >= 0xD800 && cp <= 0xDBFF && s_.substr(pos_, 2) == "\\u") {
                        pos_ += 2;
                        unsigned lo = 0;
                        if (!Hex4(lo)) return false;
                        if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: err_ = "неизвестная escape-последовательность"; return false;
            }
        }
        err_ = "незакрытая строка";
        return false;
    }

    bool ParseArray(Value& out, int depth) {
        ++pos_;
        out = Value::MakeArray();
        SkipSpace();
        if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
        while (true) {
            SkipSpace();
            Value item;
            if (!ParseValue(item, depth + 1)) return false;
            out.Push(std::move(item));
            SkipSpace();
            if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
            if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
            err_ = "ожидалась , или ]";
            return false;
        }
    }

    bool ParseObject(Value& out, int depth) {
        ++pos_;
        out = Value::MakeObject();
        SkipSpace();
        if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
        while (true) {
            SkipSpace();
            if (pos_ >= s_.size() || s_[pos_] != '"') { err_ = "ожидалось имя ключа"; return false; }
            std::string key;
            if (!ParseString(key)) return false;
            SkipSpace();
            if (pos_ >= s_.size() || s_[pos_] != ':') { err_ = "ожидалось :"; return false; }
            ++pos_;
            SkipSpace();
            Value item;
            if (!ParseValue(item, depth + 1)) return false;
            out[key] = std::move(item);  // повторный ключ заменяет прежний
            SkipSpace();
            if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
            if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
            err_ = "ожидалась , или }";
            return false;
        }
    }

    std::string_view s_;
    size_t pos_ = 0;
    std::string err_;
};

void DumpString(std::string& out, const std::string& s) {
    out += '"';
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += c;  // UTF-8 пишем как есть
                }
        }
    }
    out += '"';
}

void DumpNumber(std::string& out, double d) {
    if (!std::isfinite(d)) { out += "0"; return; }
    if (d == std::floor(d) && std::abs(d) < 1e15) {
        out += std::to_string(static_cast<long long>(d));
        return;
    }
    // Кратчайшая запись, которая читается обратно в то же число
    char buf[40];
    for (int precision = 6; precision <= 17; ++precision) {
        std::snprintf(buf, sizeof buf, "%.*g", precision, d);
        if (std::strtod(buf, nullptr) == d) break;
    }
    out += buf;
}

void DumpValue(std::string& out, const Value& v, int indent, int level) {
    const std::string pad = indent > 0 ? "\n" + std::string(static_cast<size_t>(indent * (level + 1)), ' ') : "";
    const std::string padEnd = indent > 0 ? "\n" + std::string(static_cast<size_t>(indent * level), ' ') : "";
    switch (v.GetType()) {
        case Value::Type::Null: out += "null"; break;
        case Value::Type::Bool: out += v.AsBool() ? "true" : "false"; break;
        case Value::Type::Number: DumpNumber(out, v.AsNumber()); break;
        case Value::Type::String: DumpString(out, v.AsString()); break;
        case Value::Type::Array: {
            if (v.Items().empty()) { out += "[]"; break; }
            // Массивы из чисел пишем в одну строку - так короче кривые и точки
            bool flat = true;
            for (const auto& it : v.Items()) flat = flat && it.IsNumber();
            out += '[';
            for (size_t i = 0; i < v.Items().size(); ++i) {
                if (i) out += flat ? ", " : ",";
                if (!flat) out += pad;
                DumpValue(out, v.Items()[i], indent, level + 1);
            }
            if (!flat) out += padEnd;
            out += ']';
            break;
        }
        case Value::Type::Object: {
            if (v.Members().empty()) { out += "{}"; break; }
            out += '{';
            for (size_t i = 0; i < v.Members().size(); ++i) {
                if (i) out += ',';
                out += pad;
                DumpString(out, v.Members()[i].first);
                out += indent > 0 ? ": " : ":";
                DumpValue(out, v.Members()[i].second, indent, level + 1);
            }
            out += padEnd;
            out += '}';
            break;
        }
    }
}

}  // namespace

const std::string& Value::AsString() const { return type_ == Type::String ? string_ : kEmptyString; }

const Value* Value::Find(std::string_view key) const {
    if (type_ != Type::Object) return nullptr;
    for (const auto& m : members_)
        if (m.first == key) return &m.second;
    return nullptr;
}

Value* Value::Find(std::string_view key) {
    if (type_ != Type::Object) return nullptr;
    for (auto& m : members_)
        if (m.first == key) return &m.second;
    return nullptr;
}

Value& Value::operator[](std::string_view key) {
    if (type_ != Type::Object) *this = MakeObject();
    if (Value* v = Find(key)) return *v;
    members_.emplace_back(std::string(key), Value());
    return members_.back().second;
}

const Value& Value::Get(std::string_view key) const {
    const Value* v = Find(key);
    return v ? *v : kNullValue;
}

bool Value::Erase(std::string_view key) {
    for (auto it = members_.begin(); it != members_.end(); ++it) {
        if (it->first == key) {
            members_.erase(it);
            return true;
        }
    }
    return false;
}

bool Value::operator==(const Value& o) const {
    if (type_ != o.type_) return false;
    switch (type_) {
        case Type::Null: return true;
        case Type::Bool: return bool_ == o.bool_;
        case Type::Number: return number_ == o.number_;
        case Type::String: return string_ == o.string_;
        case Type::Array: return items_ == o.items_;
        case Type::Object: {
            // Порядок ключей не важен для равенства
            if (members_.size() != o.members_.size()) return false;
            for (const auto& m : members_) {
                const Value* other = o.Find(m.first);
                if (!other || !(*other == m.second)) return false;
            }
            return true;
        }
    }
    return false;
}

bool Parse(std::string_view text, Value& out, std::string* error) {
    Parser p(text);
    Value result;
    if (!p.Run(result, error)) return false;
    out = std::move(result);
    return true;
}

std::string Dump(const Value& v, int indent) {
    std::string out;
    DumpValue(out, v, indent, 0);
    if (indent > 0) out += '\n';
    return out;
}

}  // namespace json
