#pragma once
// Переносимая часть плагина загрузки: какие ссылки понимаем, какие аргументы даём yt-dlp и
// как читаем его вывод. Без Win32 - проверяется тестами на Linux (tests/test_downloader.cpp).
//
// yt-dlp запускается с шаблонами вывода, которые печатают строки с метками:
//   PMITEM|<номер в плейлисте>|<всего>|<название>|<исполнитель>   перед загрузкой трека
//   PMPROG|<проценты>|<скорость>|<осталось>                        ход загрузки
//   PMFILE|<путь>                                                   готовый файл (после конвертации)
// Остальное - обычные строки yt-dlp: ERROR:, WARNING:, [ExtractAudio] и т.п.

#include <string>
#include <vector>

namespace dl {

enum class Source { YouTubeMusic, YouTube, Yandex, Other, Invalid };

Source DetectSource(const std::string& url);
const char* SourceName(Source s);  // «YouTube Music», «Яндекс Музыка»...

// Все ссылки http(s) из текста буфера обмена, без повторов, в порядке появления
std::vector<std::string> ExtractUrls(const std::string& text);

struct Options {
    std::string outDir;     // куда сохранять (UTF-8)
    std::string browser;    // откуда взять вход: "", "firefox", "chrome", "edge"
    std::string ffmpegDir;  // папка с ffmpeg, если не в PATH
    bool mp3 = true;        // перекодировать в MP3 (нужен ffmpeg); иначе как есть
};

// Аргументы для yt-dlp (без имени программы)
std::vector<std::string> BuildArgs(const std::string& url, const Options& o);
// Командная строка Windows из аргументов (правила CommandLineToArgvW)
std::string QuoteArgs(const std::vector<std::string>& args);

struct Event {
    enum class Type { None, Item, Progress, File, Converting, Error, Warning };
    Type type = Type::None;
    double percent = -1;        // 0..100
    std::string speed, eta;     // как печатает yt-dlp: «2.10MiB/s», «00:05»
    int item = 0, items = 0;    // номер трека в плейлисте и сколько всего (0 - не плейлист)
    std::string title, artist;  // для Item
    std::string text;           // путь для File, сообщение для Error/Warning
};

Event ParseLine(std::string line);

// Скорость по-русски: «2.10MiB/s» -> «2.1 МБ/с»
std::string HumanSpeed(const std::string& s);

}  // namespace dl
