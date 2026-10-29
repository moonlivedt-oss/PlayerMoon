#pragma once
// HTTPS GET через WinHTTP (синхронно, для фоновых потоков): тексты песен с lrclib.net.
// Прокси берётся системный (как в браузере), сертификаты проверяет Windows.

#include <cstddef>
#include <string>

namespace ui {

// Код ответа HTTP (200, 404...) или 0 - нет сети, таймаут, ответ больше maxBytes
int HttpsGet(const wchar_t* host, const std::string& pathAndQuery, std::string& body, size_t maxBytes = 2u << 20);

}  // namespace ui
