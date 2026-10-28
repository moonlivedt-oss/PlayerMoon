@echo off
rem Сборка PlayerMoon под Windows через MSYS2 UCRT64 (как RadialMenu).
rem   build.cmd         - Release: build\PlayerMoon.exe и папка build\assets
rem   build.cmd test    - собрать и запустить тесты (build\pm_tests.exe)
rem   build.cmd clean   - удалить папку build
rem Путь к MSYS2 по умолчанию D:\msys64; другой - через переменную MSYS2.
rem Нужные пакеты: pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
setlocal
chcp 65001 >nul
cd /d "%~dp0"
if "%MSYS2%"=="" set "MSYS2=D:\msys64"
set "PATH=%MSYS2%\ucrt64\bin;%PATH%"

if /i "%~1"=="clean" (
    if exist build rmdir /s /q build
    echo Папка build удалена.
    exit /b 0
)

where g++ >nul 2>nul || (
    echo Не найден g++ в %MSYS2%\ucrt64\bin. Установите пакеты, указанные в начале build.cmd.
    exit /b 1
)

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1

if /i "%~1"=="test" (
    cmake --build build --target pm_tests || exit /b 1
    build\pm_tests.exe
    exit /b %errorlevel%
)

cmake --build build || exit /b 1
echo Готово: build\PlayerMoon.exe
