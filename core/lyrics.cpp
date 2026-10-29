#include "core/lyrics.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "core/json.h"
#include "core/text.h"

namespace pm {

namespace {

constexpr int64_t kRetryNotFound = 14 * 24 * 3600;  // «не найдено» проверяем заново через две недели
constexpr size_t kMaxLrcFile = 1u << 20;

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

// «мм:сс», «мм:сс.xx», «мм:сс:xx» -> секунды; false - не метка времени
bool ParseTimeTag(std::string_view t, double& out) {
    size_t i = 0;
    long minutes = 0;
    if (i >= t.size() || !IsDigit(t[i])) return false;
    while (i < t.size() && IsDigit(t[i]) && i < 4) minutes = minutes * 10 + (t[i++] - '0');
    if (i >= t.size() || t[i] != ':') return false;
    ++i;
    if (i + 2 > t.size() || !IsDigit(t[i]) || !IsDigit(t[i + 1])) return false;
    const int seconds = (t[i] - '0') * 10 + (t[i + 1] - '0');
    i += 2;
    double frac = 0;
    if (i < t.size() && (t[i] == '.' || t[i] == ':')) {
        ++i;
        double scale = 0.1;
        size_t digits = 0;
        while (i < t.size() && IsDigit(t[i]) && digits < 3) {
            frac += (t[i++] - '0') * scale;
            scale /= 10;
            ++digits;
        }
        if (digits == 0) return false;
    }
    if (i != t.size() || seconds > 59) return false;
    out = minutes * 60.0 + seconds + frac;
    return true;
}

// Метки слов «<мм:сс.xx>» внутри строки (расширенный LRC) убираются
std::string StripWordTags(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '<') {
            const size_t close = s.find('>', i);
            double t = 0;
            if (close != std::string_view::npos && ParseTimeTag(s.substr(i + 1, close - i - 1), t)) {
                i = close;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

std::string Lower(std::string s) {
    // Только ASCII и кириллица в UTF-8 (А-Я, Ё): для ключа кэша этого хватает
    std::string out;
    for (char32_t c : text::CodePoints(s)) {
        if (c >= 'A' && c <= 'Z') c += 32;
        else if (c >= 0x410 && c <= 0x42F) c += 32;
        else if (c == 0x401) c = 0x451;
        text::AppendUtf8(out, c);
    }
    return out;
}

bool ReadFile(const std::filesystem::path& p, std::string& out) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(p, ec);
    if (ec || size > kMaxLrcFile) return false;
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(static_cast<size_t>(size), '\0');
    f.read(out.data(), static_cast<std::streamsize>(size));
    out.resize(static_cast<size_t>(f.gcount()));
    // BOM UTF-8
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF && static_cast<unsigned char>(out[1]) == 0xBB &&
        static_cast<unsigned char>(out[2]) == 0xBF)
        out.erase(0, 3);
    return true;
}

bool WriteFile(const std::filesystem::path& p, const std::string& data) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    const std::filesystem::path tmp = p.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!f) return false;
    }
    std::filesystem::rename(tmp, p, ec);
    return !ec;
}

// Служебные метки кэша
constexpr std::string_view kNotFound = "[pm:notfound:";
constexpr std::string_view kInstrumental = "[pm:instrumental]";

}  // namespace

Lyrics ParseLrc(std::string_view text) {
    Lyrics out;
    std::vector<std::string> plain;
    double offset = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        std::vector<double> times;
        bool tagsOnly = false;
        // Метки в начале строки
        while (!line.empty() && line.front() == '[') {
            const size_t close = line.find(']');
            if (close == std::string_view::npos) break;
            const std::string_view tag = line.substr(1, close - 1);
            double t = 0;
            if (ParseTimeTag(tag, t)) {
                times.push_back(t);
            } else if (tag.rfind("offset:", 0) == 0) {
                offset = std::atof(std::string(tag.substr(7)).c_str()) / 1000.0;
                tagsOnly = true;
            } else if (tag.find(':') != std::string_view::npos && times.empty()) {
                tagsOnly = true;  // [ar:...], [ti:...], [length:...]
            } else {
                break;
            }
            line.remove_prefix(close + 1);
        }
        const std::string body = text::Trim(StripWordTags(line));
        if (!times.empty()) {
            for (double t : times) out.lines.push_back({t, body});
        } else if (!(tagsOnly && body.empty())) {
            plain.push_back(body);
        }
        if (end == text.size()) break;
    }
    if (out.lines.empty()) {
        // Простой текст: пустые строки по краям убираем, внутри (между куплетами) оставляем
        while (!plain.empty() && plain.back().empty()) plain.pop_back();
        size_t first = 0;
        while (first < plain.size() && plain[first].empty()) ++first;
        for (size_t i = first; i < plain.size(); ++i) out.lines.push_back({-1, plain[i]});
        return out;
    }
    out.synced = true;
    // Положительный offset - текст раньше
    for (auto& l : out.lines) l.time = std::max(0.0, l.time - offset);
    std::stable_sort(out.lines.begin(), out.lines.end(),
                     [](const LyricLine& a, const LyricLine& b) { return a.time < b.time; });
    // Пустые строки в конце (метка конца песни) не нужны
    while (!out.lines.empty() && out.lines.back().text.empty()) out.lines.pop_back();
    if (out.lines.empty()) out.synced = false;
    return out;
}

int LyricLineAt(const Lyrics& lyrics, double seconds) {
    if (!lyrics.synced || lyrics.lines.empty()) return -1;
    const auto it = std::upper_bound(lyrics.lines.begin(), lyrics.lines.end(), seconds,
                                     [](double s, const LyricLine& l) { return s < l.time; });
    return static_cast<int>(it - lyrics.lines.begin()) - 1;
}

std::string ToLrc(const Lyrics& lyrics) {
    std::string out;
    for (const auto& l : lyrics.lines) {
        if (lyrics.synced) {
            const long cs = std::lround(l.time * 100);
            char buf[32];
            std::snprintf(buf, sizeof buf, "[%02ld:%02ld.%02ld]", cs / 6000, (cs / 100) % 60, cs % 100);
            out += buf;
        }
        out += l.text;
        out += '\n';
    }
    return out;
}

std::string UrlEncode(std::string_view s) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

void SplitArtistTitle(LyricsRequest& r) {
    if (!r.artist.empty()) return;
    const size_t dash = r.title.find(" - ");
    if (dash == std::string::npos) return;
    r.artist = text::Trim(r.title.substr(0, dash));
    r.title = text::Trim(r.title.substr(dash + 3));
}

std::string LrclibGetQuery(const LyricsRequest& r) {
    std::string q = "/api/get?track_name=" + UrlEncode(r.title) + "&artist_name=" + UrlEncode(r.artist);
    if (!r.album.empty()) q += "&album_name=" + UrlEncode(r.album);
    if (r.duration > 0) q += "&duration=" + std::to_string(std::lround(r.duration));
    return q;
}

std::string LrclibSearchQuery(const LyricsRequest& r) {
    return "/api/search?track_name=" + UrlEncode(r.title) + "&artist_name=" + UrlEncode(r.artist);
}

LyricsStatus ParseLrclibResponse(std::string_view body, double duration, Lyrics& out) {
    out = Lyrics{};
    json::Value v;
    if (!json::Parse(body, v)) return LyricsStatus::NetworkError;
    const json::Value* best = nullptr;
    if (v.IsObject()) {
        best = &v;
    } else if (v.IsArray()) {
        // Синхронный текст лучше простого, ближе по длительности - лучше; дальше 5 с - не тот трек
        double bestScore = 1e9;
        for (const auto& item : v.Items()) {
            if (!item.IsObject()) continue;
            const double d = item.Get("duration").AsNumber(0);
            const double diff = duration > 0 && d > 0 ? std::abs(d - duration) : 0;
            if (diff > 5) continue;
            const bool synced = !item.Get("syncedLyrics").AsString().empty();
            const bool any = synced || !item.Get("plainLyrics").AsString().empty() ||
                             item.Get("instrumental").AsBool();
            if (!any) continue;
            const double score = diff + (synced ? 0 : 100);
            if (score < bestScore) {
                bestScore = score;
                best = &item;
            }
        }
    }
    if (!best) return LyricsStatus::NotFound;
    if (best->Get("instrumental").AsBool()) return LyricsStatus::Instrumental;
    const std::string& synced = best->Get("syncedLyrics").AsString();
    if (!synced.empty()) out = ParseLrc(synced);
    if (out.Empty()) out = ParseLrc(best->Get("plainLyrics").AsString());
    if (out.Empty()) return LyricsStatus::NotFound;
    return LyricsStatus::Found;
}

std::string LyricsCacheName(const LyricsRequest& r) {
    // FNV-1a 64
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : Lower(text::Trim(r.artist)) + "\n" + Lower(text::Trim(r.title))) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[24];
    std::snprintf(buf, sizeof buf, "%016llx.lrc", static_cast<unsigned long long>(h));
    return buf;
}

LyricsResult FindLyrics(LyricsRequest r, const std::filesystem::path& cacheDir, const HttpGet& get, int64_t now) {
    LyricsResult res;
    res.serial = r.serial;
    std::string data;
    // 1. Рядом с треком: «Трек.lrc»
    if (!r.track.empty()) {
        for (const char* ext : {".lrc", ".LRC"}) {
            std::filesystem::path p = r.track;
            p.replace_extension(ext);
            if (ReadFile(p, data)) {
                res.lyrics = ParseLrc(data);
                if (!res.lyrics.Empty()) {
                    res.status = LyricsStatus::Found;
                    return res;
                }
            }
        }
    }
    SplitArtistTitle(r);
    if (r.title.empty()) return res;
    // 2. Кэш
    const std::filesystem::path cached = cacheDir.empty() ? std::filesystem::path() : cacheDir / LyricsCacheName(r);
    if (!cached.empty() && ReadFile(cached, data)) {
        if (data.rfind(kInstrumental, 0) == 0) {
            res.status = LyricsStatus::Instrumental;
            return res;
        }
        if (data.rfind(kNotFound, 0) == 0) {
            const int64_t at = std::atoll(data.c_str() + kNotFound.size());
            if (now - at < kRetryNotFound) return res;  // NotFound
        } else {
            res.lyrics = ParseLrc(data);
            if (!res.lyrics.Empty()) {
                res.status = LyricsStatus::Found;
                return res;
            }
        }
    }
    // 3. LRCLIB: точный запрос, потом поиск
    if (!get || r.artist.empty()) return res;
    std::string body;
    int code = get(LrclibGetQuery(r), body);
    LyricsStatus st = LyricsStatus::NetworkError;
    if (code == 200) st = ParseLrclibResponse(body, r.duration, res.lyrics);
    else if (code == 404 || code == 400) st = LyricsStatus::NotFound;
    if (st == LyricsStatus::NotFound) {
        body.clear();
        code = get(LrclibSearchQuery(r), body);
        st = code == 200 ? ParseLrclibResponse(body, r.duration, res.lyrics) : LyricsStatus::NetworkError;
    }
    res.status = st;
    if (!cached.empty()) {
        if (st == LyricsStatus::Found) WriteFile(cached, ToLrc(res.lyrics));
        else if (st == LyricsStatus::Instrumental) WriteFile(cached, std::string(kInstrumental) + "\n");
        else if (st == LyricsStatus::NotFound) WriteFile(cached, std::string(kNotFound) + std::to_string(now) + "]\n");
    }
    return res;
}

// ---------------- фоновый поток ----------------

void LyricsService::Start(std::filesystem::path cacheDir, HttpGet get, std::function<void()> ready) {
    Stop();
    state_ = std::make_shared<State>();
    state_->cacheDir = std::move(cacheDir);
    state_->get = std::move(get);
    state_->ready = std::move(ready);
    thread_ = std::thread(&LyricsService::Run, state_);
}

void LyricsService::Request(LyricsRequest r) {
    if (!state_) return;
    std::lock_guard<std::mutex> lock(state_->mu);
    state_->request = std::move(r);
    state_->hasRequest = true;
    state_->cv.notify_one();
}

bool LyricsService::Take(LyricsResult& out) {
    if (!state_) return false;
    std::lock_guard<std::mutex> lock(state_->mu);
    if (!state_->hasResult) return false;
    out = std::move(state_->result);
    state_->hasResult = false;
    return true;
}

void LyricsService::Stop() {
    if (!state_) return;
    bool busy;
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        state_->stop = true;
        busy = state_->busy;
        state_->cv.notify_one();
    }
    if (thread_.joinable()) {
        if (busy) thread_.detach();  // ждёт ответа сети: доработает с общим состоянием и выйдет
        else thread_.join();
    }
    state_.reset();
}

void LyricsService::Run(std::shared_ptr<State> s) {
    for (;;) {
        LyricsRequest r;
        {
            std::unique_lock<std::mutex> lock(s->mu);
            s->cv.wait(lock, [&] { return s->stop || s->hasRequest; });
            if (s->stop) return;
            r = std::move(s->request);
            s->hasRequest = false;
            s->busy = true;
        }
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        LyricsResult res = FindLyrics(std::move(r), s->cacheDir, s->get, now);
        bool notify = false;
        {
            std::lock_guard<std::mutex> lock(s->mu);
            s->busy = false;
            if (s->stop) return;
            // Пока искали, пришёл новый запрос - этот результат уже не нужен
            if (!s->hasRequest) {
                s->result = std::move(res);
                s->hasResult = true;
                notify = true;
            }
        }
        if (notify && s->ready) s->ready();
    }
}

}  // namespace pm
