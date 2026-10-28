#include "core/library.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <unordered_map>

#include "core/player.h"
#include "core/tags.h"
#include "core/text.h"

namespace pm {

namespace {

// Путь для сравнения: абсолютный, без «..», без разделителя в конце; на Windows без учёта регистра
std::string Key(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::path a = std::filesystem::absolute(p, ec).lexically_normal();
    std::string s = text::PathToUtf8(a);
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
#ifdef _WIN32
    std::replace(s.begin(), s.end(), '/', '\\');
    s = text::AsciiLower(s);
#endif
    return s;
}

// a лежит внутри b (или совпадает)
bool Inside(const std::string& a, const std::string& b) {
    if (a.size() < b.size() || a.compare(0, b.size(), b) != 0) return false;
    return a.size() == b.size() || a[b.size()] == '/' || a[b.size()] == '\\';
}

int64_t MTime(const std::filesystem::directory_entry& e) {
    std::error_code ec;
    const auto t = e.last_write_time(ec);
    return ec ? 0 : static_cast<int64_t>(t.time_since_epoch().count());
}

}  // namespace

bool Library::AddFolder(const std::filesystem::path& dir) {
    if (dir.empty()) return false;
    const std::string k = Key(dir);
    for (const auto& f : folders_)
        if (Inside(k, Key(f))) return false;
    // Новая папка охватывает известные - они больше не нужны
    folders_.erase(std::remove_if(folders_.begin(), folders_.end(), [&](const auto& f) { return Inside(Key(f), k); }),
                   folders_.end());
    folders_.push_back(std::filesystem::absolute(dir).lexically_normal());
    return true;
}

bool Library::RemoveFolder(const std::filesystem::path& dir) {
    const std::string k = Key(dir);
    const size_t before = folders_.size();
    folders_.erase(std::remove_if(folders_.begin(), folders_.end(), [&](const auto& f) { return Key(f) == k; }),
                   folders_.end());
    if (folders_.size() == before) return false;
    // Треки из убранной папки уходят сразу, не дожидаясь сканирования
    tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(), [&](const LibraryTrack& t) { return !Contains(t.path); }),
                  tracks_.end());
    return true;
}

bool Library::LearnFrom(const std::filesystem::path& opened) {
    std::error_code ec;
    const bool isDir = std::filesystem::is_directory(opened, ec);
    return AddFolder(isDir ? opened : opened.parent_path());
}

bool Library::Contains(const std::filesystem::path& file) const {
    const std::string k = Key(file);
    for (const auto& f : folders_)
        if (Inside(k, Key(f))) return true;
    return false;
}

void Library::SetTracks(std::vector<LibraryTrack> tracks) {
    std::unordered_map<std::string, const LibraryTrack*> old;
    for (const auto& t : tracks_) old[Key(t.path)] = &t;
    for (auto& t : tracks) {
        auto it = old.find(Key(t.path));
        if (it != old.end()) {
            t.plays = it->second->plays;
            t.lastPlayed = it->second->lastPlayed;
        }
    }
    tracks_ = std::move(tracks);
}

void Library::NotePlayed(const std::filesystem::path& file, int64_t unixTime) {
    const std::string k = Key(file);
    for (auto& t : tracks_)
        if (Key(t.path) == k) {
            ++t.plays;
            t.lastPlayed = unixTime;
            return;
        }
}

std::vector<PlaylistEntry> Library::Entries() const {
    std::vector<PlaylistEntry> out;
    out.reserve(tracks_.size());
    for (const auto& t : tracks_) out.push_back({t.path, t.meta});
    return out;
}

std::vector<LibraryTrack> Library::Scan(const std::vector<std::filesystem::path>& folders,
                                        const std::vector<LibraryTrack>& previous, const std::atomic<bool>* cancel) {
    std::unordered_map<std::string, const LibraryTrack*> cache;
    for (const auto& t : previous) cache[Key(t.path)] = &t;
    std::vector<LibraryTrack> out;
    std::unordered_map<std::string, bool> seen;  // папки могут пересекаться через ссылки
    for (const auto& dir : folders) {
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(
                 dir, std::filesystem::directory_options::skip_permission_denied, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (cancel && cancel->load()) return {};
            // Скрытые и служебные папки («.git», «$RECYCLE.BIN») не обходим
            const std::string name = text::PathToUtf8(it->path().filename());
            if (it->is_directory(ec) && !name.empty() && (name[0] == '.' || name[0] == '$')) {
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec) || !IsSupportedAudio(it->path())) continue;
            const std::string k = Key(it->path());
            if (seen[k]) continue;
            seen[k] = true;
            LibraryTrack t;
            t.path = it->path();
            t.size = it->file_size(ec);
            t.mtime = MTime(*it);
            auto c = cache.find(k);
            if (c != cache.end() && c->second->size == t.size && c->second->mtime == t.mtime) {
                t.meta = c->second->meta;
                t.trackNo = c->second->trackNo;
            } else {
                TagInfo tags;
                if (ReadTags(t.path, tags, false)) {
                    t.meta.title = tags.title;
                    t.meta.artist = tags.artist;
                    t.meta.album = tags.album;
                    t.meta.duration = tags.duration;
                    t.trackNo = tags.track;
                }
            }
            out.push_back(std::move(t));
        }
    }
    // Порядок: папка (альбом), номер трека, имя файла «как человек»
    std::sort(out.begin(), out.end(), [](const LibraryTrack& a, const LibraryTrack& b) {
        const std::string pa = text::PathToUtf8(a.path.parent_path()), pb = text::PathToUtf8(b.path.parent_path());
        if (pa != pb) return NaturalLess(pa, pb);
        if (a.trackNo != b.trackNo) return (a.trackNo ? a.trackNo : 100000) < (b.trackNo ? b.trackNo : 100000);
        return NaturalLess(text::PathToUtf8(a.path.filename()), text::PathToUtf8(b.path.filename()));
    });
    return out;
}

json::Value Library::ToJson() const {
    json::Value v = json::Value::MakeObject();
    v["version"] = 1;
    v["autoScan"] = autoScan_;
    json::Value folders = json::Value::MakeArray();
    for (const auto& f : folders_) folders.Push(text::PathToUtf8(f));
    v["folders"] = std::move(folders);
    json::Value tracks = json::Value::MakeArray();
    for (const auto& t : tracks_) {
        json::Value o = json::Value::MakeObject();
        o["path"] = text::PathToUtf8(t.path);
        if (!t.meta.title.empty()) o["title"] = t.meta.title;
        if (!t.meta.artist.empty()) o["artist"] = t.meta.artist;
        if (!t.meta.album.empty()) o["album"] = t.meta.album;
        if (t.meta.duration > 0) o["duration"] = std::round(t.meta.duration * 100) / 100;
        if (t.trackNo) o["track"] = t.trackNo;
        o["size"] = static_cast<double>(t.size);
        o["mtime"] = std::to_string(t.mtime);  // строкой: в double счётчик времени Windows теряет точность
        if (t.plays) o["plays"] = static_cast<double>(t.plays);
        if (t.lastPlayed) o["lastPlayed"] = static_cast<double>(t.lastPlayed);
        tracks.Push(std::move(o));
    }
    v["tracks"] = std::move(tracks);
    return v;
}

void Library::FromJson(const json::Value& v) {
    folders_.clear();
    tracks_.clear();
    autoScan_ = v.Get("autoScan").AsBool(true);
    for (const auto& f : v.Get("folders").Items())
        if (f.IsString() && !f.AsString().empty()) folders_.push_back(text::Utf8ToPath(f.AsString()));
    for (const auto& o : v.Get("tracks").Items()) {
        if (!o.Get("path").IsString()) continue;
        LibraryTrack t;
        t.path = text::Utf8ToPath(o.Get("path").AsString());
        t.meta.title = o.Get("title").AsString();
        t.meta.artist = o.Get("artist").AsString();
        t.meta.album = o.Get("album").AsString();
        t.meta.duration = std::max(0.0, o.Get("duration").AsNumber(0));
        t.trackNo = static_cast<int>(o.Get("track").AsNumber(0));
        t.size = static_cast<uint64_t>(std::max(0.0, o.Get("size").AsNumber(0)));
        t.mtime = std::strtoll(o.Get("mtime").AsString().c_str(), nullptr, 10);
        t.plays = static_cast<uint32_t>(std::max(0.0, o.Get("plays").AsNumber(0)));
        t.lastPlayed = static_cast<int64_t>(o.Get("lastPlayed").AsNumber(0));
        tracks_.push_back(std::move(t));
    }
}

bool Library::Save(const std::filesystem::path& file) const {
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    std::filesystem::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        const std::string data = json::Dump(ToJson(), 1);
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!f) return false;
    }
    std::filesystem::rename(tmp, file, ec);
    return !ec;
}

bool Library::Load(const std::filesystem::path& file) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return false;
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    json::Value v;
    if (!json::Parse(data, v) || !v.IsObject()) return false;
    FromJson(v);
    return true;
}

// ---------------- фоновое сканирование ----------------

LibraryScanner::~LibraryScanner() { Stop(); }

bool LibraryScanner::Start(std::vector<std::filesystem::path> folders, std::vector<LibraryTrack> previous,
                           std::function<void()> done) {
    if (running_.load()) return false;
    if (thread_.joinable()) thread_.join();
    running_ = true;
    cancel_ = false;
    ready_ = false;
    thread_ = std::thread([this, folders = std::move(folders), previous = std::move(previous),
                           done = std::move(done)] {
        result_ = Library::Scan(folders, previous, &cancel_);
        ready_ = !cancel_.load();
        running_ = false;
        if (ready_ && done) done();
    });
    return true;
}

bool LibraryScanner::Take(std::vector<LibraryTrack>& out) {
    if (running_.load() || !ready_.exchange(false)) return false;
    if (thread_.joinable()) thread_.join();
    out = std::move(result_);
    result_.clear();
    return true;
}

void LibraryScanner::Stop() {
    cancel_ = true;
    if (thread_.joinable()) thread_.join();
    running_ = false;
}

}  // namespace pm
