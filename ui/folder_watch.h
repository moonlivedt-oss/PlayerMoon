#pragma once
// Наблюдение за папками библиотеки: новые, удалённые и изменённые файлы.
// Свой поток ждёт уведомлений Windows (FindFirstChangeNotification, с подпапками) и, когда
// изменения затихли на 3 секунды, шлёт окну одно сообщение - окно запускает пересканирование.
// В покое поток спит в WaitForMultipleObjects: процессор не тратится.

#include <windows.h>

#include <filesystem>
#include <thread>
#include <vector>

namespace ui {

class FolderWatcher {
public:
    ~FolderWatcher() { Stop(); }
    // Начать заново с новым списком папок (не больше 63). message уходит окну hwnd.
    void Start(const std::vector<std::filesystem::path>& folders, HWND hwnd, UINT message);
    void Stop();

private:
    std::thread thread_;
    HANDLE stop_ = nullptr;
};

}  // namespace ui
