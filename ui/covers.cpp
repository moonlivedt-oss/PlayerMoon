#include "ui/covers.h"

#include <algorithm>

namespace ui {

bool CoverCache::Load(uint64_t serial, const std::vector<uint8_t>& bytes, const std::string& title,
                      theme::CoverPalette& palette) {
    palette = theme::MakeCoverPalette(title);
    if (!wic_ || bytes.empty() || bytes.size() > (32u << 20)) return false;

    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic_->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()))) ||
        FAILED(wic_->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(wic_->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)))
        return false;

    UINT w = 0, h = 0;
    converter->GetSize(&w, &h);
    if (!w || !h) return false;
    // Большие картинки уменьшаем сразу: память и время отрисовки
    ComPtr<IWICBitmapSource> source = converter;
    const UINT side = std::max(w, h);
    if (side > kMaxSide) {
        ComPtr<IWICBitmapScaler> scaler;
        const UINT nw = std::max<UINT>(1, w * kMaxSide / side), nh = std::max<UINT>(1, h * kMaxSide / side);
        if (SUCCEEDED(wic_->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(converter.Get(), nw, nh, WICBitmapInterpolationModeFant)))
            source = scaler;
    }
    Entry e;
    // Материализуем картинку: после этого байты из тегов больше не нужны
    if (FAILED(wic_->CreateBitmapFromSource(source.Get(), WICBitmapCacheOnLoad, &e.image))) return false;

    // Цвета: уменьшенная копия 16x16
    ComPtr<IWICBitmapScaler> tiny;
    if (SUCCEEDED(wic_->CreateBitmapScaler(&tiny)) &&
        SUCCEEDED(tiny->Initialize(e.image.Get(), 16, 16, WICBitmapInterpolationModeFant))) {
        std::vector<uint8_t> px(16 * 16 * 4);
        if (SUCCEEDED(tiny->CopyPixels(nullptr, 16 * 4, static_cast<UINT>(px.size()), px.data())))
            palette = theme::PaletteFromPixels(px.data(), 16 * 16, title);
    }
    entries_[serial] = std::move(e);
    return true;
}

ID2D1Bitmap1* CoverCache::Get(uint64_t serial, ID2D1DeviceContext* dc, uint64_t generation) {
    auto it = entries_.find(serial);
    if (it == entries_.end()) return nullptr;
    Entry& e = it->second;
    if (!e.bitmap || e.generation != generation) {
        e.bitmap.Reset();
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(dc->CreateBitmapFromWicBitmap(e.image.Get(), &props, &e.bitmap))) return nullptr;
        e.generation = generation;
    }
    return e.bitmap.Get();
}

void CoverCache::Keep(const std::vector<uint64_t>& serials) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (std::find(serials.begin(), serials.end(), it->first) == serials.end()) it = entries_.erase(it);
        else ++it;
    }
}

}  // namespace ui
