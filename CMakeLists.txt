# PlayerMoon - аудиоплеер для Windows (C++20, Win32, Direct2D/DirectComposition)
#
# Цели:
#   pm_miniaudio - miniaudio (C), отдельной библиотекой, чтобы его предупреждения не мешали
#   pm_core      - ядро без окон: воспроизведение, плейлист, теги, FFT, состояние, JSON
#   pm_motion    - анимационная математика: пружина, кривые, твины
#   pm_theme     - модель темы, загрузка, проверка по схеме, палитра обложки
#   pm_view      - состояние интерфейса без Win32: раскладки, виджеты, переходы
#   pm_tests     - тесты ядра, темы, анимаций и интерфейса (собираются и на Linux)
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
    core/decoder.cpp core/engine.cpp core/player.cpp core/text.cpp core/library.cpp core/plugins.cpp)
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

# Состояние интерфейса без Win32: раскладки, виджеты, переходы режимов (проверяется тестами)
add_library(pm_view STATIC view/layout.cpp view/motion_params.cpp view/widgets.cpp view/marquee.cpp view/svg_arc.cpp
    view/visualizer.cpp view/player_view.cpp view/panel.cpp)
target_include_directories(pm_view PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(pm_view PUBLIC pm_theme pm_core pm_motion)
target_compile_options(pm_view PRIVATE ${PM_WARNINGS})

# ---------- Плагины ----------
# Пример плагина: журнал прослушиваний. Кладётся в plugins/history рядом с exe.
add_library(pm_plugin_history SHARED plugins/history/history.cpp)
target_include_directories(pm_plugin_history PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(pm_plugin_history PRIVATE ${PM_WARNINGS})
set_target_properties(pm_plugin_history PROPERTIES OUTPUT_NAME history PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins/history
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins/history
    CXX_VISIBILITY_PRESET hidden)
if(WIN32)
    target_link_libraries(pm_plugin_history PRIVATE shell32)
endif()
add_custom_command(TARGET pm_plugin_history POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/plugins/history/plugin.json
        ${CMAKE_BINARY_DIR}/plugins/history/plugin.json)

# Плагин «Загрузка музыки» (только Windows: запускает yt-dlp). Кладётся в plugins/downloader.
if(WIN32)
    add_library(pm_plugin_downloader SHARED plugins/downloader/downloader.cpp plugins/downloader/ytdlp.cpp)
    target_include_directories(pm_plugin_downloader PRIVATE ${CMAKE_SOURCE_DIR})
    target_compile_options(pm_plugin_downloader PRIVATE ${PM_WARNINGS})
    target_link_libraries(pm_plugin_downloader PRIVATE shell32 ole32)
    set_target_properties(pm_plugin_downloader PROPERTIES OUTPUT_NAME downloader PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins/downloader)
    add_custom_command(TARGET pm_plugin_downloader POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/plugins/downloader/plugin.json
            ${CMAKE_BINARY_DIR}/plugins/downloader/plugin.json)
endif()

# Поддельный yt-dlp для проверки плагина под Wine без сети (не входит в поставку)
if(WIN32)
    add_executable(pm_fake_ytdlp tests/fake_ytdlp/fake_ytdlp.cpp)
    target_link_options(pm_fake_ytdlp PRIVATE -municode)
    set_target_properties(pm_fake_ytdlp PROPERTIES OUTPUT_NAME yt-dlp RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)
endif()

# ---------- Тесты ----------
# Тестовый плагин версии 2 (страницы, post из своего потока) - только для pm_tests
add_library(pm_test_plugin_pages SHARED tests/plugin_pages/page_test.cpp)
target_include_directories(pm_test_plugin_pages PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(pm_test_plugin_pages PRIVATE ${PM_WARNINGS})
set_target_properties(pm_test_plugin_pages PROPERTIES OUTPUT_NAME pagetest PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test-plugins/pagetest
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test-plugins/pagetest
    CXX_VISIBILITY_PRESET hidden)
if(NOT WIN32)
    target_link_libraries(pm_test_plugin_pages PRIVATE Threads::Threads)
endif()
add_custom_command(TARGET pm_test_plugin_pages POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/tests/plugin_pages/plugin.json
        ${CMAKE_BINARY_DIR}/test-plugins/pagetest/plugin.json)

add_executable(pm_tests tests/main.cpp tests/test_motion.cpp tests/test_json.cpp tests/test_theme.cpp
    tests/test_playlist.cpp tests/test_tags.cpp tests/test_fft.cpp tests/test_state.cpp tests/test_engine.cpp
    tests/test_view.cpp tests/test_plugins.cpp tests/test_downloader.cpp
    plugins/downloader/ytdlp.cpp)
target_link_libraries(pm_tests PRIVATE pm_view pm_theme pm_core pm_motion)
target_compile_options(pm_tests PRIVATE ${PM_WARNINGS})
# GCC 13 даёт ложное срабатывание stringop-overflow на vector::push_back при -O3 в тестах,
# где байты файлов собираются вручную. Отключаем только для тестов.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(pm_tests PRIVATE -Wno-stringop-overflow)
endif()
target_compile_definitions(pm_tests PRIVATE PM_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    PM_PLUGINS_DIR="${CMAKE_BINARY_DIR}/plugins" PM_TEST_PLUGINS_DIR="${CMAKE_BINARY_DIR}/test-plugins")
add_dependencies(pm_tests pm_plugin_history pm_test_plugin_pages)
enable_testing()
add_test(NAME pm_tests COMMAND pm_tests)

# ---------- Окно (только Windows) ----------
if(WIN32)
    file(GLOB PM_UI_SOURCES CONFIGURE_DEPENDS ui/*.cpp)
    add_executable(PlayerMoon WIN32 ${PM_UI_SOURCES} ui/res/app.rc)
    target_link_libraries(PlayerMoon PRIVATE pm_view pm_theme pm_core pm_motion
        d2d1 dwrite dcomp d3d11 dxgi dxguid dwmapi windowscodecs ole32 oleaut32 uuid shell32 shlwapi user32 uxtheme advapi32 gdi32)
    add_dependencies(PlayerMoon pm_plugin_history pm_plugin_downloader)
    target_compile_options(PlayerMoon PRIVATE ${PM_WARNINGS})
    target_link_options(PlayerMoon PRIVATE -municode)
    set_target_properties(PlayerMoon PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    # assets рядом с exe: темы и схему можно править без пересборки
    add_custom_command(TARGET PlayerMoon POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:PlayerMoon>/assets)
endif()
