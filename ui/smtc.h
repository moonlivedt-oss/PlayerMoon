#pragma once
// Системная медиапанель Windows (SMTC): название, исполнитель, альбом, обложка и позиция трека
// во всплывающей панели громкости, на экране блокировки и в «Быстрых настройках»; кнопки
// «пуск», «пауза», «стоп», «назад», «вперёд» и перемотка оттуда же. Через неё плеером
// управляют и другие программы (RadialMenu - через GlobalSystemMediaTransportControlsSessionManager).
//
// WinRT вызывается через ABI без C++/WinRT: интерфейсов SMTC нет в заголовках mingw-w64,
// они объявлены в smtc.cpp по метаданным Windows SDK. Нет WinRT (Windows 7, старый Wine) -
// модуль молча ничего не делает.
//
// События кнопок приходят в потоке пула WinRT; модуль только пересылает их окну сообщением.

#include <windows.h>

#include <unknwn.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ui {

class Smtc {
public:
    // Что прислать окну: wParam - Command, lParam - позиция в миллисекундах для kSeek
    enum Command : WPARAM { kPlay = 0, kPause = 1, kStop = 2, kNext = 6, kPrevious = 7, kSeek = 100 };

    Smtc() = default;
    ~Smtc() { Detach(); }
    Smtc(const Smtc&) = delete;
    Smtc& operator=(const Smtc&) = delete;

    // message - номер сообщения окну для команд
    void Attach(HWND hwnd, UINT message);
    void Detach();
    bool Active() const { return controls_ != nullptr; }

    // Новый трек (cover может быть пустым)
    void SetTrack(const std::string& title, const std::string& artist, const std::string& album, int trackNumber,
                  const std::vector<uint8_t>& cover);
    void ClearTrack();
    // Состояние и позиция; вызывать на каждом кадре и тике - без изменений ничего не отправляется
    void Update(bool hasTrack, bool playing, double position, double duration);

private:
    void PushTimeline(double position, double duration);

    HWND hwnd_ = nullptr;
    UINT message_ = 0;
    IUnknown* controls_ = nullptr;   // ISystemMediaTransportControls
    IUnknown* controls2_ = nullptr;  // ISystemMediaTransportControls2 (позиция и перемотка)
    IUnknown* buttonHandler_ = nullptr;
    IUnknown* seekHandler_ = nullptr;
    int64_t buttonToken_ = 0, seekToken_ = 0;
    int status_ = -1;  // MediaPlaybackStatus, отправленный в последний раз
    bool hasTrack_ = false;
    double duration_ = -1, position_ = 0;
    ULONGLONG positionAt_ = 0;  // когда отправлена позиция (мс)
};

}  // namespace ui
