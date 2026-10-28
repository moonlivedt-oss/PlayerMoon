#pragma once
// Картинки обложек: декодирование байтов из тегов через WIC, кэш битмапов Direct2D
// и цвета для фона окна (theme::PaletteFromPixels по уменьшенной копии).
// Декодированная картинка не зависит от устройства; битмап D2D создаётся заново после
// потери устройства (по номеру поколения Renderer).

#include <d2d1_1.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "theme/palette.h"

namespace ui {

using Microsoft::WRL::ComPtr;

class CoverCache {
public:
    void Init(IWICImagingFactory* wic) { wic_ = wic; }

    // Декодирует картинку (JPEG/PNG и всё, что умеет WIC) для трека serial.
    // palette получает цвета картинки. false - картинки нет или она битая (тогда заглушка).
    bool Load(uint64_t serial, const std::vector<uint8_t>& bytes, const std::string& title,
              theme::CoverPalette& palette);
    // Битмап для рисования; nullptr - картинки нет
    ID2D1Bitmap1* Get(uint64_t serial, ID2D1DeviceContext* dc, uint64_t generation);
    // Оставить только нужные (текущий и уходящий треки)
    void Keep(const std::vector<uint64_t>& serials);

private:
    struct Entry {
        ComPtr<IWICBitmap> image;     // 32bppPBGRA, сторона не больше kMaxSide
        ComPtr<ID2D1Bitmap1> bitmap;  // для текущего устройства
        uint64_t generation = 0;
    };
    static constexpr UINT kMaxSide = 640;  // хватает на обложку 292 DIP при 200%

    ComPtr<IWICImagingFactory> wic_;
    std::map<uint64_t, Entry> entries_;
};

}  // namespace ui
