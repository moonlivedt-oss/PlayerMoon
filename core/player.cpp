#include "core/player.h"

#include <algorithm>
#include <cctype>
#include <system_error>

#include "core/text.h"

namespace pm {

bool NaturalLess(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[j]);
        if (std::isdigit(ca) && std::isdigit(cb)) {
            // Сравниваем числа целиком: сначала без ведущих нулей по длине, потом по цифрам
            size_t ei = i, ej = j;
            while (ei < a.size() && std::isdigit(static_cast<unsigned char>(a[ei]))) ++ei;
            while (ej < b.size() && std::isdigit(static_cast<unsigned char>(b[ej]))) ++ej;
            size_t si = i, sj = j;
            while (si + 1 < ei && a[si] == '0') ++si;
            while (sj + 1 < ej && b[sj] == '0') ++sj;
            if (ei - si != ej - sj) return ei - si < ej - sj;
            const int cmp = a.compare(si, ei - si, b, sj, ej - sj);
            if (cmp != 0) return cmp < 0;
            i = ei;
            j = ej;
            continue;
        }
        const int la = std::tolower(ca), lb = std::tolower(cb);
        if (la != lb) return la < lb;
        ++i;
        ++j;
    }
    return a.size() - i < b.size() - j;
}

Player::Player() = default;

Player::~Player() { Shutdown(); }

bool Player::Init(const Options& options, WakeFn wake, std::string* error) {
    Engine::Options eo;
    eo.nullBackend = options.nullBackend;
    ready_ = engine_.Start(eo, std::move(wake), error);
    ApplyVolume();
    return ready_;
}

void Player::Shutdown() {
    engine_.Shutdown();
    ready_ = false;
}

int Player::LoadFiles(const std::vector<std::filesystem::path>& files, bool append) {
    if (!append) {
        engine_.Pause();
        engine_.ClearTrack();
        playlist_.Clear();
        NowPlaying empty;
        empty.serial = now_.serial + 1;
        now_ = std::move(empty);
    }
    int added = 0;
    for (const auto& f : files) {
        if (!IsSupportedAudio(f)) continue;
        TagInfo tags;
        TrackMeta meta;
        if (ReadTags(f, tags, false)) {
            meta.title = tags.title;
            meta.artist = tags.artist;
            meta.album = tags.album;
            meta.duration = tags.duration;
        }
        playlist_.Add(f, std::move(meta));
        ++added;
    }
    return added;
}

int Player::LoadFolder(const std::filesystem::path& dir, bool append) {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec) && IsSupportedAudio(it->path())) files.push_back(it->path());
    }
    std::sort(files.begin(), files.end(), [](const auto& x, const auto& y) {
        return NaturalLess(text::PathToUtf8(x), text::PathToUtf8(y));
    });
    return LoadFiles(files, append);
}

bool Player::OpenTrack(int index, double position, int direction) {
    // Битые файлы пропускаем в направлении движения, но не больше одного круга
    const int count = playlist_.Size();
    for (int attempt = 0; attempt < count; ++attempt) {
        if (index < 0 || index >= count) return false;
        PlaylistEntry& e = playlist_.At(index);
        std::string err;
        auto decoder = e.meta.broken ? nullptr : OpenDecoder(e.path, Engine::kChannels, engine_.SampleRate(), &err);
        if (decoder) {
            // Advance/Back уже сдвинули текущий трек; Select нужен только для выбора мимо порядка
            if (playlist_.Current() != index) playlist_.Select(index);
            NowPlaying np;
            np.index = index;
            np.path = e.path;
            ReadTags(e.path, np.tags, true);
            np.title = np.tags.title.empty() ? text::PathToUtf8(e.path.stem()) : np.tags.title;
            np.serial = now_.serial + 1;
            // Точная длительность - от декодера; сохраняем в плейлист для показа
            const uint32_t rate = engine_.SampleRate();
            if (decoder->LengthFrames() && rate) e.meta.duration = double(decoder->LengthFrames()) / rate;
            const uint64_t start = static_cast<uint64_t>(std::max(0.0, position) * rate);
            engine_.SetTrack(std::move(decoder), start);
            now_ = std::move(np);
            if (onTrackChanged_) onTrackChanged_(index, direction);
            return true;
        }
        e.meta.broken = true;
        index = direction < 0 ? (index - 1 + count) % count : (index + 1) % count;
    }
    return false;
}

bool Player::OpenAt(int index, double position, bool play) {
    if (!OpenTrack(index, position, 0)) return false;
    if (play) engine_.Play();
    return true;
}

void Player::Play() {
    if (!engine_.HasTrack()) {
        const int idx = playlist_.Current() >= 0 ? playlist_.Current() : 0;
        if (!OpenTrack(idx, 0, 0)) return;
    }
    engine_.Play();
}

void Player::Pause() { engine_.Pause(); }

void Player::Toggle() {
    if (engine_.IsPlaying()) Pause();
    else Play();
}

void Player::Next() {
    const int idx = playlist_.Advance(false);
    if (idx >= 0) OpenTrack(idx, 0, 1);
}

void Player::Prev() {
    if (GetState().position > 3) {
        engine_.Seek(0);
        return;
    }
    const int idx = playlist_.Back();
    if (idx >= 0) OpenTrack(idx, 0, -1);
}

void Player::PlayIndex(int index) {
    if (index < 0 || index >= playlist_.Size()) return;
    const int dir = index >= now_.index ? 1 : -1;
    if (OpenTrack(index, 0, dir)) engine_.Play();
}

void Player::Seek(double seconds) {
    const uint32_t rate = engine_.SampleRate();
    engine_.Seek(static_cast<uint64_t>(std::max(0.0, seconds) * rate));
}

void Player::SetVolume(float volume) {
    volume_ = std::clamp(volume, 0.f, 1.f);
    if (volume_ > 0) muted_ = false;
    ApplyVolume();
}

void Player::SetMuted(bool muted) {
    muted_ = muted;
    ApplyVolume();
}

void Player::ApplyVolume() {
    // Громкость ползунка - квадратичная: так середина ползунка звучит как «половина» на слух
    const float v = muted_ ? 0.f : volume_ * volume_;
    engine_.SetVolume(v);
}

void Player::SetRepeat(Repeat r) { playlist_.SetRepeat(r); }
void Player::SetShuffle(bool on) { playlist_.SetShuffle(on); }

PlayerState Player::GetState() const {
    PlayerState s;
    s.playing = engine_.IsPlaying();
    s.hasTrack = engine_.HasTrack();
    s.index = now_.index;
    s.count = playlist_.Size();
    const uint32_t rate = engine_.SampleRate();
    s.position = rate ? double(engine_.PositionFrames()) / rate : 0;
    s.duration = rate && engine_.LengthFrames() ? double(engine_.LengthFrames()) / rate : now_.tags.duration;
    s.volume = volume_;
    s.muted = muted_;
    s.repeat = playlist_.GetRepeat();
    s.shuffle = playlist_.Shuffle();
    return s;
}

void Player::PumpEvents() {
    if (!engine_.Service()) return;
    // Трек доиграл
    if (playlist_.GetRepeat() == Repeat::One) {
        engine_.Seek(0);
        return;
    }
    const int idx = playlist_.Advance(true);
    if (idx >= 0) {
        OpenTrack(idx, 0, 1);
        return;
    }
    // Конец списка без повтора: как в макете - встаём на первый трек и паузу
    engine_.Pause();
    playlist_.Select(0);
    OpenTrack(0, 0, 1);
}

size_t Player::ReadPcm(float* dst, size_t n) const { return engine_.CopyRecentMono(dst, n); }

uint32_t Player::SampleRate() const { return engine_.SampleRate(); }

}  // namespace pm
