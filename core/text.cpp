#include "core/text.h"

namespace text {

void AppendUtf8(std::string& out, char32_t cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::vector<char32_t> CodePoints(std::string_view s) {
    std::vector<char32_t> out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int extra = 0;
        char32_t cp = 0;
        if (c < 0x80) { cp = c; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { out.push_back(0xFFFD); ++i; continue; }
        if (i + static_cast<size_t>(extra) >= s.size()) {  // последовательность обрезана концом строки
            out.push_back(0xFFFD);
            break;
        }
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + static_cast<size_t>(k)]);
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { out.push_back(0xFFFD); ++i; continue; }
        out.push_back(cp);
        i += static_cast<size_t>(extra) + 1;
    }
    return out;
}

std::wstring Utf8ToWide(std::string_view s) {
    std::wstring out;
    out.reserve(s.size());
    for (char32_t cp : CodePoints(s)) {
        if constexpr (sizeof(wchar_t) == 2) {
            if (cp >= 0x10000) {
                cp -= 0x10000;
                out += static_cast<wchar_t>(0xD800 + (cp >> 10));
                out += static_cast<wchar_t>(0xDC00 + (cp & 0x3FF));
                continue;
            }
        }
        out += static_cast<wchar_t>(cp);
    }
    return out;
}

std::string WideToUtf8(std::wstring_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char32_t cp = static_cast<char32_t>(s[i]);
        if constexpr (sizeof(wchar_t) == 2) {
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size()) {
                const char32_t lo = static_cast<char32_t>(s[i + 1]);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    ++i;
                }
            }
        }
        AppendUtf8(out, cp);
    }
    return out;
}

std::string Utf16ToUtf8(const uint8_t* data, size_t bytes, bool bigEndian) {
    std::string out;
    for (size_t i = 0; i + 1 < bytes; i += 2) {
        char32_t cp = bigEndian ? static_cast<char32_t>((data[i] << 8) | data[i + 1])
                                : static_cast<char32_t>(data[i] | (data[i + 1] << 8));
        if (cp == 0) break;
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < bytes) {
            const char32_t lo = bigEndian ? static_cast<char32_t>((data[i + 2] << 8) | data[i + 3])
                                          : static_cast<char32_t>(data[i + 2] | (data[i + 3] << 8));
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        AppendUtf8(out, cp);
    }
    return out;
}

std::string Latin1ToUtf8(const uint8_t* data, size_t bytes) {
    std::string out;
    for (size_t i = 0; i < bytes && data[i]; ++i) AppendUtf8(out, data[i]);
    return out;
}

std::string PathToUtf8(const std::filesystem::path& p) {
#ifdef _WIN32
    return WideToUtf8(p.native());
#else
    return p.native();
#endif
}

std::filesystem::path Utf8ToPath(std::string_view s) {
#ifdef _WIN32
    return std::filesystem::path(Utf8ToWide(s));
#else
    return std::filesystem::path(std::string(s));
#endif
}

std::string Trim(std::string s) {
    auto bad = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0'; };
    while (!s.empty() && bad(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && bad(s[i])) ++i;
    return s.substr(i);
}

std::string AsciiLower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

}  // namespace text
