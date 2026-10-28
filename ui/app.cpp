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
constexpr UINT kWakeMessage = WM_APP + 1;  // аудиопоток: есть события для PumpEvents
constexpr UINT_PTR kTickTimer = 1;         // раз в секунду: время в мини-режиме

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
    kMenuThemeFirst = 100,  // встроенные темы по порядку
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
                                             const wchar_t* filterSpec) {
    std::vector<std::filesystem::path> out;
    Microsoft::WRL::ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return out;
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

}  // namespace

// ---------------- запуск ----------------

int App::Run(HINSTANCE instance, int show) {
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

    ShowWindow(hwnd_, show);
    RegisterMediaKeys();
    if (silent) {
        const std::wstring msg = L"Не удалось открыть звуковое устройство, звук выключен.\n" + ::text::Utf8ToWide(error);
        MessageBoxW(hwnd_, msg.c_str(), L"PlayerMoon", MB_ICONWARNING);
    }
    MessageLoop();

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

    // WS_CAPTION нужен для системной тени и скруглённых углов; саму рамку убирает WM_NCCALCSIZE
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_APPWINDOW | WS_EX_ACCEPTFILES, kClassName,
                            L"PlayerMoon", style, CW_USEDEFAULT, CW_USEDEFAULT, 420, 680, nullptr, nullptr, instance,
                            this);
    if (!hwnd_) return false;
    dpi_ = static_cast<float>(GetDpiForWindow(hwnd_));

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
    pm::LoadState(statePath_, st);  // нет файла - значения по умолчанию

    // Тема: своя (документ целиком) или встроенная по id
    theme::Theme t;
    std::string err;
    userTheme_ = !st.userTheme.IsNull() && themes_.FromDocument(st.userTheme, t, &err);
    if (!userTheme_) t = themes_.Make(st.themeId);

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

// ---------------- тема ----------------

void App::ReadSystemReduce() {
    BOOL anim = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0);
    systemReduce_ = !anim;
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
    countText_ = buf;
    view_.SetPlaylistSize(pl.Size());
}

void App::LoadPaths(const std::vector<std::filesystem::path>& paths) {
    // Одна папка - сканируется целиком; иначе берём файлы и папки по очереди
    std::vector<std::filesystem::path> files;
    bool replaced = false;
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
    if (player_.GetPlaylist().Size() > 0) player_.PlayIndex(0);
    Invalidate();
}

void App::OpenFiles(bool folder) {
    auto paths = PickPaths(hwnd_, folder, !folder, L"Аудио (MP3, FLAC, WAV)", L"*.mp3;*.flac;*.wav");
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
        default:
            if (cmd >= kMenuThemeFirst && cmd < kMenuThemeFirst + ids.size()) {
                userTheme_ = false;
                ApplyTheme(themes_.Make(ids[cmd - kMenuThemeFirst]), true);
            }
            break;
    }
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
    TextSpec spec = style == view::TextStyle::Title ? Painter::TitleSpec(view_) : Painter::MiniTitleSpec(view_);
    return text_.Measure(::text::Utf8ToWide(s), spec);
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
    view_.Update(t, snap, spectrum, visible_);
    if (!visible_) return;

    ID2D1DeviceContext* dc = renderer_.BeginFrame();
    if (!dc) return;
    PaintInput in;
    in.window = ClientDip();
    in.systemBackdrop = acrylicOn_;
    in.rows = &rows_;
    in.countText = countText_;
    in.emptyHint = L"Перетащите сюда файлы или папку";
    painter_.Paint(dc, view_, in);
    if (!renderer_.EndFrame()) dirty_ = true;  // устройство пересоздано - повторить кадр

    // В мини-режиме во время игры - один кадр в секунду по таймеру, без цикла кадров
    const bool wantTicks = view_.WantsSecondTicks();
    if (wantTicks != tickTimer_) {
        if (wantTicks) SetTimer(hwnd_, kTickTimer, 1000, nullptr);
        else KillTimer(hwnd_, kTickTimer);
        tickTimer_ = wantTicks;
    }
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
            Invalidate();
            break;
        case WM_ENDSESSION:
            if (wp) SaveCurrentState();
            return 0;
        case WM_CLOSE:
            SaveCurrentState();
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            for (int id = kHotPlay; id <= kHotStop; ++id) UnregisterHotKey(hwnd_, id);
            KillTimer(hwnd_, kTickTimer);
            PostQuitMessage(0);
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace ui
