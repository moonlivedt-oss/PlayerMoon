#include "ui/taskbar.h"

#include <d2d1_1helper.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>

#include "ui/icons.h"

namespace ui {

namespace {

// Пути в сетке 24x24 (как в окне); play и pause - сплошные
const char* const kPrevPaths[] = {"M17.4 6.3 9.6 11.4a.7.7 0 0 0 0 1.2l7.8 5.1a.7.7 0 0 0 1.1-.6V6.9a.7.7 0 0 0-1.1-.6Z",
                                  "M5.5 6h2.2v12H5.5Z"};
const char* const kNextPaths[] = {"M6.6 6.3l7.8 5.1a.7.7 0 0 1 0 1.2l-7.8 5.1a.7.7 0 0 1-1.1-.6V6.9a.7.7 0 0 1 1.1-.6Z",
                                  "M16.3 6h2.2v12h-2.2Z"};
const char* const kPlayPaths[] = {"M8 5.2 19 12 8 18.8Z"};
const char* const kPausePaths[] = {"M6.5 5h3.6v14H6.5Z", "M13.9 5h3.6v14h-3.6Z"};

}  // namespace

void Taskbar::Attach(HWND hwnd, ID2D1Factory1* factory, IWICImagingFactory* wic) {
    Detach();
    hwnd_ = hwnd;
    factory_ = factory;
    wic_ = wic;
    if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&list_))) ||
        FAILED(list_->HrInit())) {
        list_.Reset();
        return;
    }
    prev_ = MakeIcon(kPrevPaths, 2, true);
    next_ = MakeIcon(kNextPaths, 2, true);
    play_ = MakeIcon(kPlayPaths, 1, true);
    pause_ = MakeIcon(kPausePaths, 2, true);
    THUMBBUTTON b[3] = {};
    const UINT ids[3] = {kPrev, kPlayPause, kNext};
    HICON icons[3] = {prev_, play_, next_};
    const wchar_t* tips[3] = {L"Назад", L"Пуск", L"Вперёд"};
    for (int i = 0; i < 3; ++i) {
        b[i].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
        b[i].iId = ids[i];
        b[i].hIcon = icons[i];
        b[i].dwFlags = THBF_ENABLED;
        wcsncpy_s(b[i].szTip, tips[i], _TRUNCATE);
    }
    list_->ThumbBarAddButtons(hwnd_, 3, b);
    playShown_ = 0;
    progressState_ = -1;
    progressValue_ = ~0ull;
}

void Taskbar::Detach() {
    list_.Reset();
    for (HICON* h : {&prev_, &next_, &play_, &pause_}) {
        if (*h) DestroyIcon(*h);
        *h = nullptr;
    }
}

void Taskbar::SetPlayIcon(bool playing) {
    if (!list_ || playShown_ == (playing ? 1 : 0)) return;
    THUMBBUTTON b{};
    b.dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
    b.iId = kPlayPause;
    b.hIcon = playing ? pause_ : play_;
    b.dwFlags = THBF_ENABLED;
    wcsncpy_s(b.szTip, playing ? L"Пауза" : L"Пуск", _TRUNCATE);
    if (SUCCEEDED(list_->ThumbBarUpdateButtons(hwnd_, 1, &b))) playShown_ = playing ? 1 : 0;
}

void Taskbar::Update(bool hasTrack, bool playing, double position, double duration) {
    if (!list_) return;
    SetPlayIcon(playing);
    // Прогресс: зелёный при игре, жёлтый на паузе; нет трека - без полосы
    const int state = !hasTrack || duration <= 0 ? TBPF_NOPROGRESS : playing ? TBPF_NORMAL : TBPF_PAUSED;
    if (state != progressState_) {
        list_->SetProgressState(hwnd_, static_cast<TBPFLAG>(state));
        progressState_ = state;
        progressValue_ = ~0ull;
    }
    if (state == TBPF_NOPROGRESS) return;
    // Тысяча шагов: значение меняется не чаще, чем видно глазу
    const ULONGLONG value = static_cast<ULONGLONG>(std::clamp(position / duration, 0.0, 1.0) * 1000);
    if (value != progressValue_) {
        list_->SetProgressValue(hwnd_, value, 1000);
        progressValue_ = value;
    }
}

HICON Taskbar::MakeIcon(const char* const* paths, int count, bool solid) {
    if (!factory_ || !wic_) return nullptr;
    const int size = GetSystemMetrics(SM_CXSMICON);  // 16 при 100%, 24 при 150%
    Microsoft::WRL::ComPtr<IWICBitmap> bmp;
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> rt;
    if (FAILED(wic_->CreateBitmap(size, size, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp))) return nullptr;
    const auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96, 96);
    if (FAILED(factory_->CreateWicBitmapRenderTarget(bmp.Get(), props, &rt))) return nullptr;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> white;
    rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &white);
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    const float k = static_cast<float>(size) / 24.f;
    rt->SetTransform(D2D1::Matrix3x2F::Scale(k, k));
    for (int i = 0; i < count; ++i) {
        auto geo = ParseSvgPath(factory_.Get(), paths[i]);
        if (!geo) continue;
        if (solid) rt->FillGeometry(geo.Get(), white.Get());
        rt->DrawGeometry(geo.Get(), white.Get(), 1.2f);
    }
    if (FAILED(rt->EndDraw())) return nullptr;

    // WIC (premultiplied) -> DIB с обычной альфой -> HICON
    std::vector<uint8_t> px(static_cast<size_t>(size) * size * 4);
    if (FAILED(bmp->CopyPixels(nullptr, size * 4, static_cast<UINT>(px.size()), px.data()))) return nullptr;
    for (size_t i = 0; i < px.size(); i += 4) {
        const uint8_t a = px[i + 3];
        if (a == 0) continue;
        for (int c = 0; c < 3; ++c) px[i + c] = static_cast<uint8_t>(std::min(255, px[i + c] * 255 / a));
    }
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;  // сверху вниз
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) return nullptr;
    std::memcpy(bits, px.data(), px.size());
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

}  // namespace ui
