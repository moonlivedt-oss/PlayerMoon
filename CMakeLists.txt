# PlayerMoon - аудиоплеер для Windows (C++20, Win32, Direct2D/DirectComposition)
#
# Цели:
#   pm_miniaudio - miniaudio (C), отдельной библиотекой, чтобы его предупреждения не мешали
#   pm_core      - ядро без окон: воспроизведение, плейлист, теги, FFT, состояние, JSON
#   pm_motion    - анимационная математика: пружина, кривые, твины
#   pm_theme     - модель темы, загрузка, проверка по схеме, палитра обложки
#   pm_tests     - тесты ядра, темы и анимаций (собираются и на Linux)
#   PlayerMoon   - окно (только Windows)
cmake_minimum_required(VERSION 3.20)
project(PlayerMoon LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PM_WARNINGS -Wall -Wextra -Wshadow)
option(PM_WERROR "Предупреждения своего кода считать ошибками" ON)
if(PM_WERROR)
    list(APPEND PM_WARNINGS -Werror)
endif()

if(WIN32)
    add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00)
    # Статическая сборка, как у RadialMenu: один exe без DLL рантайма
    add_link_options(-static -static-libgcc -static-libstdc++)
endif()

# ---------- miniaudio ----------
add_library(pm_miniaudio STATIC third_party/miniaudio/miniaudio_impl.c)
target_include_directories(pm_miniaudio PUBLIC third_party/miniaudio)
target_compile_options(pm_miniaudio PRIVATE -w)
if(WIN32)
    target_link_libraries(pm_miniaudio PUBLIC ole32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(pm_miniaudio PUBLIC Threads::Threads m ${CMAKE_DL_LIBS})
endif()

# ---------- Встроенные ресурсы: темы, схема, раскладки ----------
# Файлы из assets/ зашиваются в exe как запасной вариант; при запуске
# приоритет у файлов рядом с exe (их можно править без пересборки).
set(PM_GEN_DIR ${CMAKE_BINARY_DIR}/generated)
set(PM_ASSET_FILES
    assets/theme-schema.json assets/layouts.json
    assets/themes/glass.json assets/themes/frost.json assets/themes/neon.json
    assets/themes/sunset.json assets/themes/minimal.json)
set(PM_ASSET_ABS)
foreach(f ${PM_ASSET_FILES})
    list(APPEND PM_ASSET_ABS ${CMAKE_SOURCE_DIR}/${f})
endforeach()
string(REPLACE ";" "|" PM_ASSET_LIST "${PM_ASSET_FILES}")
add_custom_command(
    OUTPUT ${PM_GEN_DIR}/builtin_assets.h
    COMMAND ${CMAKE_COMMAND} -DOUT=${PM_GEN_DIR}/builtin_assets.h -DSRC=${CMAKE_SOURCE_DIR}
            -DFILES=${PM_ASSET_LIST} -P ${CMAKE_SOURCE_DIR}/cmake/embed_assets.cmake
    DEPENDS ${PM_ASSET_ABS} ${CMAKE_SOURCE_DIR}/cmake/embed_assets.cmake
    COMMENT "Встраиваю assets в builtin_assets.h"
    VERBATIM)
add_custom_target(pm_assets DEPENDS ${PM_GEN_DIR}/builtin_assets.h)

# ---------- Ядро ----------
add_library(pm_core STATIC
    core/json.cpp core/playlist.cpp core/tags.cpp core/fft.cpp core/state.cpp
    core/decoder.cpp core/engine.cpp core/player.cpp core/text.cpp)
target_include_directories(pm_core PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(pm_core PUBLIC pm_miniaudio)
target_compile_options(pm_core PRIVATE ${PM_WARNINGS})

add_library(pm_motion STATIC motion/spring.cpp motion/easing.cpp motion/tween.cpp)
target_include_directories(pm_motion PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(pm_motion PRIVATE ${PM_WARNINGS})

add_library(pm_theme STATIC theme/schema.cpp theme/theme.cpp theme/palette.cpp theme/assets.cpp)
target_include_directories(pm_theme PUBLIC ${CMAKE_SOURCE_DIR} ${PM_GEN_DIR})
target_link_libraries(pm_theme PUBLIC pm_core pm_motion)
target_compile_options(pm_theme PRIVATE ${PM_WARNINGS})
add_dependencies(pm_theme pm_assets)

# ---------- Тесты ----------
add_executable(pm_tests tests/main.cpp tests/test_motion.cpp tests/test_json.cpp tests/test_theme.cpp
    tests/test_playlist.cpp tests/test_tags.cpp tests/test_fft.cpp tests/test_state.cpp tests/test_engine.cpp)
target_link_libraries(pm_tests PRIVATE pm_theme pm_core pm_motion)
target_compile_options(pm_tests PRIVATE ${PM_WARNINGS})
# GCC 13 даёт ложное срабатывание stringop-overflow на vector::push_back при -O3 в тестах,
# где байты файлов собираются вручную. Отключаем только для тестов.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(pm_tests PRIVATE -Wno-stringop-overflow)
endif()
target_compile_definitions(pm_tests PRIVATE PM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
enable_testing()
add_test(NAME pm_tests COMMAND pm_tests)

# ---------- Окно (только Windows) ----------
if(WIN32)
    file(GLOB PM_UI_SOURCES CONFIGURE_DEPENDS ui/*.cpp)
    add_executable(PlayerMoon WIN32 ${PM_UI_SOURCES} ui/res/app.rc)
    target_link_libraries(PlayerMoon PRIVATE pm_theme pm_core pm_motion
        d2d1 dwrite dcomp d3d11 dxgi dwmapi windowscodecs ole32 oleaut32 uuid shell32 shlwapi user32 uxtheme)
    target_compile_options(PlayerMoon PRIVATE ${PM_WARNINGS})
    target_link_options(PlayerMoon PRIVATE -municode)
    set_target_properties(PlayerMoon PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    # assets рядом с exe: темы и схему можно править без пересборки
    add_custom_command(TARGET PlayerMoon POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:PlayerMoon>/assets)
endif()
