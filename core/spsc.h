#pragma once
// Очередь «один писатель - один читатель» без блокировок.
// Используется между потоком UI и аудиопотоком: аудиопоток не должен ждать замков.
// Писатель трогает только tail_, читатель - только head_; данные публикуются release/acquire.

#include <array>
#include <atomic>
#include <cstddef>

namespace pm {

template <typename T, size_t Capacity>
class SpscQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "размер - степень двойки");

public:
    // Только писатель. false - очередь полна.
    bool Push(const T& item) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail - head_.load(std::memory_order_acquire) >= Capacity) return false;
        items_[tail & (Capacity - 1)] = item;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Только читатель. false - очередь пуста.
    bool Pop(T& out) {
        const size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) return false;
        out = items_[head & (Capacity - 1)];
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

private:
    std::array<T, Capacity> items_{};
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

}  // namespace pm
