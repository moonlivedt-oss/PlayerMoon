#include "ui/http.h"

#include <windows.h>

#include <winhttp.h>

#include "core/text.h"

namespace ui {

namespace {

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
};

}  // namespace

int HttpsGet(const wchar_t* host, const std::string& pathAndQuery, std::string& body, size_t maxBytes) {
    body.clear();
    Handle session, connect, request;
    // Автоматический прокси Windows 8.1+; старым системам хватит прокси по умолчанию
    session.h = WinHttpOpen(L"PlayerMoon/1.7", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h)
        session.h = WinHttpOpen(L"PlayerMoon/1.7", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) return 0;
    // Короткие таймауты: поток не должен висеть на выходе из программы
    WinHttpSetTimeouts(session.h, 4000, 4000, 4000, 6000);
    connect.h = WinHttpConnect(session.h, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect.h) return 0;
    const std::wstring path = ::text::Utf8ToWide(pathAndQuery);
    request.h = WinHttpOpenRequest(connect.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request.h) return 0;
    if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr))
        return 0;
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
        return 0;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request.h, &avail)) return 0;
        if (avail == 0) break;
        if (body.size() + avail > maxBytes) return 0;
        const size_t at = body.size();
        body.resize(at + avail);
        DWORD got = 0;
        if (!WinHttpReadData(request.h, body.data() + at, avail, &got)) return 0;
        body.resize(at + got);
    }
    return static_cast<int>(status);
}

}  // namespace ui
