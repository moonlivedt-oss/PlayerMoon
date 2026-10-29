#include "ui/app.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwchar>

#include "core/json.h"
#include "core/state.h"
#include "core/text.h"
#include "theme/assets.h"
#include "theme/palette.h"

namespace ui {

namespace {

constexpr wchar_t kClassName[] = L"PlayerMoonWindow";
constexpr wchar_t kInstanceMutex[] = L"Local\\PlayerMoon.Instance";
constexpr UINT kWakeMessage = WM_APP + 1;     // аудиопоток: есть события для PumpEvents
constexpr UINT kLibraryMessage = WM_APP + 2;  // сканер библиотеки закончил
constexpr UINT kWatchMessage = WM_APP + 3;    // в папках библиотеки что-то поменялось
constexpr UINT kPluginMessage = WM_APP + 4;   // плагин передал работу в поток окна (host->post)
constexpr UINT kSmtcMessage = WM_APP + 5;     // кнопка или перемотка из системной медиапанели
constexpr ULONG_PTR kCopyDataPaths = 0x504D;  // WM_COPYDATA: пути от второго экземпляра
constexpr UINT_PTR kTickTimer = 1;            // раз в секунду: время в мини-режиме и на панели задач
constexpr UINT_PTR kSaveTimer = 2;            // отложенное сохранение после изменений
constexpr UINT_PTR kSaveTick = 3;             // во время игры - позиция раз в 30 секунд
constexpr UINT_PTR kLazyTimer = 4;            // подсказки и уведомления: разовый кадр к нужному времени

// Атрибуты DWM числами: в заголовках MinGW имён может не быть (как в RadialMenu)
constexpr DWORD kDwmDarkMode = 20;
constexpr DWORD kDwmCornerPreference = 33;
constexpr DWORD kDwmSystemBackdrop = 38;
constexpr int kBackdropNone = 1;
constexpr int kBackdropTransient = 3;  // акрил
constexpr int kCornerRound = 2;
constexpr int kCornerRoundSmall = 3;

// Пункты меню
enum MenuId : UINT {
    kMenuOpenFiles = 1,
    kMenuOpenFolder,
    kMenuImportTheme,
    kMenuExportTheme,
    kMenuMini,
    kMenuPlaylist,
    kMenuExit,
    kMenuNormalize,
    kMenuMiniTop,
    kMenuLibraryAll,
    kMenuLibraryRescan,
    kMenuLibraryAdd,
    kMenuLibraryAuto,
    kMenuPlugins,
    kMenuRowPlay,
    kMenuRowNext,
    kMenuRowShow,
    kMenuRowRemove,
    kMenuThemeFirst = 100,          // встроенные темы по порядку
    kMenuLibraryFolderFirst = 200,  // папки библиотеки (убрать)
    kMenuPluginItemFirst = 1000,    // пункты, добавленные плагинами
};

// Глобальные медиаклавиши (RegisterHotKey)
enum HotKeyId : int { kHotPlay = 1, kHotNext, kHotPrev, kHotStop };

// Номер сборки Windows: RtlGetVersion не врёт про версию, в отличие от GetVersionEx
DWORD WindowsBuild() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return 0;
    auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
    if (!fn) return 0;
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    return fn(&info) == 0 ? info.dwBuildNumber : 0;
}

std::wstring FormatDuration(double seconds) {
    if (!(seconds > 0)) return L"";
    const long t = static_cast<long>(std::lround(seconds));
    wchar_t buf[32];
    if (t >= 3600) swprintf(buf, 32, L"%ld:%02ld:%02ld", t / 3600, (t / 60) % 60, t % 60);
    else swprintf(buf, 32, L"%ld:%02ld", t / 60, t % 60);
    return buf;
}

// «1 трек», «3 трека», «12 треков»
const wchar_t* TracksWord(int n) {
    const int n10 = n % 10, n100 = n % 100;
    if (n10 == 1 && n100 != 11) return L"трек";
    if (n10 >= 2 && n10 <= 4 && (n100 < 12 || n100 > 14)) return L"трека";
    return L"треков";
}

pm::Repeat RepeatFromString(const std::string& s) {
    if (s == "all") return pm::Repeat::All;
    if (s == "one") return pm::Repeat::One;
    return pm::Repeat::Off;
}

const char* RepeatToString(pm::Repeat r) {
    switch (r) {
        case pm::Repeat::All: return "all";
        case pm::Repeat::One: return "one";
        default: return "off";
    }
}

// Диалог выбора файлов или папки. Возвращает пустой список при отмене.
std::vector<std::filesystem::path> PickPaths(HWND owner, bool folder, bool multi, const wchar_t* filterName,
                                             const wchar_t* filterSpec, const wchar_t* title = nullptr) {
    std::vector<std::filesystem::path> out;
    Microsoft::WRL::ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return out;
    if (title) dlg->SetTitle(title);
    FILEOPENDIALOGOPTIONS opts = 0;
    dlg->GetOptions(&opts);
    opts |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    if (folder) opts |= FOS_PICKFOLDERS;
    else opts |= FOS_FILEMUSTEXIST;
    if (multi) opts |= FOS_ALLOWMULTISELECT;
    dlg->SetOptions(opts);
    if (!folder && filterSpec) {
        const COMDLG_FILTERSPEC spec[] = {{filterName, filterSpec}, {L"Все файлы", L"*.*"}};
        dlg->SetFileTypes(2, spec);
    }
    if (FAILED(dlg->Show(owner))) return out;
    Microsoft::WRL::ComPtr<IShellItemArray> items;
    if (FAILED(dlg->GetResults(&items))) return out;
    DWORD count = 0;
    items->GetCount(&count);
    for (DWORD i = 0; i < count; ++i) {
        Microsoft::WRL::ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            out.emplace_back(path);
            CoTaskMemFree(path);
        }
    }
    return out;
}

// Текст из буфера обмена в UTF-8 (пусто - нет текста)
std::string ClipboardText(HWND owner) {
    std::string out;
    if (!OpenClipboard(owner)) return out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* w = static_cast<const wchar_t*>(GlobalLock(h))) {
            out = ::text::WideToUtf8(w);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

}  // namespace

// ---------------- запуск ----------------

int App::Run(HINSTANCE instance, int show) {
    // Один экземпляр: повторный запуск («Открыть с помощью», двойной клик по файлу) отдаёт
    // файлы уже открытому окну. Мьютекс живёт до выхода из процесса.
    CreateMutexW(nullptr, FALSE, kInstanceMutex);
    if (GetLastError() == ERROR_ALREADY_EXISTS && ForwardToRunningInstance()) return 0;

    QueryPerformanceFrequency(&freq_);
    ResolvePaths();
    std::vector<std::string> log;
    themes_.Load(assetsDir_, &log);
    win11Backdrop_ = WindowsBuild() >= 22621;
    ReadSystemReduce();

    if (!CreateMainWindow(instance)) return 1;

    std::string error;
    HWND hwnd = hwnd_;
    std::atomic<bool>* posted = &wakePosted_;
    // Аудиопоток будит окно; флаг не даёт засыпать очередь одинаковыми сообщениями
    auto wake = [hwnd, posted] {
        if (!posted->exchange(true)) PostMessageW(hwnd, kWakeMessage, 0, 0);
    };
    // Нет устройства вывода - плеер всё равно работает (без звука), чтобы не терять плейлист
    bool silent = false;
    if (!player_.Init({}, wake, &error)) {
        pm::Player::Options opts;
        opts.nullBackend = true;
        silent = player_.Init(opts, wake);
    }
    player_.SetOnTrackChanged([this](int index, int direction) { OnTrackChanged(index, direction); });
    pcm_.resize(analyzer_.FftSize());

    view_.SetHost(this);
    LoadSavedState();

    // Файлы из командной строки («Открыть с помощью»)
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        std::vector<std::filesystem::path> paths;
        for (int i = 1; i < argc; ++i) paths.emplace_back(argv[i]);
        LocalFree(argv);
        if (!paths.empty()) LoadPaths(paths);
    }
    started_ = true;
    lastCopyData_ = Now();  // файлы, пришедшие следом от других экземпляров, - дописываются

    ShowWindow(hwnd_, show);
    RegisterMediaKeys();
    smtc_.Attach(hwnd_, kSmtcMessage);
    if (const pm::NowPlaying& np = player_.Current(); np.index >= 0)  // трек из state.json - сразу в панель
        smtc_.SetTrack(np.title, np.tags.artist, np.tags.album, np.tags.track, np.tags.cover);
    if (silent) {
        const std::wstring msg = L"Не удалось открыть звуковое устройство, звук выключен.\n" + ::text::Utf8ToWide(error);
        MessageBoxW(hwnd_, msg.c_str(), L"PlayerMoon", MB_ICONWARNING);
    }
    MessageLoop();

    watcher_.Stop();
    scanner_.Stop();
    plugins_.UnloadAll();
    taskbar_.Detach();
    smtc_.Detach();
    player_.Shutdown();
    renderer_.Shutdown();
    return 0;
}

void App::ResolvePaths() {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    exeDir_ = std::filesystem::path(std::wstring(buf, n)).parent_path();
    assetsDir_ = exeDir_ / L"assets";

    // state.json рядом с exe, если туда можно писать (переносная папка); иначе %APPDATA%\PlayerMoon
    const std::filesystem::path probe = exeDir_ / L".write-test";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        statePath_ = exeDir_ / L"state.json";
        return;
    }
    PWSTR appData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        std::filesystem::path dir = std::filesystem::path(appData) / L"PlayerMoon";
        CoTaskMemFree(appData);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        statePath_ = dir / L"state.json";
    } else {
        statePath_ = exeDir_ / L"state.json";
    }
}

bool App::CreateMainWindow(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = &App::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    if (!RegisterClassExW(&wc)) return false;
    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarButtonCreated");

    // WS_CAPTION нужен для системной тени и скруглённых углов; саму рамку убирает WM_NCCALCSIZE
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_APPWINDOW | WS_EX_ACCEPTFILES, kClassName,
                            L"PlayerMoon", style, CW_USEDEFAULT, CW_USEDEFAULT, 420, 680, nullptr, nullptr, instance,
                            this);
    if (!hwnd_) return false;
    dpi_ = static_cast<float>(GetDpiForWindow(hwnd_));
    // Окно с повышенными правами иначе не получит сообщение о кнопке на панели задач
    ChangeWindowMessageFilterEx(hwnd_, taskbarCreatedMsg_, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd_, WM_COPYDATA, MSGFLT_ALLOW, nullptr);

    const MARGINS margins{-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(hwnd_, &margins);
    // Пересчитать рамку после того, как WM_NCCALCSIZE начал возвращать 0
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    if (!renderer_.Init(hwnd_)) {
        MessageBoxW(hwnd_, L"Не удалось запустить Direct3D 11 / Direct2D.", L"PlayerMoon", MB_ICONERROR);
        return false;
    }
    renderer_.SetDpi(dpi_);
    text_.Init(renderer_.DWrite(), assetsDir_ / L"fonts");
    icons_.Init(renderer_.Factory());
    covers_.Init(renderer_.Wic());
    painter_.Init(&renderer_, &text_, &icons_, &covers_);
    return true;
}

// ---------------- состояние ----------------

void App::LoadSavedState() {
    pm::AppState st;
    // Нет файла - первый запуск: значения по умолчанию и тема под оформление Windows
    const bool firstRun = !pm::LoadState(statePath_, st);
    if (firstRun) st.themeId = SystemPrefersLight() ? "frost" : "glass";

    // Тема: своя (документ целиком) или встроенная по id
    theme::Theme t;
    std::string err;
    userTheme_ = !st.userTheme.IsNull() && themes_.FromDocument(st.userTheme, t, &err);
    if (!userTheme_) t = themes_.Make(st.themeId);
    view_.showRemaining = st.showRemaining;
    miniOnTop_ = st.miniOnTop;
    playlistFromLibrary_ = st.playlistFromLibrary;
    player_.SetNormalize(st.normalize);

    view_.RestoreModes(st.mini, st.playlistOpen);
    ApplyTheme(t, false);

    // Окно на прежнее место, если тот монитор ещё подключён
    if (st.hasWindowPos) {
        const POINT pt{st.windowX + 40, st.windowY + 20};
        if (MonitorFromPoint(pt, MONITOR_DEFAULTTONULL))
            SetWindowPos(hwnd_, nullptr, st.windowX, st.windowY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    player_.SetVolume(st.volume);
    player_.SetMuted(st.muted);
    player_.SetRepeat(RepeatFromString(st.repeat));
    player_.SetShuffle(st.shuffle);
    if (!st.playlist.empty()) {
        player_.LoadFiles(st.playlist);
        RebuildRows();
        const int count = player_.GetPlaylist().Size();
        const int index = std::clamp(st.index, 0, std::max(0, count - 1));
        if (count > 0) player_.OpenAt(index, st.position, false);
    }
    InitPlugins(st.plugins);
    InitLibrary(firstRun);
    if (!player_.GetState().hasTrack) OnTrackChanged(-1, 0);
    RebuildRows();
}

void App::SaveCurrentState() {
    pm::AppState st;
    const pm::Playlist& pl = player_.GetPlaylist();
    for (int i = 0; i < pl.Size(); ++i) st.playlist.push_back(pl.At(i).path);
    const pm::PlayerState ps = player_.GetState();
    st.index = ps.index;
    st.position = ps.position;
    st.volume = ps.volume;
    st.muted = ps.muted;
    st.repeat = RepeatToString(ps.repeat);
    st.shuffle = ps.shuffle;
    st.themeId = theme_.id;
    if (userTheme_) st.userTheme = themes_.ToUserDocument(theme_, theme_.name);
    st.normalize = player_.Normalize();
    st.showRemaining = view_.showRemaining;
    st.miniOnTop = miniOnTop_;
    st.playlistFromLibrary = playlistFromLibrary_;
    st.plugins = plugins_.EnabledIds();
    st.mini = view_.mini;
    st.playlistOpen = view_.playlistOpen;
    RECT rc{};
    if (GetWindowRect(hwnd_, &rc) && !IsIconic(hwnd_)) {
        st.hasWindowPos = true;
        st.windowX = rc.left;
        st.windowY = rc.top;
    }
    pm::SaveState(statePath_, st);
}

void App::MarkDirty() {
    // Повторный вызов переставляет таймер: серия изменений даёт одну запись
    if (hwnd_) SetTimer(hwnd_, kSaveTimer, 2000, nullptr);
}

// ---------------- тема ----------------

void App::ReadSystemReduce() {
    BOOL anim = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0);
    systemReduce_ = !anim;
}

bool App::SystemPrefersLight() {
    DWORD value = 0, size = sizeof(value);
    const LSTATUS r = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                                   L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return r == ERROR_SUCCESS && value != 0;
}

void App::ApplyTopmost() {
    // Мини-плеер поверх окон (настройка), полный режим - обычное окно
    const bool want = view_.mini && miniOnTop_;
    if (want == topmostApplied_) return;
    SetWindowPos(hwnd_, want ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    topmostApplied_ = want;
}

void App::SnapToEdges(RECT* r) const {
    // Прилипание к краям рабочей области монитора при перетаскивании (ближе 16 DIP)
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromRect(r, MONITOR_DEFAULTTONEAREST), &mi)) return;
    const RECT& w = mi.rcWork;
    const LONG th = ToPx(16), width = r->right - r->left, height = r->bottom - r->top;
    if (std::abs(r->left - w.left) < th) r->left = w.left;
    else if (std::abs(r->right - w.right) < th) r->left = w.right - width;
    if (std::abs(r->top - w.top) < th) r->top = w.top;
    else if (std::abs(r->bottom - w.bottom) < th) r->top = w.bottom - height;
    r->right = r->left + width;
    r->bottom = r->top + height;
}

void App::ApplyTheme(const theme::Theme& t, bool animate) {
    theme_ = t;
    text_.SetFamilies(t.material.font.family, t.material.font.familyDisplay);
    // Геометрия - той раскладки, что реально будет (на этапе 1 у всех тем «Классика»)
    const std::string layoutId(view::MakeLayout(t.settings.layout)->Id());
    view_.ApplyTheme(t, themes_.Layout(layoutId), systemReduce_, Now(), animate && started_);
    ApplyBackdrop();
    Invalidate();
}

void App::ApplyBackdrop() {
    const theme::Settings& s = theme_.settings;
    acrylicOn_ = win11Backdrop_ && s.backdrop == "acrylic" && renderer_.Composition();
    const int backdrop = acrylicOn_ ? kBackdropTransient : kBackdropNone;
    if (win11Backdrop_ && FAILED(DwmSetWindowAttribute(hwnd_, kDwmSystemBackdrop, &backdrop, sizeof(backdrop))))
        acrylicOn_ = false;
    // Тёмный режим рамки и акрила по цвету текста темы (светлый текст - тёмная тема)
    const BOOL dark = view_.DarkUi() ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, kDwmDarkMode, &dark, sizeof(dark));
    // Углы окна системные (этап 1): маленький радиус темы - малое скругление
    const int corner = s.radius < 10 ? kCornerRoundSmall : kCornerRound;
    DwmSetWindowAttribute(hwnd_, kDwmCornerPreference, &corner, sizeof(corner));
}

// ---------------- трек и плейлист ----------------

void App::OnTrackChanged(int index, int direction) {
    view::TrackText t;
    const pm::NowPlaying& np = player_.Current();
    if (index >= 0) {
        t.title = np.title;
        t.artist = np.tags.artist;
        t.album = np.tags.album;
        t.serial = np.serial;
        t.hasImage = covers_.Load(np.serial, np.tags.cover, np.title, t.palette);
    } else {
        // Пустой плейлист: подсказка вместо названия
        t.title = "PlayerMoon";
        t.artist = "Нет треков";
        t.album = "Правый клик - открыть файлы или папку";
        t.palette = theme::MakeCoverPalette(t.title);
    }
    view_.OnTrackChanged(t, direction, Now(), started_);
    std::vector<uint64_t> keep;
    for (const view::CoverSlot& c : view_.covers) keep.push_back(c.serial);
    covers_.Keep(keep);
    RebuildRows();  // длительность трека уточнилась после открытия декодера
    if (index >= 0) smtc_.SetTrack(np.title, np.tags.artist, np.tags.album, np.tags.track, np.tags.cover);
    else smtc_.ClearTrack();
    if (index >= 0 && started_) library_.NotePlayed(np.path, static_cast<int64_t>(time(nullptr)));
    NotifyPluginsTrack();
    MarkDirty();
    Invalidate();
}

void App::RebuildRows() {
    const pm::Playlist& pl = player_.GetPlaylist();
    rows_.clear();
    rows_.reserve(static_cast<size_t>(pl.Size()));
    double total = 0;
    for (int i = 0; i < pl.Size(); ++i) {
        const pm::PlaylistEntry& e = pl.At(i);
        PlaylistRow row;
        row.title = e.meta.title.empty() ? e.path.stem().wstring() : ::text::Utf8ToWide(e.meta.title);
        row.artist = ::text::Utf8ToWide(e.meta.artist);
        row.duration = FormatDuration(e.meta.duration);
        total += e.meta.duration;
        rows_.push_back(std::move(row));
    }
    wchar_t buf[64];
    swprintf(buf, 64, L"%d %ls, %ld мин", pl.Size(), TracksWord(pl.Size()), std::lround(total / 60));
    countText_ = pl.Size() > 0 ? buf : L"";
    view_.SetPlaylistSize(pl.Size());
}

void App::LoadPaths(const std::vector<std::filesystem::path>& paths, bool append) {
    // Одна папка - сканируется целиком; иначе берём файлы и папки по очереди
    std::vector<std::filesystem::path> files;
    bool replaced = append;
    const int before = player_.GetPlaylist().Size();
    playlistFromLibrary_ = false;
    LearnFolders(paths);
    for (const auto& p : paths) {
        std::error_code ec;
        if (std::filesystem::is_directory(p, ec)) {
            player_.LoadFolder(p, replaced);
            replaced = true;
        } else {
            files.push_back(p);
        }
    }
    if (!files.empty()) player_.LoadFiles(files, replaced);
    RebuildRows();
    if (append && player_.GetPlaylist().Size() > before)
        view_.ShowToast("Добавлено треков: " + std::to_string(player_.GetPlaylist().Size() - before), Now());
    // Дописанные треки не прерывают текущий; новый плейлист начинается сразу
    if (!append && player_.GetPlaylist().Size() > 0) player_.PlayIndex(0);
    else if (append && !player_.GetState().hasTrack && player_.GetPlaylist().Size() > before) player_.PlayIndex(before);
    MarkDirty();
    Invalidate();
}

void App::OpenFiles(bool folder) {
    auto paths = PickPaths(hwnd_, folder, !folder, L"Аудио (MP3, FLAC, WAV, M4A, AAC, OGG, Opus)",
                           L"*.mp3;*.flac;*.wav;*.m4a;*.m4b;*.aac;*.ogg;*.oga;*.opus");
    if (!paths.empty()) LoadPaths(paths);
}

void App::LoadDropped(HDROP drop) {
    std::vector<std::filesystem::path> paths;
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        const UINT len = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring s(len + 1, L'\0');
        DragQueryFileW(drop, i, s.data(), len + 1);
        s.resize(len);
        paths.emplace_back(s);
    }
    DragFinish(drop);
    if (!paths.empty()) LoadPaths(paths);
}

// ---------------- темы: импорт и экспорт ----------------

void App::ImportTheme() {
    auto paths = PickPaths(hwnd_, false, false, L"Тема PlayerMoon (*.json)", L"*.json");
    if (paths.empty()) return;
    std::string data, err;
    json::Value doc;
    theme::Theme t;
    if (!theme::ReadFile(paths[0], data) || !json::Parse(data, doc, &err) || !themes_.FromDocument(doc, t, &err)) {
        const std::wstring msg = L"Тема не подходит.\n" + ::text::Utf8ToWide(err);
        MessageBoxW(hwnd_, msg.c_str(), L"Импорт темы", MB_ICONWARNING);
        return;
    }
    userTheme_ = !t.builtin;
    ApplyTheme(t, true);
}

void App::ExportTheme() {
    Microsoft::WRL::ComPtr<IFileSaveDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    const COMDLG_FILTERSPEC spec[] = {{L"Тема PlayerMoon (*.json)", L"*.json"}};
    dlg->SetFileTypes(1, spec);
    dlg->SetDefaultExtension(L"json");
    const std::wstring name = ::text::Utf8ToWide(theme_.name.empty() ? std::string("Моя тема") : theme_.name);
    dlg->SetFileName(name.c_str());
    if (FAILED(dlg->Show(hwnd_))) return;
    Microsoft::WRL::ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return;
    const std::filesystem::path file(path);
    CoTaskMemFree(path);
    const json::Value doc = themes_.ToUserDocument(theme_, theme_.name.empty() ? "Моя тема" : theme_.name);
    if (!theme::WriteFileAtomic(file, json::Dump(doc)))
        MessageBoxW(hwnd_, L"Не удалось записать файл.", L"Экспорт темы", MB_ICONWARNING);
}

// ---------------- меню ----------------

void App::ShowContextMenu(POINT screen) {
    HMENU menu = CreatePopupMenu();
    HMENU themes = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kMenuOpenFiles, L"Открыть файлы...");
    AppendMenuW(menu, MF_STRING, kMenuOpenFolder, L"Открыть папку...");

    // Библиотека: вся найденная музыка, папки, автопоиск
    HMENU lib = CreatePopupMenu();
    HMENU libFolders = CreatePopupMenu();
    wchar_t allText[64];
    swprintf(allText, 64, L"Вся музыка (%d)", static_cast<int>(library_.Tracks().size()));
    AppendMenuW(lib, MF_STRING | (library_.Tracks().empty() ? MF_GRAYED : 0), kMenuLibraryAll, allText);
    AppendMenuW(lib, MF_STRING | (scanner_.Running() ? MF_GRAYED : 0), kMenuLibraryRescan,
                scanner_.Running() ? L"Идёт поиск музыки..." : L"Обновить");
    AppendMenuW(lib, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(lib, MF_STRING, kMenuLibraryAdd, L"Добавить папку...");
    const auto& folders = library_.Folders();
    for (size_t i = 0; i < folders.size() && i < 100; ++i)
        AppendMenuW(libFolders, MF_STRING, kMenuLibraryFolderFirst + i, folders[i].wstring().c_str());
    if (folders.empty()) AppendMenuW(libFolders, MF_STRING | MF_GRAYED, 0, L"Папок нет");
    AppendMenuW(lib, MF_POPUP, reinterpret_cast<UINT_PTR>(libFolders), L"Папки (нажмите, чтобы убрать)");
    AppendMenuW(lib, MF_STRING | (library_.AutoScan() ? MF_CHECKED : 0), kMenuLibraryAuto,
                L"Искать новую музыку автоматически");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(lib), L"Библиотека");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const auto& ids = themes_.BuiltinIds();
    for (size_t i = 0; i < ids.size(); ++i) {
        const theme::Theme* t = themes_.Builtin(ids[i]);
        const std::wstring name = ::text::Utf8ToWide(t ? t->name : ids[i]);
        const bool current = !userTheme_ && theme_.id == ids[i];
        AppendMenuW(themes, MF_STRING | (current ? MF_CHECKED : 0), kMenuThemeFirst + i, name.c_str());
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(themes), L"Тема");
    AppendMenuW(menu, MF_STRING, kMenuImportTheme, L"Импорт темы...");
    AppendMenuW(menu, MF_STRING, kMenuExportTheme, L"Экспорт темы...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (!view_.mini)
        AppendMenuW(menu, MF_STRING | (view_.playlistOpen ? MF_CHECKED : 0), kMenuPlaylist, L"Плейлист");
    AppendMenuW(menu, MF_STRING | (view_.mini ? MF_CHECKED : 0), kMenuMini, L"Мини-режим");
    AppendMenuW(menu, MF_STRING | (miniOnTop_ ? MF_CHECKED : 0), kMenuMiniTop, L"Мини-плеер поверх окон");
    AppendMenuW(menu, MF_STRING | (player_.Normalize() ? MF_CHECKED : 0), kMenuNormalize,
                L"Выравнивать громкость (ReplayGain)");

    // Плагины: их собственные пункты и раздел управления
    HMENU plug = CreatePopupMenu();
    const auto& items = plugins_.MenuItems();
    for (size_t i = 0; i < items.size() && i < 500; ++i)
        AppendMenuW(plug, MF_STRING, kMenuPluginItemFirst + i, ::text::Utf8ToWide(items[i].title).c_str());
    if (!items.empty()) AppendMenuW(plug, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(plug, MF_STRING, kMenuPlugins, L"Управление плагинами");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(plug), L"Плагины");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Выход");

    SetForegroundWindow(hwnd_);
    const UINT cmd = static_cast<UINT>(
        TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, screen.x, screen.y, hwnd_, nullptr));
    DestroyMenu(menu);  // вместе с подменю

    switch (cmd) {
        case 0: break;
        case kMenuOpenFiles: OpenFiles(false); break;
        case kMenuOpenFolder: OpenFiles(true); break;
        case kMenuImportTheme: ImportTheme(); break;
        case kMenuExportTheme: ExportTheme(); break;
        case kMenuPlaylist: view_.TogglePlaylist(Now()); break;
        case kMenuMini: view_.ToggleMini(Now()); break;
        case kMenuExit: CloseWindow(); break;
        case kMenuNormalize: player_.SetNormalize(!player_.Normalize()); break;
        case kMenuMiniTop:
            miniOnTop_ = !miniOnTop_;
            ApplyTopmost();
            break;
        case kMenuLibraryAll: LoadLibraryPlaylist(true); break;
        case kMenuLibraryRescan: StartLibraryScan(); break;
        case kMenuLibraryAdd: AddLibraryFolder(); break;
        case kMenuLibraryAuto:
            library_.SetAutoScan(!library_.AutoScan());
            RestartWatcher();
            if (library_.AutoScan()) StartLibraryScan();
            library_.Save(libraryPath_);
            break;
        case kMenuPlugins:
            plugins_.Discover({exeDir_ / L"plugins", UserPluginsDir()});
            RebuildPluginRows();
            view_.ShowPanelTab(view_.PluginsTab(), Now());
            break;
        default:
            if (cmd >= kMenuLibraryFolderFirst && cmd < kMenuLibraryFolderFirst + folders.size()) {
                const std::filesystem::path dir = folders[cmd - kMenuLibraryFolderFirst];
                const std::wstring q = L"Убрать папку из библиотеки?\n" + dir.wstring() +
                                       L"\n\nФайлы на диске не удаляются.";
                if (MessageBoxW(hwnd_, q.c_str(), L"Библиотека", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    library_.RemoveFolder(dir);
                    library_.Save(libraryPath_);
                    RestartWatcher();
                }
            }
            if (cmd >= kMenuPluginItemFirst && cmd < kMenuPluginItemFirst + items.size())
                plugins_.InvokeMenuItem(cmd - kMenuPluginItemFirst);
            if (cmd >= kMenuThemeFirst && cmd < kMenuThemeFirst + ids.size()) {
                userTheme_ = false;
                ApplyTheme(themes_.Make(ids[cmd - kMenuThemeFirst]), true);
            }
            break;
    }
    MarkDirty();
    Invalidate();
}

// ---------------- меню строки плейлиста ----------------

void App::ShowRowMenu(int row, view::Point at) {
    const pm::Playlist& pl = player_.GetPlaylist();
    if (row < 0 || row >= pl.Size()) return;
    const pm::PlayerState ps = player_.GetState();
    const bool current = row == ps.index;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kMenuRowPlay, current && ps.playing ? L"Пауза" : L"Играть");
    AppendMenuW(menu, MF_STRING | (current ? MF_GRAYED : 0), kMenuRowNext, L"Играть следующим");
    AppendMenuW(menu, MF_STRING, kMenuRowShow, L"Показать в папке");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuRowRemove, L"Убрать из плейлиста");
    POINT pt{ToPx(at.x), ToPx(at.y)};
    ClientToScreen(hwnd_, &pt);
    SetForegroundWindow(hwnd_);
    const UINT cmd = static_cast<UINT>(
        TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, hwnd_, nullptr));
    DestroyMenu(menu);
    const std::filesystem::path path = pl.At(row).path;
    switch (cmd) {
        case kMenuRowPlay:
            if (current) player_.Toggle();
            else player_.PlayIndex(row);
            break;
        case kMenuRowNext: player_.PlayNext(row); break;
        case kMenuRowShow:
            // Проводник с выделенным файлом
            if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str())) {
                SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                ILFree(pidl);
            }
            break;
        case kMenuRowRemove: player_.Remove(row); break;
        default: return;
    }
    RebuildRows();
    MarkDirty();
    Invalidate();
}

// ---------------- медиаклавиши ----------------

void App::RegisterMediaKeys() {
    // Глобально, чтобы работали и без фокуса. Если клавишу уже занял другой плеер, остаётся
    // запасной путь WM_APPCOMMAND (только когда окно активно).
    RegisterHotKey(hwnd_, kHotPlay, MOD_NOREPEAT, VK_MEDIA_PLAY_PAUSE);
    RegisterHotKey(hwnd_, kHotNext, MOD_NOREPEAT, VK_MEDIA_NEXT_TRACK);
    RegisterHotKey(hwnd_, kHotPrev, MOD_NOREPEAT, VK_MEDIA_PREV_TRACK);
    RegisterHotKey(hwnd_, kHotStop, MOD_NOREPEAT, VK_MEDIA_STOP);
}

void App::HandleMediaCommand(int command) {
    switch (command) {
        case APPCOMMAND_MEDIA_PLAY_PAUSE: TogglePlay(); break;
        case APPCOMMAND_MEDIA_PLAY: player_.Play(); break;
        case APPCOMMAND_MEDIA_PAUSE:
        case APPCOMMAND_MEDIA_STOP: player_.Pause(); break;
        case APPCOMMAND_MEDIA_NEXTTRACK: Next(); break;
        case APPCOMMAND_MEDIA_PREVIOUSTRACK: Prev(); break;
        default: break;
    }
    Invalidate();
}

// ---------------- IViewHost ----------------

void App::TogglePlay() {
    player_.Toggle();
    Invalidate();
}
void App::Next() {
    player_.Next();
    Invalidate();
}
void App::Prev() {
    player_.Prev();
    Invalidate();
}
void App::Seek(double seconds) {
    player_.Seek(seconds);
    Invalidate();
}
void App::SetVolume(float volume) {
    if (player_.GetState().muted && volume > 0) player_.SetMuted(false);
    player_.SetVolume(volume);
    Invalidate();
}
void App::ToggleMute() {
    player_.SetMuted(!player_.GetState().muted);
    Invalidate();
}
void App::CycleRepeat() {
    const pm::Repeat r = player_.GetState().repeat;
    player_.SetRepeat(r == pm::Repeat::Off ? pm::Repeat::All : r == pm::Repeat::All ? pm::Repeat::One : pm::Repeat::Off);
    Invalidate();
}
void App::ToggleShuffle() {
    player_.SetShuffle(!player_.GetState().shuffle);
    Invalidate();
}
void App::PlayIndex(int index) {
    player_.PlayIndex(index);
    Invalidate();
}

void App::ResizeWindow(view::Size dip) {
    // Мгновенно: левый верхний угол на месте, содержимое анимирует PlayerView
    SetWindowPos(hwnd_, nullptr, 0, 0, ToPx(dip.w), ToPx(dip.h), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    Invalidate();
}

void App::MinimizeWindow() { ShowWindow(hwnd_, SW_MINIMIZE); }
void App::CloseWindow() { PostMessageW(hwnd_, WM_CLOSE, 0, 0); }

void App::ShowMenu(view::Point at) {
    POINT pt{ToPx(at.x), ToPx(at.y)};
    ClientToScreen(hwnd_, &pt);
    ShowContextMenu(pt);
}

float App::MeasureText(view::TextStyle style, const std::string& s) {
    TextSpec spec;
    switch (style) {
        case view::TextStyle::Title: spec = Painter::TitleSpec(view_); break;
        case view::TextStyle::MiniTitle: spec = Painter::MiniTitleSpec(view_); break;
        case view::TextStyle::Tab: spec = Painter::TabSpecFor(view_, false); break;
        case view::TextStyle::TabSmall: spec = Painter::TabSpecFor(view_, true); break;
        case view::TextStyle::Pill: spec = Painter::PillSpec(); break;
        case view::TextStyle::Tooltip: spec = Painter::TooltipSpec(); break;
    }
    return text_.Measure(::text::Utf8ToWide(s), spec);
}

void App::PageAction(int page, int rowId, int actionId) {
    plugins_.PageAction(static_cast<size_t>(page), rowId, actionId);
    SyncPages();
    Invalidate();
}

void App::SyncPages() {
    if (plugins_.PagesRevision() == pagesRevision_) return;
    pagesRevision_ = plugins_.PagesRevision();
    std::vector<view::PanelPage> pages;
    for (const auto& src : plugins_.Pages()) {
        view::PanelPage p;
        p.title = src.title;
        p.emptyText = src.emptyText;
        for (const auto& a : src.actions) p.actions.push_back({a.id, a.label, 0});
        for (const auto& r : src.rows) {
            view::PanelRow row{r.id, r.title, r.subtitle, r.status, r.progress, r.state, {}};
            for (const auto& a : r.actions) row.actions.push_back({a.id, a.label, 0});
            p.rows.push_back(std::move(row));
        }
        pages.push_back(std::move(p));
    }
    view_.SetPages(std::move(pages), Now());
    Invalidate();
}

bool App::InPlaylist(const std::filesystem::path& p) const {
    const pm::Playlist& pl = player_.GetPlaylist();
    for (int i = 0; i < pl.Size(); ++i)
        if (pl.At(i).path == p) return true;
    return false;
}

void App::AddFilesFromPlugin(const std::vector<std::string>& paths, bool play) {
    std::vector<std::filesystem::path> files;
    int playIndex = -1;
    const pm::Playlist& pl = player_.GetPlaylist();
    for (const auto& s : paths) {
        const std::filesystem::path p = ::text::Utf8ToPath(s);
        if (!pm::IsSupportedAudio(p)) continue;
        // Уже в плейлисте (например, библиотека успела добавить) - не дублируем
        int found = -1;
        for (int i = 0; i < pl.Size() && found < 0; ++i)
            if (pl.At(i).path == p) found = i;
        if (found >= 0) {
            if (playIndex < 0) playIndex = found;
        } else {
            files.push_back(p);
        }
    }
    const int before = pl.Size();
    if (!files.empty()) {
        player_.LoadFiles(files, true);
        RebuildRows();
        if (playIndex < 0) playIndex = before;
    }
    if (play && playIndex >= 0) player_.PlayIndex(playIndex);
    else if (!player_.GetState().hasTrack && player_.GetPlaylist().Size() > 0) player_.OpenAt(0, 0, false);
    MarkDirty();
    Invalidate();
}

// ---------------- кадры ----------------

double App::Now() const {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return freq_.QuadPart ? static_cast<double>(t.QuadPart) / static_cast<double>(freq_.QuadPart) : 0;
}

int App::ToPx(float dip) const { return static_cast<int>(std::lround(dip * dpi_ / 96.f)); }

view::Point App::ToDip(LPARAM lp) const {
    const float k = 96.f / dpi_;
    return {static_cast<float>(GET_X_LPARAM(lp)) * k, static_cast<float>(GET_Y_LPARAM(lp)) * k};
}

view::Size App::ClientDip() const {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const float k = 96.f / dpi_;
    return {static_cast<float>(rc.right - rc.left) * k, static_cast<float>(rc.bottom - rc.top) * k};
}

void App::RenderFrame() {
    dirty_ = false;
    visible_ = IsWindowVisible(hwnd_) && !IsIconic(hwnd_);
    const double t = Now();
    const pm::PlayerState ps = player_.GetState();
    view::PlaybackSnapshot snap;
    snap.playing = ps.playing;
    snap.hasTrack = ps.hasTrack;
    snap.position = ps.position;
    snap.duration = ps.duration;
    snap.volume = ps.volume;
    snap.muted = ps.muted;
    snap.repeat = ps.repeat;
    snap.shuffle = ps.shuffle;
    snap.index = ps.index;
    snap.count = ps.count;

    // Спектр считаем, только когда визуализатор видно и звук идёт
    const std::vector<float>* spectrum = nullptr;
    if (visible_ && ps.playing && view_.WantsSpectrum() && player_.AudioDeviceRunning() && view_.viz.Bars() > 0) {
        if (player_.ReadPcm(pcm_.data(), pcm_.size()) == pcm_.size()) {
            analyzer_.Compute(pcm_.data(), player_.SampleRate(), view_.viz.Bars(), spectrum_);
            spectrum = &spectrum_;
        }
    }
    SyncPages();
    view_.SetBusy(scanner_.Running(), "Поиск музыки");
    view_.Update(t, snap, spectrum, visible_);
    ApplyTopmost();
    // Подсказка или уведомление ждут своего времени: разовый таймер вместо цикла кадров
    const double wake = view_.NextLazyWake();
    if (wake < 1e300 && !view_.NeedsFrames())
        SetTimer(hwnd_, kLazyTimer, static_cast<UINT>(std::clamp((wake - t) * 1000.0 + 5, 10.0, 60000.0)), nullptr);
    else
        KillTimer(hwnd_, kLazyTimer);
    taskbar_.Update(ps.hasTrack, ps.playing, ps.position, ps.duration);
    smtc_.Update(ps.hasTrack, ps.playing, ps.position, ps.duration);
    if (lastPlaying_ != (ps.playing ? 1 : 0)) {
        if (lastPlaying_ >= 0) plugins_.NotifyPlayState(ps.playing);
        lastPlaying_ = ps.playing ? 1 : 0;
    }

    // Раз в секунду: мини-режим во время игры и свёрнутое окно (полоса на панели задач)
    const bool wantTicks = view_.WantsSecondTicks() || (ps.playing && !visible_);
    if (wantTicks != tickTimer_) {
        if (wantTicks) SetTimer(hwnd_, kTickTimer, 1000, nullptr);
        else KillTimer(hwnd_, kTickTimer);
        tickTimer_ = wantTicks;
    }
    // Во время игры позиция сохраняется раз в 30 секунд; на паузе таймера нет
    if (ps.playing != saveTick_) {
        if (ps.playing) SetTimer(hwnd_, kSaveTick, 30000, nullptr);
        else KillTimer(hwnd_, kSaveTick);
        saveTick_ = ps.playing;
    }
    if (!visible_) return;

    ID2D1DeviceContext* dc = renderer_.BeginFrame();
    if (!dc) return;
    PaintInput in;
    in.window = ClientDip();
    in.systemBackdrop = acrylicOn_;
    in.rows = &rows_;
    in.countText = countText_;
    in.emptyHint = scanner_.Running() ? L"Ищу музыку в папках библиотеки..." : L"Перетащите сюда файлы или папку";
    in.plugins = &pluginRows_;
    in.pluginCountText = pluginCountText_;
    in.pluginsHint = L"Плагинов пока нет. Положите папку плагина в plugins рядом с PlayerMoon.exe";
    painter_.Paint(dc, view_, in);
    if (!renderer_.EndFrame()) dirty_ = true;  // устройство пересоздано - повторить кадр
}

void App::MessageLoop() {
    MSG msg;
    for (;;) {
        const bool frames = dirty_ || (visible_ && view_.NeedsFrames());
        HANDLE waitable = renderer_.FrameWaitable();
        if (frames && waitable) {
            // Ждём, пока DWM готов принять кадр, и одновременно обрабатываем сообщения
            const DWORD r = MsgWaitForMultipleObjectsEx(1, &waitable, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (r == WAIT_OBJECT_0) {
                RenderFrame();
                continue;
            }
        } else if (frames) {
            RenderFrame();
        } else {
            // Покой: ноль кадров, поток спит до сообщения
            if (GetMessageW(&msg, nullptr, 0, 0) <= 0) return;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

// ---------------- окно ----------------

LRESULT CALLBACK App::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* app = nullptr;
    if (msg == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        app->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    } else {
        app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return app ? app->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == taskbarCreatedMsg_ && taskbarCreatedMsg_) {
        taskbar_.Attach(hwnd_, renderer_.Factory(), renderer_.Wic());
        Invalidate();
        return 0;
    }
    switch (msg) {
        case WM_NCCALCSIZE:
            // Клиентская область = всё окно: рамку и заголовок рисуем сами
            if (wp) return 0;
            break;
        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd_, &pt);
            const float k = 96.f / dpi_;
            const view::Point p{static_cast<float>(pt.x) * k, static_cast<float>(pt.y) * k};
            return view_.HitTest(p) == view::PlayerView::Hit::Caption ? HTCAPTION : HTCLIENT;
        }
        case WM_SIZE:
            renderer_.Resize(LOWORD(lp), HIWORD(lp));
            visible_ = wp != SIZE_MINIMIZED;
            Invalidate();
            return 0;
        case WM_SHOWWINDOW:
            Invalidate();
            break;
        case WM_DPICHANGED: {
            dpi_ = static_cast<float>(HIWORD(wp));
            renderer_.SetDpi(dpi_);
            // Размер окна в DIP не меняется - пересчитываем пиксели от новой точки
            const RECT* suggested = reinterpret_cast<const RECT*>(lp);
            const view::Size dip = view_.mini ? view::Size{view::kMiniWidth, view::kMiniHeight}
                                              : view_.layout ? view_.layout->WindowSize(view_.layoutInput,
                                                                                        view_.playlistOpen)
                                                             : view::Size{420, 680};
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, ToPx(dip.w), ToPx(dip.h),
                         SWP_NOZORDER | SWP_NOACTIVATE);
            Invalidate();
            return 0;
        }
        case WM_MOUSEMOVE:
            if (!trackingMouse_) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
                trackingMouse_ = TrackMouseEvent(&tme);
            }
            view_.OnMouseMove(ToDip(lp), Now());
            Invalidate();
            return 0;
        case WM_MOUSELEAVE:
            trackingMouse_ = false;
            view_.OnMouseLeave(Now());
            Invalidate();
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            SetCapture(hwnd_);
            view_.OnMouseDown(ToDip(lp), Now());
            Invalidate();
            return 0;
        case WM_LBUTTONUP:
            ReleaseCapture();
            view_.OnMouseUp(ToDip(lp), Now());
            MarkDirty();
            Invalidate();
            return 0;
        case WM_RBUTTONUP:
            view_.OnRightClick(ToDip(lp));
            return 0;
        case WM_NCRBUTTONUP:
            // Правый клик по заголовку - наше меню вместо системного
            if (wp == HTCAPTION) {
                ShowContextMenu({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
                return 0;
            }
            break;
        case WM_MOUSEWHEEL: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd_, &pt);
            const float k = 96.f / dpi_;
            view_.OnWheel({static_cast<float>(pt.x) * k, static_cast<float>(pt.y) * k},
                          static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA, Now());
            MarkDirty();
            Invalidate();
            return 0;
        }
        case WM_KEYDOWN: {
            const pm::PlayerState ps = player_.GetState();
            switch (wp) {
                case VK_SPACE: TogglePlay(); break;
                case VK_LEFT: Seek(std::max(0.0, ps.position - 5)); break;
                case VK_RIGHT: Seek(ps.position + 5); break;
                case VK_UP: SetVolume(std::min(1.f, ps.volume + 0.05f)); break;
                case VK_DOWN: SetVolume(std::max(0.f, ps.volume - 0.05f)); break;
                case 'M': ToggleMute(); break;
                case 'L': view_.TogglePlaylist(Now()); break;
                case 'O':
                    if (GetKeyState(VK_CONTROL) < 0) OpenFiles(false);
                    break;
                default: break;
            }
            MarkDirty();
            Invalidate();
            return 0;
        }
        case WM_HOTKEY:
            switch (static_cast<int>(wp)) {
                case kHotPlay: HandleMediaCommand(APPCOMMAND_MEDIA_PLAY_PAUSE); break;
                case kHotNext: HandleMediaCommand(APPCOMMAND_MEDIA_NEXTTRACK); break;
                case kHotPrev: HandleMediaCommand(APPCOMMAND_MEDIA_PREVIOUSTRACK); break;
                case kHotStop: HandleMediaCommand(APPCOMMAND_MEDIA_STOP); break;
                default: break;
            }
            return 0;
        case WM_APPCOMMAND: {
            const int cmd = GET_APPCOMMAND_LPARAM(lp);
            if (cmd == APPCOMMAND_MEDIA_PLAY_PAUSE || cmd == APPCOMMAND_MEDIA_PLAY || cmd == APPCOMMAND_MEDIA_PAUSE ||
                cmd == APPCOMMAND_MEDIA_STOP || cmd == APPCOMMAND_MEDIA_NEXTTRACK ||
                cmd == APPCOMMAND_MEDIA_PREVIOUSTRACK) {
                HandleMediaCommand(cmd);
                return TRUE;
            }
            break;
        }
        case WM_DROPFILES:
            LoadDropped(reinterpret_cast<HDROP>(wp));
            return 0;
        case WM_TIMER:
            if (wp == kTickTimer) Invalidate();
            if (wp == kSaveTimer) {
                KillTimer(hwnd_, kSaveTimer);
                SaveCurrentState();
            }
            if (wp == kSaveTick) SaveCurrentState();
            if (wp == kLazyTimer) {
                KillTimer(hwnd_, kLazyTimer);
                Invalidate();
            }
            return 0;
        case kSmtcMessage:
            switch (wp) {
                case Smtc::kPlay: player_.Play(); break;
                case Smtc::kPause:
                case Smtc::kStop: player_.Pause(); break;
                case Smtc::kNext: Next(); break;
                case Smtc::kPrevious: Prev(); break;
                case Smtc::kSeek: Seek(static_cast<double>(lp) / 1000.0); break;
                default: break;
            }
            Invalidate();
            return 0;
        case kPluginMessage:
            plugins_.RunPosted();
            SyncPages();
            Invalidate();
            return 0;
        case WM_COMMAND:
            // Кнопки в превью окна на панели задач
            if (HIWORD(wp) == THBN_CLICKED) {
                switch (LOWORD(wp)) {
                    case Taskbar::kPrev: Prev(); break;
                    case Taskbar::kPlayPause: TogglePlay(); break;
                    case Taskbar::kNext: Next(); break;
                    default: break;
                }
                MarkDirty();
                return 0;
            }
            break;
        case WM_COPYDATA:
            HandleCopyData(reinterpret_cast<const COPYDATASTRUCT*>(lp));
            return TRUE;
        case WM_MOVING:
            SnapToEdges(reinterpret_cast<RECT*>(lp));
            return TRUE;
        case WM_EXITSIZEMOVE:
            MarkDirty();  // новое место окна
            return 0;
        case kLibraryMessage:
            OnLibraryScanned();
            return 0;
        case kWatchMessage:
            StartLibraryScan();
            return 0;
        case kWakeMessage:
            wakePosted_ = false;
            player_.PumpEvents();
            Invalidate();
            return 0;
        case WM_SETTINGCHANGE: {
            const bool before = systemReduce_;
            ReadSystemReduce();
            if (before != systemReduce_) ApplyTheme(theme_, false);
            break;
        }
        case WM_ACTIVATE:
            // Вернулись из папки плагинов - перечитать список
            if (LOWORD(wp) != WA_INACTIVE && pluginsRefreshPending_) {
                pluginsRefreshPending_ = false;
                plugins_.Discover({exeDir_ / L"plugins", UserPluginsDir()});
                RebuildPluginRows();
            }
            Invalidate();
            break;
        case WM_ENDSESSION:
            if (wp) {
                SaveCurrentState();
                library_.Save(libraryPath_);
            }
            return 0;
        case WM_CLOSE:
            SaveCurrentState();
            library_.Save(libraryPath_);
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            for (int id = kHotPlay; id <= kHotStop; ++id) UnregisterHotKey(hwnd_, id);
            KillTimer(hwnd_, kTickTimer);
            KillTimer(hwnd_, kSaveTimer);
            KillTimer(hwnd_, kSaveTick);
            PostQuitMessage(0);
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

// ---------------- один экземпляр ----------------

bool App::ForwardToRunningInstance() {
    // Первый экземпляр мог ещё не создать окно - ждём до двух секунд
    HWND other = nullptr;
    for (int i = 0; i < 40 && !other; ++i) {
        other = FindWindowW(kClassName, nullptr);
        if (!other) Sleep(50);
    }
    if (!other) return false;
    // Пути через '\n' (в путях Windows его не бывает); пустой список - просто показать окно
    std::wstring data;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) {
            if (!data.empty()) data += L'\n';
            data += argv[i];
        }
        LocalFree(argv);
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(other, &pid);
    AllowSetForegroundWindow(pid);  // чтобы окно могло выйти на передний план
    COPYDATASTRUCT cds{};
    cds.dwData = kCopyDataPaths;
    cds.cbData = static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t));
    cds.lpData = data.data();
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(other, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG, 5000,
                               &result) != 0;
}

void App::HandleCopyData(const COPYDATASTRUCT* cds) {
    if (!cds || cds->dwData != kCopyDataPaths || cds->cbData < sizeof(wchar_t)) return;
    const std::wstring data(static_cast<const wchar_t*>(cds->lpData), cds->cbData / sizeof(wchar_t) - 1);
    std::vector<std::filesystem::path> paths;
    size_t start = 0;
    while (start < data.size()) {
        const size_t end = std::min(data.find(L'\n', start), data.size());
        if (end > start) paths.emplace_back(data.substr(start, end - start));
        start = end + 1;
    }
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
    if (paths.empty()) return;
    // Проводник запускает по экземпляру на каждый выделенный файл: файлы, пришедшие подряд
    // (меньше чем через 1.5 с), дописываются к только что открытым
    const double now = Now();
    const bool append = now - lastCopyData_ < 1.5;
    lastCopyData_ = now;
    LoadPaths(paths, append);
}

// ---------------- библиотека ----------------

void App::InitLibrary(bool firstRun) {
    libraryPath_ = statePath_.parent_path() / L"library.json";
    const bool loaded = library_.Load(libraryPath_);
    if (!loaded || (firstRun && library_.Folders().empty())) {
        // Первый запуск: стандартные папки, где обычно лежит музыка
        auto known = [](REFKNOWNFOLDERID id) {
            std::filesystem::path p;
            PWSTR s = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &s))) p = s;
            CoTaskMemFree(s);
            return p;
        };
        std::vector<std::filesystem::path> candidates = {known(FOLDERID_Music), known(FOLDERID_Downloads)};
        const std::filesystem::path oneDrive = known(FOLDERID_SkyDrive);
        if (!oneDrive.empty()) {
            candidates.push_back(oneDrive / L"Music");
            candidates.push_back(oneDrive / L"Музыка");
        }
        for (const auto& c : candidates) {
            std::error_code ec;
            if (!c.empty() && std::filesystem::is_directory(c, ec)) library_.AddFolder(c);
        }
        library_.Save(libraryPath_);
    }
    // Плейлист пуст, а библиотека уже знает треки (кэш прошлого запуска) - сразу показываем их
    if (player_.GetPlaylist().Empty() && !library_.Tracks().empty()) LoadLibraryPlaylist(false);
    RestartWatcher();
    if (library_.AutoScan()) StartLibraryScan();
}

void App::RestartWatcher() {
    watcher_.Stop();
    if (library_.AutoScan()) watcher_.Start(library_.Folders(), hwnd_, kWatchMessage);
}

void App::StartLibraryScan() {
    if (library_.Folders().empty()) return;
    HWND hwnd = hwnd_;
    // Уже идёт - повторим по окончании (изменения могли прийти посреди обхода)
    if (!scanner_.Start(library_.Folders(), library_.Tracks(), [hwnd] { PostMessageW(hwnd, kLibraryMessage, 0, 0); }))
        rescanPending_ = true;
    Invalidate();
}

void App::OnLibraryScanned() {
    std::vector<pm::LibraryTrack> tracks;
    if (!scanner_.Take(tracks)) return;
    // Новые для библиотеки файлы (не просто отсутствующие в плейлисте: убранное руками не возвращается)
    std::set<std::filesystem::path> known;
    for (const auto& t : library_.Tracks()) known.insert(t.path);
    std::vector<pm::PlaylistEntry> fresh;
    for (const auto& t : tracks)
        if (!known.count(t.path) && !InPlaylist(t.path)) fresh.push_back({t.path, t.meta});
    const bool wasEmpty = library_.Tracks().empty();
    library_.SetTracks(std::move(tracks));
    library_.Save(libraryPath_);

    if (player_.GetPlaylist().Empty() && !library_.Tracks().empty()) {
        LoadLibraryPlaylist(false);
    } else if (playlistFromLibrary_ && !fresh.empty() && !wasEmpty) {
        player_.LoadTracks(fresh, true);
        RebuildRows();
        view_.ShowToast(fresh.size() == 1 ? "Новый трек в библиотеке" : "Новых треков в библиотеке: " + std::to_string(fresh.size()), Now());
        MarkDirty();
    }
    if (rescanPending_) {
        rescanPending_ = false;
        StartLibraryScan();
    }
    Invalidate();
}

void App::LoadLibraryPlaylist(bool play) {
    if (library_.Tracks().empty()) return;
    player_.LoadTracks(library_.Entries());
    playlistFromLibrary_ = true;
    RebuildRows();
    if (play) player_.PlayIndex(0);
    else player_.OpenAt(0, 0, false);
    MarkDirty();
    Invalidate();
}

void App::AddLibraryFolder() {
    auto paths = PickPaths(hwnd_, true, false, nullptr, nullptr);
    if (paths.empty()) return;
    if (library_.AddFolder(paths[0])) {
        library_.Save(libraryPath_);
        RestartWatcher();
        StartLibraryScan();
    }
}

void App::LearnFolders(const std::vector<std::filesystem::path>& paths) {
    bool changed = false;
    for (const auto& p : paths) changed = library_.LearnFrom(p) || changed;
    if (!changed) return;
    library_.Save(libraryPath_);
    RestartWatcher();
    StartLibraryScan();
}

// ---------------- плагины ----------------

std::filesystem::path App::UserPluginsDir() const {
    // Рядом с exe, если туда можно писать (там же state.json), иначе в %APPDATA%\PlayerMoon
    return statePath_.parent_path() / L"plugins";
}

void App::InitPlugins(const std::set<std::string>& enabled) {
    plugins_.SetDataRoot(statePath_.parent_path() / L"plugins-data");
    pm::PluginHostFunctions host;
    host.playback = [this] {
        const pm::PlayerState ps = player_.GetState();
        return PmPlayback{ps.playing ? 1 : 0, ps.index, ps.count, ps.position, ps.duration, ps.volume};
    };
    host.command = [this](int cmd) {
        switch (cmd) {
            case PM_CMD_TOGGLE: player_.Toggle(); break;
            case PM_CMD_PLAY: player_.Play(); break;
            case PM_CMD_PAUSE: player_.Pause(); break;
            case PM_CMD_NEXT: player_.Next(); break;
            case PM_CMD_PREV: player_.Prev(); break;
            default: break;
        }
        Invalidate();
    };
    host.seek = [this](double s) {
        player_.Seek(s);
        Invalidate();
    };
    // Из потоков плагинов: только разбудить окно
    HWND hwnd = hwnd_;
    host.wake = [hwnd] { PostMessageW(hwnd, kPluginMessage, 0, 0); };
    host.pageShow = [this](size_t page) {
        SyncPages();
        view_.ShowPanelTab(view_.PageTab(static_cast<int>(page)), Now());
        Invalidate();
    };
    host.toast = [this](const std::string& t) {
        view_.ShowToast(t, Now());
        Invalidate();
    };
    host.clipboardText = [this] { return ClipboardText(hwnd_); };
    host.openPath = [this](const std::string& p) {
        ShellExecuteW(hwnd_, L"open", ::text::Utf8ToWide(p).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    };
    host.addFiles = [this](const std::vector<std::string>& paths, bool play) { AddFilesFromPlugin(paths, play); };
    host.addLibraryFolder = [this](const std::string& p) {
        std::error_code ec;
        const std::filesystem::path dir = ::text::Utf8ToPath(p);
        std::filesystem::create_directories(dir, ec);
        if (library_.AddFolder(dir)) {
            library_.Save(libraryPath_);
            if (started_) {
                RestartWatcher();
                StartLibraryScan();
            }
        }
    };
    host.pickFolder = [this](const std::string& title) {
        auto paths = PickPaths(hwnd_, true, false, nullptr, nullptr, ::text::Utf8ToWide(title).c_str());
        return paths.empty() ? std::string() : ::text::PathToUtf8(paths[0]);
    };
    plugins_.SetHost(host);
    plugins_.Discover({exeDir_ / L"plugins", UserPluginsDir()});
    plugins_.EnableSaved(enabled);
    RebuildPluginRows();
    SyncPages();
}

void App::RebuildPluginRows() {
    pluginRows_.clear();
    std::vector<bool> on;
    int enabled = 0;
    for (const auto& p : plugins_.Plugins()) {
        pluginRows_.push_back({::text::Utf8ToWide(p.name), ::text::Utf8ToWide(p.version),
                               ::text::Utf8ToWide(p.description), ::text::Utf8ToWide(p.error)});
        on.push_back(p.enabled);
        enabled += p.enabled ? 1 : 0;
    }
    wchar_t buf[64];
    swprintf(buf, 64, L"включено %d из %d", enabled, static_cast<int>(pluginRows_.size()));
    pluginCountText_ = buf;
    view_.SetPlugins(on, Now());
    SyncPages();
    Invalidate();
}

void App::TogglePlugin(int index) {
    const auto& list = plugins_.Plugins();
    if (index < 0 || index >= static_cast<int>(list.size())) return;
    const pm::PluginInfo& p = list[static_cast<size_t>(index)];
    if (!p.enabled) {
        // Плагин - чужой код внутри плеера: включение только с явного согласия
        const std::wstring q = L"Включить плагин «" + ::text::Utf8ToWide(p.name) +
                               L"»?\n\nПлагин работает внутри PlayerMoon с правами вашей учётной записи. "
                               L"Включайте только плагины, которым доверяете.";
        if (MessageBoxW(hwnd_, q.c_str(), L"Плагины", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    }
    const bool wasEnabled = p.enabled;
    const std::string name = p.name;
    if (!plugins_.SetEnabled(static_cast<size_t>(index), !wasEnabled) && !wasEnabled)
        view_.ShowToast("Плагин «" + name + "» не запустился", Now());
    RebuildPluginRows();
    MarkDirty();
}

void App::OpenPluginsFolder() {
    const std::filesystem::path dir = UserPluginsDir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    ShellExecuteW(hwnd_, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    pluginsRefreshPending_ = true;
}

void App::NotifyPluginsTrack() {
    const pm::NowPlaying& np = player_.Current();
    if (np.index < 0 || !player_.GetState().hasTrack) {
        plugins_.NotifyTrack(nullptr);
        return;
    }
    // Строки живут до конца вызова - как требует интерфейс плагинов
    const std::string path = ::text::PathToUtf8(np.path);
    PmTrack t{path.c_str(), np.title.c_str(), np.tags.artist.c_str(), np.tags.album.c_str(),
              player_.GetState().duration};
    plugins_.NotifyTrack(&t);
}

}  // namespace ui
