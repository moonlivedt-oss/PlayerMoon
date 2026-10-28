#include "ui/text.h"

#include <windows.h>

#include <cmath>
#include <cwchar>
#include <system_error>

namespace ui {

bool TextSystem::Init(IDWriteFactory* factory, const std::filesystem::path& fontsDir) {
    factory_ = factory;
    if (FAILED(factory_->GetSystemFontCollection(&system_))) return false;

    // Свои шрифты: все .ttf/.otf из assets/fonts в отдельную коллекцию (нужен IDWriteFactory5, Windows 10 1703+)
    ComPtr<IDWriteFactory5> f5;
    std::error_code ec;
    if (SUCCEEDED(factory_.As(&f5)) && std::filesystem::is_directory(fontsDir, ec)) {
        ComPtr<IDWriteFontSetBuilder1> builder;
        if (SUCCEEDED(f5->CreateFontSetBuilder(&builder))) {
            int added = 0;
            for (const auto& entry : std::filesystem::directory_iterator(fontsDir, ec)) {
                const std::wstring ext = entry.path().extension().wstring();
                if (_wcsicmp(ext.c_str(), L".ttf") != 0 && _wcsicmp(ext.c_str(), L".otf") != 0) continue;
                ComPtr<IDWriteFontFile> file;
                if (SUCCEEDED(f5->CreateFontFileReference(entry.path().c_str(), nullptr, &file)) &&
                    SUCCEEDED(builder->AddFontFile(file.Get())))
                    ++added;
            }
            ComPtr<IDWriteFontSet> set;
            ComPtr<IDWriteFontCollection1> coll;
            if (added > 0 && SUCCEEDED(builder->CreateFontSet(&set)) &&
                SUCCEEDED(f5->CreateFontCollectionFromFontSet(set.Get(), &coll)))
                custom_ = coll;
        }
    }
    SetFamilies(L"", L"");
    return true;
}

bool TextSystem::HasFamily(IDWriteFontCollection* c, const std::wstring& name) const {
    if (!c || name.empty()) return false;
    UINT32 index = 0;
    BOOL exists = FALSE;
    return SUCCEEDED(c->FindFamilyName(name.c_str(), &index, &exists)) && exists;
}

std::wstring TextSystem::PickFamily(const std::wstring& wanted, const wchar_t* fallback1, const wchar_t* fallback2) {
    if (HasFamily(custom_.Get(), wanted) || HasFamily(system_.Get(), wanted)) return wanted;
    if (HasFamily(system_.Get(), fallback1)) return fallback1;
    return fallback2;
}

void TextSystem::SetFamilies(const std::wstring& text, const std::wstring& display) {
    // Segoe UI Variable есть только в Windows 11; иначе - Segoe UI
    textFamily_ = PickFamily(text, L"Segoe UI Variable Text", L"Segoe UI");
    displayFamily_ = PickFamily(display.empty() ? text : display, L"Segoe UI Variable Display", L"Segoe UI");
    textCollection_ = HasFamily(custom_.Get(), textFamily_) ? custom_.Get() : nullptr;
    displayCollection_ = HasFamily(custom_.Get(), displayFamily_) ? custom_.Get() : nullptr;
    formats_.clear();
    layouts_.clear();
}

IDWriteTextFormat* TextSystem::Format(const TextSpec& spec) {
    wchar_t key[96];
    swprintf(key, 96, L"%.2f|%.0f|%d|%d", spec.size, spec.weight, spec.display ? 1 : 0, static_cast<int>(spec.align));
    auto it = formats_.find(key);
    if (it != formats_.end()) return it->second.Get();

    const std::wstring& family = spec.display ? displayFamily_ : textFamily_;
    IDWriteFontCollection* coll = spec.display ? displayCollection_ : textCollection_;
    // Статическим шрифтам - ближайшее начертание; переменным (Segoe UI Variable) - точное значение оси wght
    const auto weight = static_cast<DWRITE_FONT_WEIGHT>(std::lround(spec.weight));
    ComPtr<IDWriteTextFormat> format;
    if (FAILED(factory_->CreateTextFormat(family.c_str(), coll, weight, DWRITE_FONT_STYLE_NORMAL,
                                          DWRITE_FONT_STRETCH_NORMAL, spec.size, L"ru-ru", &format)))
        return nullptr;
    ComPtr<IDWriteTextFormat3> f3;
    if (SUCCEEDED(format.As(&f3))) {
        const DWRITE_FONT_AXIS_VALUE axis{DWRITE_FONT_AXIS_TAG_WEIGHT, spec.weight};
        f3->SetFontAxisValues(&axis, 1);
    }
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format->SetTextAlignment(spec.align);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    formats_[key] = format;
    return format.Get();
}

ComPtr<IDWriteTextLayout> TextSystem::Layout(const std::wstring& text, const TextSpec& spec, float width,
                                             float height) {
    wchar_t head[128];
    swprintf(head, 128, L"%.2f|%.0f|%d|%d|%d|%.2f|%d|%d|%.1f|%.1f|", spec.size, spec.weight, spec.display ? 1 : 0,
             static_cast<int>(spec.align), spec.upper ? 1 : 0, spec.spacing, spec.ellipsis ? 1 : 0,
             spec.tabular ? 1 : 0, width, height);
    const std::wstring key = head + text;
    auto it = layouts_.find(key);
    if (it != layouts_.end()) return it->second;
    if (layouts_.size() > 512) layouts_.clear();  // кэш не растёт бесконечно

    IDWriteTextFormat* format = Format(spec);
    if (!format) return nullptr;
    std::wstring shown = text;
    if (spec.upper && !shown.empty()) {
        // Заглавные с учётом языка (ё -> Ё)
        std::wstring up(shown.size() * 2 + 4, L'\0');
        const int n = LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_UPPERCASE, shown.c_str(),
                                    static_cast<int>(shown.size()), up.data(), static_cast<int>(up.size()), nullptr,
                                    nullptr, 0);
        if (n > 0) shown.assign(up.data(), static_cast<size_t>(n));
    }
    const float maxW = width > 0 ? width : 100000.f;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory_->CreateTextLayout(shown.c_str(), static_cast<UINT32>(shown.size()), format, maxW, height,
                                          &layout)))
        return nullptr;
    if (spec.spacing != 0) {
        ComPtr<IDWriteTextLayout1> l1;
        if (SUCCEEDED(layout.As(&l1)))
            l1->SetCharacterSpacing(0, spec.spacing, 0, {0, static_cast<UINT32>(shown.size())});
    }
    if (spec.tabular) {
        ComPtr<IDWriteTypography> typo;
        if (SUCCEEDED(factory_->CreateTypography(&typo))) {
            typo->AddFontFeature({DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1});
            layout->SetTypography(typo.Get(), {0, static_cast<UINT32>(shown.size())});
        }
    }
    if (spec.ellipsis && width > 0) {
        if (!ellipsis_) factory_->CreateEllipsisTrimmingSign(format, &ellipsis_);
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        layout->SetTrimming(&trimming, ellipsis_.Get());
    }
    layouts_[key] = layout;
    return layout;
}

float TextSystem::Measure(const std::wstring& text, const TextSpec& spec) {
    TextSpec s = spec;
    s.ellipsis = false;
    auto layout = Layout(text, s, 0, 100);
    if (!layout) return 0;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

}  // namespace ui
