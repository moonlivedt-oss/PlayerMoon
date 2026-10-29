#include "plugins/downloader/ytdlp.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace dl {

namespace {

std::string Lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// Хост ссылки без www. и порта
std::string Host(const std::string& url) {
    const std::string u = Lower(url);
    size_t p = u.find("://");
    if (p == std::string::npos) return {};
    p += 3;
    const size_t end = u.find_first_of("/?#:", p);
    std::string h = u.substr(p, end == std::string::npos ? std::string::npos : end - p);
    if (h.rfind("www.", 0) == 0) h.erase(0, 4);
    return h;
}

bool EndsWith(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t p = s.find(sep, start);
        out.push_back(s.substr(start, p == std::string::npos ? std::string::npos : p - start));
        if (p == std::string::npos) break;
        start = p + 1;
    }
    return out;
}

std::string Trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Убрать цветовые коды терминала (ESC [ ... m)
std::string StripAnsi(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && !((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z'))) ++i;
            continue;
        }
        out += s[i];
    }
    return out;
}

// yt-dlp печатает «NA» для неизвестных полей
std::string NotNA(const std::string& s) {
    const std::string t = Trim(s);
    return (t == "NA" || t == "None" || t == "Unknown") ? std::string() : t;
}

}  // namespace

Source DetectSource(const std::string& url) {
    const std::string u = Lower(url);
    if (u.rfind("http://", 0) != 0 && u.rfind("https://", 0) != 0) return Source::Invalid;
    const std::string h = Host(url);
    if (h.empty()) return Source::Invalid;
    if (h == "music.youtube.com") return Source::YouTubeMusic;
    if (h == "youtube.com" || h == "m.youtube.com" || h == "youtu.be" || EndsWith(h, ".youtube.com"))
        return Source::YouTube;
    if (h.rfind("music.yandex.", 0) == 0) return Source::Yandex;
    return Source::Other;
}

const char* SourceName(Source s) {
    switch (s) {
        case Source::YouTubeMusic: return "YouTube Music";
        case Source::YouTube: return "YouTube";
        case Source::Yandex: return "Яндекс Музыка";
        case Source::Other: return "Другой сайт";
        default: return "Не ссылка";
    }
}

std::vector<std::string> ExtractUrls(const std::string& text) {
    std::vector<std::string> out;
    size_t p = 0;
    while (p < text.size()) {
        size_t a = text.find("http", p);
        if (a == std::string::npos) break;
        if (text.compare(a, 7, "http://") != 0 && text.compare(a, 8, "https://") != 0) {
            p = a + 4;
            continue;
        }
        size_t b = a;
        // Конец ссылки - пробел, перевод строки, кавычка или угловая скобка
        while (b < text.size() && !std::isspace(static_cast<unsigned char>(text[b])) && text[b] != '"' &&
               text[b] != '\'' && text[b] != '<' && text[b] != '>')
            ++b;
        std::string url = text.substr(a, b - a);
        while (!url.empty() && (url.back() == '.' || url.back() == ',' || url.back() == ')' || url.back() == ';'))
            url.pop_back();
        if (DetectSource(url) != Source::Invalid && std::find(out.begin(), out.end(), url) == out.end())
            out.push_back(url);
        p = b;
    }
    return out;
}

std::vector<std::string> BuildArgs(const std::string& url, const Options& o) {
    std::vector<std::string> a = {
        "--newline", "--no-colors", "--encoding", "utf-8", "--ignore-config", "--no-mtime",
        // Шаблоны вывода с метками PM*, их разбирает ParseLine. --print прячет обычный вывод и
        // включает «только посмотреть» - поэтому --progress и --no-simulate.
        "--progress", "--no-simulate",
        "--progress-template", "download:PMPROG|%(progress._percent_str)s|%(progress._speed_str)s|%(progress._eta_str)s",
        "--print", "before_dl:PMITEM|%(playlist_index)s|%(n_entries)s|%(title)s|%(artist,creator,uploader,channel)s",
        "--print", "after_move:PMFILE|%(filepath)s",
        // Без перекодирования в MP3 берём M4A: его плеер играет через Media Foundation без расширений
        "-f", o.mp3 ? "bestaudio/best" : "bestaudio[ext=m4a]/bestaudio/best",
        "--embed-metadata", "--embed-thumbnail",
        "-o", o.outDir + "/%(artist,creator,uploader,channel)s - %(title)s.%(ext)s",
    };
    if (o.mp3) {
        a.insert(a.end(), {"-x", "--audio-format", "mp3", "--audio-quality", "0"});
    }
    if (!o.ffmpegDir.empty()) a.insert(a.end(), {"--ffmpeg-location", o.ffmpegDir});
    if (!o.browser.empty()) a.insert(a.end(), {"--cookies-from-browser", o.browser});
    a.push_back("--");  // дальше - только ссылка, даже если она начинается с «-»
    a.push_back(url);
    return a;
}

std::string QuoteArgs(const std::vector<std::string>& args) {
    std::string out;
    for (const auto& arg : args) {
        if (!out.empty()) out += ' ';
        if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) {
            out += arg;
            continue;
        }
        // Правила MSVCRT/CommandLineToArgvW: обратные косые перед кавычкой удваиваются
        out += '"';
        size_t slashes = 0;
        for (char c : arg) {
            if (c == '\\') {
                ++slashes;
            } else if (c == '"') {
                out.append(slashes * 2 + 1, '\\');
                out += '"';
                slashes = 0;
            } else {
                out.append(slashes, '\\');
                out += c;
                slashes = 0;
            }
        }
        out.append(slashes * 2, '\\');
        out += '"';
    }
    return out;
}

Event ParseLine(std::string line) {
    Event e;
    line = Trim(StripAnsi(line));
    if (line.empty()) return e;
    if (line.rfind("PMPROG|", 0) == 0) {
        const auto f = Split(line, '|');
        if (f.size() >= 4) {
            e.type = Event::Type::Progress;
            const std::string pct = Trim(f[1]);
            e.percent = pct.empty() ? -1 : std::strtod(pct.c_str(), nullptr);
            e.speed = NotNA(f[2]);
            e.eta = NotNA(f[3]);
        }
        return e;
    }
    if (line.rfind("PMITEM|", 0) == 0) {
        const auto f = Split(line, '|');
        if (f.size() >= 5) {
            e.type = Event::Type::Item;
            e.item = std::atoi(NotNA(f[1]).c_str());
            e.items = std::atoi(NotNA(f[2]).c_str());
            // В названии может быть «|» - склеиваем середину обратно
            std::string title = f[3];
            for (size_t i = 4; i + 1 < f.size(); ++i) title += "|" + f[i];
            e.title = NotNA(title);
            e.artist = NotNA(f.back());
        }
        return e;
    }
    if (line.rfind("PMFILE|", 0) == 0) {
        e.type = Event::Type::File;
        e.text = line.substr(7);
        return e;
    }
    if (line.rfind("ERROR:", 0) == 0) {
        e.type = Event::Type::Error;
        e.text = Trim(line.substr(6));
        // «[youtube] abc: Video unavailable» - убираем служебный префикс
        if (!e.text.empty() && e.text[0] == '[') {
            const size_t p = e.text.find("] ");
            if (p != std::string::npos) e.text = e.text.substr(p + 2);
        }
        return e;
    }
    if (line.rfind("WARNING:", 0) == 0) {
        e.type = Event::Type::Warning;
        e.text = Trim(line.substr(8));
        return e;
    }
    if (line.rfind("[ExtractAudio]", 0) == 0 || line.rfind("[EmbedThumbnail]", 0) == 0 ||
        line.rfind("[Metadata]", 0) == 0) {
        e.type = Event::Type::Converting;
        return e;
    }
    return e;
}

std::string HumanSpeed(const std::string& s) {
    // «2.10MiB/s» -> «2.1 МБ/с»; непонятное - как есть
    const char* units[][2] = {{"GiB/s", "ГБ/с"}, {"MiB/s", "МБ/с"}, {"KiB/s", "КБ/с"}, {"B/s", "Б/с"}};
    for (const auto& u : units) {
        const size_t p = s.find(u[0]);
        if (p == std::string::npos || p == 0) continue;
        const double v = std::strtod(s.substr(0, p).c_str(), nullptr);
        char buf[32];
        std::snprintf(buf, sizeof(buf), v >= 100 ? "%.0f %s" : "%.1f %s", v, u[1]);
        return buf;
    }
    return s;
}

}  // namespace dl
