#pragma once
// Замок для редких операций вне аудиопотока.
// На Windows - SRWLOCK: RadialMenu отказался от std::mutex из-за падений при статической
// сборке MinGW, повторяем это решение. На Linux (тесты) - std::mutex.
// В аудиопотоке замки не берутся вообще: там только атомарные операции.

#ifdef _WIN32
#include <windows.h>
#else
#include <mutex>
#endif

namespace pm {

class Lock {
public:
    Lock() = default;
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

#ifdef _WIN32
    void Acquire() { AcquireSRWLockExclusive(&lock_); }
    void Release() { ReleaseSRWLockExclusive(&lock_); }

private:
    SRWLOCK lock_ = SRWLOCK_INIT;
#else
    void Acquire() { mutex_.lock(); }
    void Release() { mutex_.unlock(); }

private:
    std::mutex mutex_;
#endif
};

class LockGuard {
public:
    explicit LockGuard(Lock& l) : lock_(l) { lock_.Acquire(); }
    ~LockGuard() { lock_.Release(); }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    Lock& lock_;
};

}  // namespace pm
