#include "theme/schema.h"

#include <cmath>
#include <map>
#include <regex>

namespace theme {

namespace {

class Validator {
public:
    explicit Validator(const json::Value& root) : root_(root) {}

    bool Check(const json::Value& node, const json::Value& v, const std::string& path, std::vector<std::string>& errs) {
        if (!node.IsObject()) return true;  // true/пустая схема - всё подходит
        bool ok = true;
        auto fail = [&](const std::string& what) {
            errs.push_back((path.empty() ? std::string("/") : path) + ": " + what);
            ok = false;
        };

        if (const json::Value* ref = node.Find("$ref")) {
            const json::Value* target = Resolve(ref->AsString());
            if (!target) fail("не найдена ссылка " + ref->AsString());
            else if (!Check(*target, v, path, errs)) ok = false;
        }
        if (const json::Value* t = node.Find("type")) {
            if (!TypeMatches(t->AsString(), v)) fail("ожидался тип " + t->AsString());
        }
        if (const json::Value* c = node.Find("const")) {
            if (*c != v) fail("значение должно быть " + json::Dump(*c, 0));
        }
        if (const json::Value* e = node.Find("enum")) {
            bool found = false;
            for (const auto& item : e->Items()) found = found || item == v;
            if (!found) fail("значение не из списка: " + json::Dump(v, 0));
        }
        if (v.IsNumber()) {
            if (const json::Value* m = node.Find("minimum"); m && v.AsNumber() < m->AsNumber())
                fail("меньше минимума " + json::Dump(*m, 0));
            if (const json::Value* m = node.Find("maximum"); m && v.AsNumber() > m->AsNumber())
                fail("больше максимума " + json::Dump(*m, 0));
        }
        if (v.IsString()) {
            if (const json::Value* m = node.Find("maxLength"); m && CodePoints(v.AsString()) > m->AsNumber())
                fail("строка длиннее " + json::Dump(*m, 0));
            if (const json::Value* p = node.Find("pattern"); p && !std::regex_search(v.AsString(), Regex(p->AsString())))
                fail("не подходит под шаблон " + p->AsString());
        }
        if (v.IsArray()) {
            if (const json::Value* m = node.Find("minItems"); m && static_cast<double>(v.Size()) < m->AsNumber())
                fail("элементов меньше " + json::Dump(*m, 0));
            if (const json::Value* m = node.Find("maxItems"); m && static_cast<double>(v.Size()) > m->AsNumber())
                fail("элементов больше " + json::Dump(*m, 0));
            if (const json::Value* items = node.Find("items")) {
                for (size_t i = 0; i < v.Items().size(); ++i)
                    if (!Check(*items, v.Items()[i], path + "/" + std::to_string(i), errs)) ok = false;
            }
        }
        if (v.IsObject()) {
            if (const json::Value* req = node.Find("required")) {
                for (const auto& key : req->Items())
                    if (!v.Find(key.AsString())) fail("нет обязательного ключа " + key.AsString());
            }
            const json::Value* props = node.Find("properties");
            const json::Value* additional = node.Find("additionalProperties");
            for (const auto& [key, value] : v.Members()) {
                const json::Value* sub = props ? props->Find(key) : nullptr;
                if (sub) {
                    if (!Check(*sub, value, path + "/" + key, errs)) ok = false;
                } else if (additional && additional->IsBool() && !additional->AsBool()) {
                    fail("лишний ключ " + key);
                } else if (additional && additional->IsObject()) {
                    if (!Check(*additional, value, path + "/" + key, errs)) ok = false;
                }
            }
        }
        if (const json::Value* all = node.Find("allOf")) {
            for (const auto& sub : all->Items())
                if (!Check(sub, v, path, errs)) ok = false;
        }
        if (const json::Value* one = node.Find("oneOf")) {
            // Ровно одна ветка должна подойти; ошибки веток показываем, только если не подошла ни одна
            int passed = 0;
            std::vector<std::string> branchErrs;
            for (const auto& sub : one->Items()) {
                std::vector<std::string> e;
                if (Check(sub, v, path, e)) ++passed;
                else branchErrs.insert(branchErrs.end(), e.begin(), e.end());
            }
            if (passed == 0) {
                errs.insert(errs.end(), branchErrs.begin(), branchErrs.end());
                fail("не подошёл ни один вариант oneOf");
            } else if (passed > 1) {
                fail("подошло несколько вариантов oneOf");
            }
        }
        return ok;
    }

private:
    static bool TypeMatches(const std::string& type, const json::Value& v) {
        if (type == "object") return v.IsObject();
        if (type == "array") return v.IsArray();
        if (type == "string") return v.IsString();
        if (type == "number") return v.IsNumber();
        if (type == "integer") return v.IsNumber() && std::floor(v.AsNumber()) == v.AsNumber();
        if (type == "boolean") return v.IsBool();
        if (type == "null") return v.IsNull();
        return true;
    }

    static double CodePoints(const std::string& s) {
        double n = 0;
        for (const char c : s)
            if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
        return n;
    }

    const std::regex& Regex(const std::string& pattern) {
        auto it = regexCache_.find(pattern);
        if (it == regexCache_.end()) it = regexCache_.emplace(pattern, std::regex(pattern)).first;
        return it->second;
    }

    // Только локальные ссылки вида #/$defs/имя
    const json::Value* Resolve(const std::string& ref) const {
        if (ref.rfind("#/", 0) != 0) return nullptr;
        const json::Value* cur = &root_;
        size_t pos = 2;
        while (cur && pos <= ref.size()) {
            const size_t next = ref.find('/', pos);
            const std::string part = ref.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
            cur = cur->Find(part);
            if (next == std::string::npos) break;
            pos = next + 1;
        }
        return cur;
    }

    const json::Value& root_;
    std::map<std::string, std::regex> regexCache_;
};

}  // namespace

bool ValidateSchema(const json::Value& schema, const json::Value& doc, std::vector<std::string>* errors) {
    Validator validator(schema);
    std::vector<std::string> errs;
    const bool ok = validator.Check(schema, doc, "", errs);
    if (errors) *errors = std::move(errs);
    return ok;
}

}  // namespace theme
