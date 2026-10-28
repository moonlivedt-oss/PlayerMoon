#pragma once
// Графика окна: D3D11 -> DXGI swap chain для композиции -> Direct2D, показ через DirectComposition.
//
// Swap chain с попиксельной прозрачностью (premultiplied alpha) лежит поверх системного
// материала окна (Acrylic). Кадры синхронизированы с DWM через waitable-объект swap chain:
// окно ждёт его в MsgWaitForMultipleObjectsEx и рисует только когда DWM готов принять кадр.
// Когда рисовать нечего, на объект никто не смотрит - кадров нет.
//
// Владелец - поток UI. При потере устройства (смена драйвера, удалённый стол) всё
// пересоздаётся, а DeviceGeneration() растёт - кэши картинок по нему понимают, что устарели.

#include <windows.h>

#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite_3.h>
#include <dxgi1_3.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>

namespace ui {

using Microsoft::WRL::ComPtr;

class Renderer {
public:
    bool Init(HWND hwnd);
    void Shutdown();

    // Размер в физических пикселях
    bool Resize(UINT width, UINT height);
    void SetDpi(float dpi);
    float Dpi() const { return dpi_; }

    // Начало кадра: nullptr, если рисовать некуда (окно нулевого размера, устройство потеряно)
    ID2D1DeviceContext* BeginFrame();
    // Конец кадра и показ. false - устройство потеряно и пересоздано, кадр надо повторить.
    bool EndFrame();

    HANDLE FrameWaitable() const { return waitable_; }
    // true - кадр лежит поверх системного материала окна (DirectComposition, есть прозрачность)
    bool Composition() const { return composition_; }
    uint64_t DeviceGeneration() const { return generation_; }

    ID2D1Factory1* Factory() const { return factory_.Get(); }
    ID2D1DeviceContext* Context() const { return dc_.Get(); }
    IDWriteFactory* DWrite() const { return dwrite_.Get(); }
    IWICImagingFactory* Wic() const { return wic_.Get(); }
    ID2D1Device* Device() const { return d2dDevice_.Get(); }

private:
    bool CreateDevice();
    void ReleaseDevice();
    bool CreateTarget();
    DXGI_SWAP_CHAIN_DESC1 ChainDesc(bool alpha) const;
    bool CreateCompositionChain(IDXGIFactory2* dxgiFactory);
    bool CreateWindowChain(IDXGIFactory2* dxgiFactory);

    HWND hwnd_ = nullptr;
    UINT width_ = 1, height_ = 1;
    float dpi_ = 96;
    uint64_t generation_ = 0;
    bool composition_ = false;

    // Не зависят от устройства
    ComPtr<ID2D1Factory1> factory_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<IWICImagingFactory> wic_;

    // Зависят от устройства
    ComPtr<ID3D11Device> d3d_;
    ComPtr<IDXGIDevice> dxgiDevice_;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> dc_;
    ComPtr<ID2D1Bitmap1> target_;
    ComPtr<IDCompositionDevice> dcomp_;
    ComPtr<IDCompositionTarget> dcompTarget_;
    ComPtr<IDCompositionVisual> visual_;
    HANDLE waitable_ = nullptr;
};

}  // namespace ui
