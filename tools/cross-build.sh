#!/usr/bin/env bash
# Проверочная сборка под Windows из Linux (mingw-w64). Запуск exe здесь невозможен -
# это только проверка, что код компилируется и линкуется.
#   tools/cross-build.sh          - Release в build-win
#   tools/cross-build.sh tests    - только pm_tests.exe
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-cross.cmake >/dev/null
if [[ "${1:-}" == "tests" ]]; then
    cmake --build build-win --target pm_tests
else
    cmake --build build-win
fi
ls -la build-win/*.exe
