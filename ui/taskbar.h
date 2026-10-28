#pragma once
// Кнопка окна на панели задач: «назад», «пуск/пауза», «вперёд» в превью окна и полоса
// прогресса трека на самой кнопке (ITaskbarList3). Иконки рисуются из тех же векторных
// путей, что и в окне, белым цветом в размер маленькой иконки системы.

#include <windows.h>

#include <d2d1_1.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace ui {

class Taskbar {
public:
    enum Button : UINT { kPrev = 4001, kPlayPause = 4002, kNext = 4003 };

    // Вызывать на сообщение RegisterWindowMessage("TaskbarButtonCreated")
    void Attach(HWND hwnd, ID2D1Factory1* factory, IWICImagingFactory* wic);
    void Detach();
    // Обновить значок «пуск/пауза» и прогресс. Вызовы без изменений ничего не делают.
    void Update(bool hasTrack, bool playing, double position, double duration);

private:
    HICON MakeIcon(const char* const* paths, int count, bool solid);
    void SetPlayIcon(bool playing);

    HWND hwnd_ = nullptr;
    Microsoft::WRL::ComPtr<ITaskbarList3> list_;
    Microsoft::WRL::ComPtr<ID2D1Factory1> factory_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;
    HICON prev_ = nullptr, next_ = nullptr, play_ = nullptr, pause_ = nullptr;
    int playShown_ = -1;      // какой значок сейчас у средней кнопки
    int progressState_ = -1;  // TBPFLAG
    ULONGLONG progressValue_ = ~0ull;
};

}  // namespace ui
