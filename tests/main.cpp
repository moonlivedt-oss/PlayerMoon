// Запуск всех тестов. Код возврата 0 - всё прошло.
#include <cstring>

#include "tests/check.h"

int main(int argc, char** argv) {
    const char* only = argc > 1 ? argv[1] : nullptr;  // имя или часть имени теста
    int run = 0;
    for (const auto& t : tests::Registry::All()) {
        if (only && !std::strstr(t.name, only)) continue;
        const int before = tests::Registry::Failures();
        t.fn();
        ++run;
        std::printf("%s %s\n", tests::Registry::Failures() == before ? "ok    " : "ПРОВАЛ", t.name);
    }
    std::printf("\nТестов: %d, проверок: %d, провалов: %d\n", run, tests::Registry::Checks(),
                tests::Registry::Failures());
    return tests::Registry::Failures() == 0 ? 0 : 1;
}
