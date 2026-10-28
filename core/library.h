#pragma once
// Музыкальная библиотека: папки, в которых плеер сам ищет музыку, и кэш найденных треков.
//
// Как приложение подстраивается под пользователя:
// - при первом запуске берёт стандартные папки (их передаёт окно: «Музыка», «Загрузки»...);
// - папки, которые пользователь открывает или перетаскивает, запоминаются (LearnFrom);
// - повторное сканирование читает теги только у новых и изменённых файлов (размер и время
//   изменения), поэтому быстрое; удалённые файлы пропадают сами;
// - сколько раз и когда слушался трек, тоже хранится (NotePlayed).
//
// Сканирование идёт в фоновом потоке (LibraryScanner); сама Library живёт в потоке UI.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/json.h"
#include "core/playlist.h"

namespace pm {

struct LibraryTrack {
    std::filesystem::path path;
    TrackMeta meta;
    int trackNo = 0;     // номер в альбоме, для порядка внутри папки
    uint64_t size = 0;   // размер и время изменения: менялся ли файл с прошлого сканирования
    int64_t mtime = 0;
    uint32_t plays = 0;  // сколько раз дослушан или выбран
    int64_t lastPlayed = 0;  // секунды Unix
};

class Library {
public:
    const std::vector<std::filesystem::path>& Folders() const { return folders_; }
    // Добавить папку. Вложенная в уже известную - не добавляется; охватывающая заменяет вложенные.
    // true - список папок изменился.
    bool AddFolder(const std::filesystem::path& dir);
    bool RemoveFolder(const std::filesystem::path& dir);
    // Запомнить папку, из которой пользователь открыл файл или папку
    bool LearnFrom(const std::filesystem::path& opened);
    bool Contains(const std::filesystem::path& file) const;

    bool AutoScan() const { return autoScan_; }
    void SetAutoScan(bool on) { autoScan_ = on; }

    const std::vector<LibraryTrack>& Tracks() const { return tracks_; }
    // Результат сканирования. Счётчики прослушиваний переносятся по пути файла.
    void SetTracks(std::vector<LibraryTrack> tracks);
    void NotePlayed(const std::filesystem::path& file, int64_t unixTime);
    // Записи для плейлиста в порядке библиотеки
    std::vector<PlaylistEntry> Entries() const;

    json::Value ToJson() const;
    void FromJson(const json::Value& v);
    bool Save(const std::filesystem::path& file) const;
    bool Load(const std::filesystem::path& file);

    // Обход папок. previous - прошлый результат: у неизменённых файлов теги не читаются заново.
    // cancel - флаг остановки (выход из программы). Результат отсортирован: папка, номер, имя.
    static std::vector<LibraryTrack> Scan(const std::vector<std::filesystem::path>& folders,
                                          const std::vector<LibraryTrack>& previous,
                                          const std::atomic<bool>* cancel = nullptr);

private:
    std::vector<std::filesystem::path> folders_;
    std::vector<LibraryTrack> tracks_;
    bool autoScan_ = true;
};

// Сканирование в фоновом потоке. done вызывается из фонового потока по окончании
// (окно отвечает на него сообщением и забирает результат в потоке UI через Take).
class LibraryScanner {
public:
    ~LibraryScanner();
    // false - сканирование уже идёт (новое начнётся по Rescan после окончания)
    bool Start(std::vector<std::filesystem::path> folders, std::vector<LibraryTrack> previous,
               std::function<void()> done);
    bool Running() const { return running_.load(); }
    // Результат последнего сканирования (только после done)
    bool Take(std::vector<LibraryTrack>& out);
    void Stop();

private:
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<bool> ready_{false};
    std::vector<LibraryTrack> result_;
};

}  // namespace pm
