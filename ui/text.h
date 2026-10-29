#pragma once
// Текст через DirectWrite: форматы, раскладки строк с кэшем, измерение ширины.
// Шрифт по умолчанию - системный Segoe UI Variable (Text для мелкого текста, Display для
// названия), на Windows 10 без него - Segoe UI. Свои шрифты кладутся в assets/fonts и
// включаются ключом material.font.family / familyDisplay в теме.

#include <dwrite_3.h>
#include <wrl/client.h>

#include <filesystem>
#include <map>
#include <string>

namespace ui {

using Microsoft::WRL::ComPtr;

struct TextSpec {
    float size = 14;
    float weight = 400;
    bool display = false;  // крупное название - семейство Display
    bool upper = false;    // заглавными (text-transform: uppercase)
    float spacing = 0;     // разрядка между буквами, DIP (letter-spacing)
    DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING;
    bool ellipsis = true;  // обрезка многоточием по ширине
    bool tabular = false;  // цифры одной ширины (время не «прыгает»)
    bool wrap = false;     // перенос по словам (строки песни); высота - MeasureHeight
};

class TextSystem {
public:
    bool Init(IDWriteFactory* factory, const std::filesystem::path& fontsDir);
    // Семейства из темы (пусто - системные по умолчанию)
    void SetFamilies(const std::wstring& text, const std::wstring& display);

    // Раскладка строки. width <= 0 - без ограничения ширины (для бегущей строки).
    ComPtr<IDWriteTextLayout> Layout(const std::wstring& text, const TextSpec& spec, float width, float height);
    float Measure(const std::wstring& text, const TextSpec& spec);
    // Высота текста с переносом по ширине width
    float MeasureHeight(const std::wstring& text, const TextSpec& spec, float width);

private:
    IDWriteTextFormat* Format(const TextSpec& spec);
    std::wstring PickFamily(const std::wstring& wanted, const wchar_t* fallback1, const wchar_t* fallback2);
    bool HasFamily(IDWriteFontCollection* c, const std::wstring& name) const;

    ComPtr<IDWriteFactory> factory_;
    ComPtr<IDWriteFontCollection> custom_;  // шрифты из assets/fonts (если есть)
    ComPtr<IDWriteFontCollection> system_;
    std::wstring textFamily_, displayFamily_;
    IDWriteFontCollection* textCollection_ = nullptr;
    IDWriteFontCollection* displayCollection_ = nullptr;
    std::map<std::wstring, ComPtr<IDWriteTextFormat>> formats_;
    std::map<std::wstring, ComPtr<IDWriteTextLayout>> layouts_;
    ComPtr<IDWriteInlineObject> ellipsis_;
};

}  // namespace ui
