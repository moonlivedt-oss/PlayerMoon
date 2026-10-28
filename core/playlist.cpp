#include "core/playlist.h"

#include <algorithm>

namespace pm {

void Playlist::Clear() {
    entries_.clear();
    order_.clear();
    current_ = -1;
    orderPos_ = 0;
}

void Playlist::Add(std::filesystem::path path, TrackMeta meta) {
    entries_.push_back({std::move(path), std::move(meta)});
    const int added = Size() - 1;
    if (current_ < 0) current_ = 0;
    if (shuffle_) {
        // Новый трек встаёт в случайное место ещё не сыгранной части порядка
        const int from = orderPos_ + 1;
        const int to = static_cast<int>(order_.size());
        const int at = from >= to ? to : std::uniform_int_distribution<int>(from, to)(rng_);
        order_.insert(order_.begin() + at, added);
    }
}

void Playlist::Remove(int index) {
    if (index < 0 || index >= Size()) return;
    entries_.erase(entries_.begin() + index);
    if (shuffle_) {
        const auto it = std::find(order_.begin(), order_.end(), index);
        if (it != order_.end()) {
            if (it - order_.begin() < orderPos_) --orderPos_;
            order_.erase(it);
        }
        for (int& i : order_)
            if (i > index) --i;
    }
    if (entries_.empty()) {
        current_ = -1;
        order_.clear();
        orderPos_ = 0;
        return;
    }
    if (index < current_) --current_;
    else if (index == current_ && current_ >= Size()) current_ = Size() - 1;
    if (shuffle_) orderPos_ = std::clamp(orderPos_, 0, static_cast<int>(order_.size()) - 1);
}

void Playlist::BuildOrder(int first, int avoidFirst) {
    order_.clear();
    for (int i = 0; i < Size(); ++i)
        if (i != first) order_.push_back(i);
    std::shuffle(order_.begin(), order_.end(), rng_);
    if (first >= 0) {
        order_.insert(order_.begin(), first);
    } else if (avoidFirst >= 0 && order_.size() > 1 && order_.front() == avoidFirst) {
        // Новый круг не начинается с только что сыгранного трека
        std::swap(order_[0], order_[1 + rng_() % (order_.size() - 1)]);
    }
    orderPos_ = 0;
}

void Playlist::SetShuffle(bool on) {
    if (on == shuffle_) return;
    shuffle_ = on;
    order_.clear();
    orderPos_ = 0;
    if (on && !entries_.empty()) BuildOrder(current_ < 0 ? 0 : current_, -1);
}

void Playlist::Select(int index) {
    if (index < 0 || index >= Size()) return;
    current_ = index;
    if (!shuffle_) return;
    // Уже сыгранное остаётся историей, выбранный трек идёт следующим, остальное перемешивается заново
    std::vector<int> visited(order_.begin(), order_.begin() + std::min<size_t>(order_.size(), orderPos_ + 1));
    visited.erase(std::remove(visited.begin(), visited.end(), index), visited.end());
    std::vector<int> rest;
    for (int i = 0; i < Size(); ++i)
        if (i != index && std::find(visited.begin(), visited.end(), i) == visited.end()) rest.push_back(i);
    std::shuffle(rest.begin(), rest.end(), rng_);
    order_ = visited;
    order_.push_back(index);
    orderPos_ = static_cast<int>(order_.size()) - 1;
    order_.insert(order_.end(), rest.begin(), rest.end());
}

int Playlist::Advance(bool automatic) {
    if (entries_.empty()) return -1;
    if (automatic && repeat_ == Repeat::One) return current_;
    if (shuffle_) {
        if (order_.size() != entries_.size()) BuildOrder(current_, -1);
        if (orderPos_ + 1 < static_cast<int>(order_.size())) {
            current_ = order_[static_cast<size_t>(++orderPos_)];
            return current_;
        }
        if (automatic && repeat_ == Repeat::Off) return -1;
        BuildOrder(-1, current_);
        current_ = order_[0];
        return current_;
    }
    if (current_ + 1 < Size()) return ++current_;
    if (automatic && repeat_ == Repeat::Off) return -1;
    current_ = 0;
    return current_;
}

int Playlist::PeekNext() const {
    if (entries_.empty() || repeat_ == Repeat::One || current_ < 0) return -1;
    if (shuffle_) {
        if (order_.size() != entries_.size()) return -1;
        if (orderPos_ + 1 < static_cast<int>(order_.size())) return order_[static_cast<size_t>(orderPos_ + 1)];
        return -1;
    }
    if (current_ + 1 < Size()) return current_ + 1;
    return repeat_ == Repeat::All ? 0 : -1;
}

void Playlist::PlayNext(int index) {
    if (index < 0 || index >= Size() || index == current_) return;
    if (shuffle_) {
        if (order_.size() != entries_.size()) BuildOrder(current_, -1);
        const auto it = std::find(order_.begin(), order_.end(), index);
        if (it != order_.end()) {
            if (it - order_.begin() < orderPos_) --orderPos_;
            order_.erase(it);
        }
        order_.insert(order_.begin() + std::min<int>(orderPos_ + 1, static_cast<int>(order_.size())), index);
        return;
    }
    // Без перемешивания порядок - это сам список: переносим запись
    PlaylistEntry e = std::move(entries_[static_cast<size_t>(index)]);
    entries_.erase(entries_.begin() + index);
    if (index < current_) --current_;
    entries_.insert(entries_.begin() + current_ + 1, std::move(e));
}

int Playlist::Back() {
    if (entries_.empty()) return -1;
    if (shuffle_) {
        if (orderPos_ > 0) current_ = order_[static_cast<size_t>(--orderPos_)];
        return current_;
    }
    current_ = current_ <= 0 ? Size() - 1 : current_ - 1;
    return current_;
}

}  // namespace pm
