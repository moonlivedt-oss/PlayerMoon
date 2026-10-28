#pragma once
// Преобразования текста: UTF-8, UTF-16, Latin-1 и пути файлов.
// Внутри ядра и в JSON строки хранятся в UTF-8; окно Windows работает с UTF-16.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace text {

std::wstring Utf8ToWide(std::string_view s);
std::string WideToUtf8(std::wstring_view s);
// UTF-16 из байт (little или big endian) -> UTF-8
std::string Utf16ToUtf8(const uint8_t* data, size_t bytes, bool bigEndian);
// Latin-1 (ISO-8859-1, кодировка 0 в ID3) -> UTF-8
std::string Latin1ToUtf8(const uint8_t* data, size_t bytes);
// Кодовые точки строки UTF-8 (неверные байты заменяются на U+FFFD)
std::vector<char32_t> CodePoints(std::string_view s);
void AppendUtf8(std::string& out, char32_t cp);

// Путь <-> UTF-8 без потерь на любой ОС
std::string PathToUtf8(const std::filesystem::path& p);
std::filesystem::path Utf8ToPath(std::string_view s);

// Обрезка пробелов и нулевых байт по краям
std::string Trim(std::string s);
// Нижний регистр для ASCII (расширения файлов)
std::string AsciiLower(std::string s);

}  // namespace text
