#pragma once
// Минимальный тестовый каркас в духе RadialMenu: CHECK(x) и регистрация тестов.
// Без исключений: проваленная проверка печатается и считается, тест идёт дальше.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace tests {

struct Registry {
    struct Entry {
        const char* name;
        void (*fn)();
    };
    static std::vector<Entry>& All() {
        static std::vector<Entry> v;
        return v;
    }
    static int& Failures() {
        static int n = 0;
        return n;
    }
    static int& Checks() {
        static int n = 0;
        return n;
    }
};

struct AutoRegister {
    AutoRegister(const char* name, void (*fn)()) { Registry::All().push_back({name, fn}); }
};

inline bool Check(bool ok, const char* expr, const char* file, int line) {
    ++Registry::Checks();
    if (!ok) {
        ++Registry::Failures();
        std::printf("  ПРОВАЛ %s:%d: %s\n", file, line, expr);
    }
    return ok;
}

inline bool Near(double a, double b, double eps) { return std::abs(a - b) <= eps; }

// Каталог исходников (для чтения assets в тестах)
inline std::string SourceDir() { return PM_SOURCE_DIR; }

}  // namespace tests

#define CHECK(x) ::tests::Check(static_cast<bool>(x), #x, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, eps) \
    ::tests::Check(::tests::Near((a), (b), (eps)), #a " ~ " #b, __FILE__, __LINE__)
#define TEST(name)                                                \
    static void name();                                           \
    static ::tests::AutoRegister name##_reg(#name, &name);        \
    static void name()
