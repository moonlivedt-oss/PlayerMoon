#include "ui/folder_watch.h"

namespace ui {

namespace {
constexpr DWORD kQuietMs = 3000;  // ждём, пока копирование альбома закончится
}

void FolderWatcher::Start(const std::vector<std::filesystem::path>& folders, HWND hwnd, UINT message) {
    Stop();
    std::vector<HANDLE> handles;
    for (const auto& f : folders) {
        if (handles.size() >= MAXIMUM_WAIT_OBJECTS - 1) break;
        HANDLE h = FindFirstChangeNotificationW(
            f.c_str(), TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                FILE_NOTIFY_CHANGE_LAST_WRITE);
        if (h != INVALID_HANDLE_VALUE) handles.push_back(h);
    }
    if (handles.empty()) return;
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_) {
        for (HANDLE h : handles) FindCloseChangeNotification(h);
        return;
    }
    thread_ = std::thread([handles, hwnd, message, stop = stop_] {
        std::vector<HANDLE> wait = handles;
        wait.push_back(stop);
        const DWORD n = static_cast<DWORD>(wait.size());
        bool pending = false;
        for (;;) {
            // Есть изменения - ждём тишины kQuietMs; нет - спим до события
            const DWORD r = WaitForMultipleObjects(n, wait.data(), FALSE, pending ? kQuietMs : INFINITE);
            if (r == WAIT_OBJECT_0 + n - 1 || r == WAIT_FAILED) break;  // стоп
            if (r == WAIT_TIMEOUT) {
                pending = false;
                PostMessageW(hwnd, message, 0, 0);
                continue;
            }
            if (r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + n - 1) {
                pending = true;
                FindNextChangeNotification(wait[r - WAIT_OBJECT_0]);
            }
        }
        for (HANDLE h : handles) FindCloseChangeNotification(h);
    });
}

void FolderWatcher::Stop() {
    if (stop_) SetEvent(stop_);
    if (thread_.joinable()) thread_.join();
    if (stop_) CloseHandle(stop_);
    stop_ = nullptr;
}

}  // namespace ui
