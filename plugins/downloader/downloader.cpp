// Плагин «Загрузка музыки»: страница «Загрузки» в панели плеера.
//
// Как работает: ссылка из буфера обмена (кнопка «Вставить ссылку») уходит в очередь; фоновый
// поток по одной запускает yt-dlp (внешняя программа, ставится отдельно) и читает его вывод;
// ход загрузки приходит в поток окна через host->post и показывается строкой с прогрессом.
// Готовые файлы сразу попадают в плейлист, папка загрузок - в библиотеку плеера.
//
// Вход в аккаунт (подписка YouTube Music Premium или Яндекс Плюс) - через cookies браузера:
// yt-dlp сам читает их (--cookies-from-browser). Сами файлы из кэша браузера не берутся: там
// обрывки потоков, а у части сервисов они зашифрованы.
//
// Потоки: всё, что трогает страницу, - в потоке окна. Фоновый поток владеет только процессом
// yt-dlp; общая очередь заданий под SRWLOCK.

#include <windows.h>

#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <cstdio>
#include <deque>
#include <string>
#include <thread>
#include <vector>

#include "plugins/downloader/ytdlp.h"
#include "plugins/sdk/playermoon_plugin.h"

namespace {

constexpr int kPage = 1;
constexpr int kToolsRow = 1;  // строка о yt-dlp и ffmpeg
// Кнопки шапки
constexpr int kActPaste = 1, kActFolder = 2, kActLogin = 3;
// Кнопки строк
constexpr int kActCancel = 1, kActPlay = 2, kActShow = 3, kActRemove = 4, kActRetry = 5;
constexpr int kActGetYtdlp = 11, kActBinFolder = 12, kActRecheck = 13, kActGetFfmpeg = 14, kActUpdate = 15;
// Пункты меню
constexpr int kMenuShow = 1, kMenuChangeFolder = 2;

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool FileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Программа: сначала в bin плагина, потом в PATH
std::wstring FindTool(const std::wstring& binDir, const wchar_t* exe) {
    const std::wstring local = binDir + L"\\" + exe;
    if (FileExists(local)) return local;
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = SearchPathW(nullptr, exe, nullptr, MAX_PATH * 2, buf, nullptr);
    return n > 0 && n < MAX_PATH * 2 ? std::wstring(buf, n) : std::wstring();
}

struct Job {
    enum class State { Waiting, Running, Done, Error, Cancelled };
    enum class Kind { Download, Probe, Update };
    int row = 0;
    Kind kind = Kind::Download;
    std::string url;
    dl::Source source = dl::Source::Other;
    State state = State::Waiting;
    std::string title, artist, error;
    double percent = -1;
    std::string speed;
    bool converting = false;
    int item = 0, items = 0;
    std::vector<std::string> files;
};

// Сообщение из фонового потока в поток окна
struct Msg {
    struct Plugin* plugin;
    int row;
    bool finished;  // процесс завершился
    int exitCode;
    bool cancelled;
    dl::Event event;
    std::string raw;  // строка вывода (для проверки инструментов)
};

struct Plugin {
    const PmHost* host = nullptr;
    std::wstring pluginDir, binDir, dataDir;
    std::string outDir;   // UTF-8
    std::string browser;  // "", firefox, chrome, edge
    std::wstring ytdlp, ffmpeg;
    std::string version;
    int yandex = -1;  // поддержка Яндекс Музыки в этой сборке yt-dlp: -1 неизвестно
    bool probing = false;
    bool warned = false;  // напоминание о правах уже показано

    std::vector<Job> jobs;  // только поток окна
    int nextRow = 100;

    // Очередь для фонового потока
    SRWLOCK lock = SRWLOCK_INIT;
    CONDITION_VARIABLE wake = CONDITION_VARIABLE_INIT;
    std::deque<Job> queue;  // копии заданий
    bool stop = false;
    HANDLE running = nullptr;  // объект задания текущего процесса (для отмены)
    int runningRow = 0;
    std::thread worker;

    void Log(const std::string& s) { host->log(host->ctx, s.c_str()); }
    void Toast(const std::string& s) { host->toast(host->ctx, s.c_str()); }
};

// ---------------- настройки ----------------

void LoadSettings(Plugin& p) {
    FILE* f = _wfopen((p.dataDir + L"\\settings.txt").c_str(), L"rb");
    if (f) {
        char line[2048];
        while (std::fgets(line, sizeof(line), f)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            const size_t eq = s.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = s.substr(0, eq), v = s.substr(eq + 1);
            if (k == "outDir" && !v.empty()) p.outDir = v;
            if (k == "browser") p.browser = v;
        }
        std::fclose(f);
    }
    if (p.outDir.empty()) {
        // По умолчанию: Музыка\PlayerMoon
        PWSTR music = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Music, 0, nullptr, &music)))
            p.outDir = Utf8(std::wstring(music) + L"\\PlayerMoon");
        CoTaskMemFree(music);
    }
}

void SaveSettings(const Plugin& p) {
    FILE* f = _wfopen((p.dataDir + L"\\settings.txt").c_str(), L"wb");
    if (!f) return;
    std::fprintf(f, "outDir=%s\nbrowser=%s\n", p.outDir.c_str(), p.browser.c_str());
    std::fclose(f);
}

// ---------------- запуск процесса (фоновый поток) ----------------

// Запустить yt-dlp и отдавать строки вывода. Процесс - в объекте задания: отмена и выход
// плеера убивают и его, и запущенный им ffmpeg.
int RunTool(Plugin& p, const std::wstring& exe, const std::vector<std::string>& args, int row,
            void (*onLine)(Plugin&, int, const std::string&), bool* cancelled) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return -1;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\" " + Wide(dl::QuoteArgs(args));
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{};
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof(lim));
    const BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                                   nullptr, nullptr, &si, &pi);
    CloseHandle(writeEnd);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(readEnd);
        CloseHandle(job);
        return -1;
    }
    AssignProcessToJobObject(job, pi.hProcess);
    AcquireSRWLockExclusive(&p.lock);
    p.running = job;
    p.runningRow = row;
    const bool stopNow = p.stop;
    ReleaseSRWLockExclusive(&p.lock);
    if (stopNow) TerminateJobObject(job, 1);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    // Чтение построчно
    std::string pending;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(readEnd, buf, sizeof(buf), &got, nullptr) && got > 0) {
        pending.append(buf, got);
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            onLine(p, row, pending.substr(0, nl));
            pending.erase(0, nl + 1);
        }
    }
    if (!pending.empty()) onLine(p, row, pending);
    CloseHandle(readEnd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);

    AcquireSRWLockExclusive(&p.lock);
    *cancelled = p.running == nullptr;  // UI обнулил - значит, отменили
    p.running = nullptr;
    p.runningRow = 0;
    ReleaseSRWLockExclusive(&p.lock);
    CloseHandle(job);
    return static_cast<int>(code);
}

void OnUiMessage(void* data);

void PostLine(Plugin& p, int row, const std::string& line) {
    auto* m = new Msg{&p, row, false, 0, false, dl::ParseLine(line), line};
    // Строки без смысла для страницы не гоняем через поток окна (кроме проверки инструментов)
    if (m->event.type == dl::Event::Type::None && row != kToolsRow) {
        delete m;
        return;
    }
    p.host->post(p.host->ctx, &OnUiMessage, m);
}

void WorkerLoop(Plugin* pp) {
    Plugin& p = *pp;
    for (;;) {
        AcquireSRWLockExclusive(&p.lock);
        while (!p.stop && p.queue.empty()) SleepConditionVariableSRW(&p.wake, &p.lock, INFINITE, 0);
        if (p.stop) {
            ReleaseSRWLockExclusive(&p.lock);
            return;
        }
        Job job = p.queue.front();
        p.queue.pop_front();
        const std::wstring exe = p.ytdlp;
        dl::Options o;
        o.outDir = p.outDir;
        o.browser = p.browser;
        o.mp3 = !p.ffmpeg.empty();
        if (!p.ffmpeg.empty()) o.ffmpegDir = Utf8(p.ffmpeg.substr(0, p.ffmpeg.find_last_of(L"\\/")));
        ReleaseSRWLockExclusive(&p.lock);

        std::vector<std::string> args;
        if (job.kind == Job::Kind::Probe) args = {"--version"};
        else if (job.kind == Job::Kind::Update) args = {"-U"};
        else args = dl::BuildArgs(job.url, o);
        bool cancelled = false;
        int code = RunTool(p, exe, args, job.row, &PostLine, &cancelled);
        if (job.kind == Job::Kind::Probe && code == 0) {
            // Вторым шагом - есть ли в этой сборке Яндекс Музыка
            code = RunTool(p, exe, {"--list-extractors"}, job.row, &PostLine, &cancelled);
        }
        auto* m = new Msg{&p, job.row, true, code, cancelled, {}, job.kind == Job::Kind::Probe ? "probe" : ""};
        p.host->post(p.host->ctx, &OnUiMessage, m);
    }
}

void Enqueue(Plugin& p, const Job& j) {
    AcquireSRWLockExclusive(&p.lock);
    p.queue.push_back(j);
    ReleaseSRWLockExclusive(&p.lock);
    WakeConditionVariable(&p.wake);
}

// ---------------- страница (поток окна) ----------------

Job* FindJob(Plugin& p, int row) {
    for (auto& j : p.jobs)
        if (j.row == row) return &j;
    return nullptr;
}

void ShowHeader(Plugin& p) {
    const char* login = p.browser == "firefox" ? "Вход: Firefox"
                        : p.browser == "chrome" ? "Вход: Chrome"
                        : p.browser == "edge"   ? "Вход: Edge"
                                                : "Вход: нет";
    const PmAction head[3] = {{kActPaste, "Вставить ссылку"}, {kActFolder, "Папка"}, {kActLogin, login}};
    p.host->pageSetActions(p.host->ctx, kPage, head, 3);
}

void ShowToolsRow(Plugin& p) {
    PmPageRow r{};
    r.id = kToolsRow;
    r.progress = -1;
    std::string sub;
    if (p.ytdlp.empty()) {
        static const PmAction acts[3] = {{kActGetYtdlp, "Скачать"}, {kActBinFolder, "Папка bin"}, {kActRecheck, "Проверить"}};
        r.title = "Нужна программа yt-dlp";
        r.subtitle = "Положите yt-dlp.exe в папку bin плагина или выполните: winget install yt-dlp";
        r.state = PM_ROW_ERROR;
        r.actions = acts;
        r.actionCount = 3;
    } else if (p.ffmpeg.empty()) {
        static const PmAction acts[3] = {{kActGetFfmpeg, "Скачать"}, {kActBinFolder, "Папка bin"}, {kActRecheck, "Проверить"}};
        r.title = "Нужен ffmpeg для MP3";
        r.subtitle = "Без него треки останутся в m4a/opus - PlayerMoon их пока не играет. winget install Gyan.FFmpeg";
        r.state = PM_ROW_ERROR;
        r.actions = acts;
        r.actionCount = 3;
    } else {
        static const PmAction acts[1] = {{kActUpdate, "Обновить yt-dlp"}};
        static std::string title;
        title = p.version.empty() ? std::string("yt-dlp готов") : "yt-dlp " + p.version + " готов";
        sub = p.probing ? "Проверяю поддержку сервисов..."
              : p.yandex == 1 ? "YouTube Music, YouTube и Яндекс Музыка поддерживаются"
              : p.yandex == 0 ? "Яндекс Музыки нет в этой версии yt-dlp - обновите её"
                              : "YouTube Music и YouTube поддерживаются";
        r.title = title.c_str();
        r.subtitle = sub.c_str();
        r.state = PM_ROW_DONE;
        r.actions = acts;
        r.actionCount = 1;
    }
    p.host->pageSetRow(p.host->ctx, kPage, &r);
}

void ShowJob(Plugin& p, const Job& j) {
    std::string title = j.title.empty() ? j.url : (j.artist.empty() ? j.title : j.artist + " - " + j.title);
    std::string sub = dl::SourceName(j.source);
    if (j.items > 1) sub += ", трек " + std::to_string(std::max(1, j.item)) + " из " + std::to_string(j.items);
    if (j.state == Job::State::Error && !j.error.empty()) sub = j.error;
    std::string status;
    float progress = -1;
    int state = PM_ROW_NORMAL;
    static const PmAction kRunning[1] = {{kActCancel, "Отмена"}};
    static const PmAction kDone[3] = {{kActPlay, "Играть"}, {kActShow, "Показать"}, {kActRemove, "Убрать"}};
    static const PmAction kFailed[2] = {{kActRetry, "Повторить"}, {kActRemove, "Убрать"}};
    const PmAction* acts = kRunning;
    int count = 1;
    switch (j.state) {
        case Job::State::Waiting:
            status = "В очереди";
            state = PM_ROW_WAITING;
            break;
        case Job::State::Running: {
            state = PM_ROW_ACTIVE;
            const double one = j.percent < 0 ? 0 : j.percent / 100.0;
            progress = static_cast<float>(j.items > 1 ? (std::max(0, j.item - 1) + one) / j.items : one);
            if (j.converting) status = "Перекодирую в MP3";
            else if (j.percent < 0) status = "Подготовка";
            else {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.0f%%", j.percent);
                status = buf;
                if (!j.speed.empty()) status += "  " + dl::HumanSpeed(j.speed);
            }
            break;
        }
        case Job::State::Done:
            state = PM_ROW_DONE;
            status = j.files.size() > 1 ? "Готово: " + std::to_string(j.files.size()) + " треков"
                     : j.files.empty()  ? "Уже было скачано"
                                        : "Готово";
            acts = j.files.empty() ? kDone + 2 : kDone;
            count = j.files.empty() ? 1 : 3;
            break;
        case Job::State::Error:
        case Job::State::Cancelled:
            state = PM_ROW_ERROR;
            status = j.state == Job::State::Error ? "Ошибка" : "Отменено";
            acts = kFailed;
            count = 2;
            break;
    }
    PmPageRow r{j.row, title.c_str(), sub.c_str(), status.c_str(), progress, state, acts, count};
    p.host->pageSetRow(p.host->ctx, kPage, &r);
}

void CheckTools(Plugin& p) {
    p.ytdlp = FindTool(p.binDir, L"yt-dlp.exe");
    p.ffmpeg = FindTool(p.binDir, L"ffmpeg.exe");
    p.version.clear();
    p.yandex = -1;
    p.probing = !p.ytdlp.empty();
    ShowToolsRow(p);
    if (p.probing) {
        Job probe;
        probe.row = kToolsRow;
        probe.kind = Job::Kind::Probe;
        Enqueue(p, probe);
    }
}

void AddUrl(Plugin& p, const std::string& url) {
    Job j;
    j.row = p.nextRow++;
    j.url = url;
    j.source = dl::DetectSource(url);
    p.jobs.push_back(j);
    ShowJob(p, j);
    Enqueue(p, j);
}

void OnUiMessage(void* data) {
    Msg* m = static_cast<Msg*>(data);
    Plugin& p = *m->plugin;
    if (m->row == kToolsRow) {
        // Проверка или обновление yt-dlp
        if (!m->finished) {
            const std::string line = m->raw;
            if (p.version.empty() && !line.empty() && line[0] >= '0' && line[0] <= '9')
                p.version = line.substr(0, line.find_first_of("\r\n "));
            if (line.rfind("YandexMusic", 0) == 0) p.yandex = 1;
        } else {
            if (m->raw == "probe" && p.yandex < 0 && m->exitCode == 0) p.yandex = 0;
            p.probing = false;
            ShowToolsRow(p);
        }
        delete m;
        return;
    }
    Job* j = FindJob(p, m->row);
    if (!j) {
        delete m;
        return;
    }
    if (!m->finished) {
        const dl::Event& e = m->event;
        j->state = Job::State::Running;
        switch (e.type) {
            case dl::Event::Type::Item:
                j->title = e.title;
                j->artist = e.artist;
                j->item = e.item;
                j->items = e.items;
                j->percent = -1;
                j->converting = false;
                break;
            case dl::Event::Type::Progress:
                j->percent = e.percent;
                j->speed = e.speed;
                j->converting = false;
                break;
            case dl::Event::Type::Converting: j->converting = true; break;
            case dl::Event::Type::File: {
                j->files.push_back(e.text);
                const char* path = e.text.c_str();
                p.host->addFiles(p.host->ctx, &path, 1, 0);  // сразу в плейлист
                break;
            }
            case dl::Event::Type::Error: j->error = e.text; break;
            default: break;
        }
    } else if (j->state != Job::State::Cancelled) {
        if (m->cancelled) j->state = Job::State::Cancelled;
        else if (m->exitCode == 0) j->state = Job::State::Done;
        else {
            j->state = Job::State::Error;
            if (j->error.empty()) j->error = "yt-dlp завершился с кодом " + std::to_string(m->exitCode);
        }
        if (j->state == Job::State::Done) {
            const std::string name = j->title.empty() ? std::string("трек") : j->title;
            p.Toast(j->files.size() > 1 ? "Скачано треков: " + std::to_string(j->files.size()) : "Скачано: " + name);
        } else if (j->state == Job::State::Error) {
            p.Toast("Не удалось скачать: " + j->error);
        }
    }
    ShowJob(p, *j);
    delete m;
}

void Cancel(Plugin& p, Job& j) {
    if (j.state == Job::State::Waiting) {
        AcquireSRWLockExclusive(&p.lock);
        for (auto it = p.queue.begin(); it != p.queue.end(); ++it)
            if (it->row == j.row) {
                p.queue.erase(it);
                break;
            }
        ReleaseSRWLockExclusive(&p.lock);
    } else if (j.state == Job::State::Running) {
        AcquireSRWLockExclusive(&p.lock);
        if (p.runningRow == j.row && p.running) {
            TerminateJobObject(p.running, 1);
            p.running = nullptr;  // признак отмены для фонового потока
        }
        ReleaseSRWLockExclusive(&p.lock);
    }
    j.state = Job::State::Cancelled;
    ShowJob(p, j);
}

void OnPageAction(void* self, int pageId, int rowId, int actionId) {
    Plugin& p = *static_cast<Plugin*>(self);
    if (pageId != kPage) return;
    if (rowId == 0) {
        if (actionId == kActPaste) {
            char buf[8192];
            const int n = p.host->clipboardText(p.host->ctx, buf, sizeof(buf));
            const auto urls = dl::ExtractUrls(n > 0 ? std::string(buf, static_cast<size_t>(n)) : std::string());
            if (p.ytdlp.empty()) p.Toast("Сначала установите yt-dlp - строка внизу подскажет как");
            else if (urls.empty()) p.Toast("В буфере обмена нет ссылки - скопируйте её в браузере");
            else {
                // Напоминание о правах - один раз за запуск
                if (!p.warned) {
                    p.warned = true;
                    p.Toast("Скачивайте только то, на что у вас есть права");
                }
                for (const auto& u : urls) AddUrl(p, u);
            }
        } else if (actionId == kActFolder) {
            CreateDirectoryW(Wide(p.outDir).c_str(), nullptr);
            p.host->openPath(p.host->ctx, p.outDir.c_str());
        } else if (actionId == kActLogin) {
            // По кругу: нет -> Firefox -> Chrome -> Edge -> нет
            p.browser = p.browser.empty() ? "firefox" : p.browser == "firefox" ? "chrome" : p.browser == "chrome" ? "edge" : "";
            SaveSettings(p);
            ShowHeader(p);
            if (p.browser == "firefox")
                p.Toast("Вход как в Firefox: подписка YouTube Music или Яндекс Плюс берётся из браузера");
            else if (!p.browser.empty())
                p.Toast("Chrome и Edge шифруют cookies - yt-dlp может их не прочитать. Надёжнее Firefox");
            else
                p.Toast("Без входа: только то, что доступно без аккаунта");
        }
        return;
    }
    if (rowId == kToolsRow) {
        if (actionId == kActGetYtdlp) p.host->openPath(p.host->ctx, "https://github.com/yt-dlp/yt-dlp/releases/latest");
        if (actionId == kActGetFfmpeg) p.host->openPath(p.host->ctx, "https://ffmpeg.org/download.html");
        if (actionId == kActBinFolder) {
            CreateDirectoryW(p.binDir.c_str(), nullptr);
            p.host->openPath(p.host->ctx, Utf8(p.binDir).c_str());
        }
        if (actionId == kActRecheck) CheckTools(p);
        if (actionId == kActUpdate && !p.ytdlp.empty()) {
            Job up;
            up.row = kToolsRow;
            up.kind = Job::Kind::Update;
            p.probing = true;
            ShowToolsRow(p);
            Enqueue(p, up);
            p.Toast("Обновляю yt-dlp");
        }
        return;
    }
    Job* j = FindJob(p, rowId);
    if (!j) return;
    switch (actionId) {
        case kActCancel: Cancel(p, *j); break;
        case kActPlay:
            if (!j->files.empty()) {
                const char* path = j->files.front().c_str();
                p.host->addFiles(p.host->ctx, &path, 1, 1);
            }
            break;
        case kActShow:
            if (!j->files.empty()) {
                // Проводник с выделенным файлом
                const std::wstring arg = L"/select,\"" + Wide(j->files.front()) + L"\"";
                ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
            }
            break;
        case kActRemove:
            p.host->pageRemoveRow(p.host->ctx, kPage, j->row);
            p.jobs.erase(p.jobs.begin() + (j - p.jobs.data()));
            break;
        case kActRetry: {
            const std::string url = j->url;
            p.host->pageRemoveRow(p.host->ctx, kPage, j->row);
            p.jobs.erase(p.jobs.begin() + (j - p.jobs.data()));
            AddUrl(p, url);
            break;
        }
        default: break;
    }
}

void OnMenuItem(void* self, int id) {
    Plugin& p = *static_cast<Plugin*>(self);
    if (id == kMenuShow) p.host->pageShow(p.host->ctx, kPage);
    if (id == kMenuChangeFolder) {
        char buf[2048];
        if (p.host->pickFolder(p.host->ctx, "Куда сохранять музыку", buf, sizeof(buf)) > 0) {
            p.outDir = buf;
            SaveSettings(p);
            p.host->addLibraryFolder(p.host->ctx, p.outDir.c_str());
            p.Toast("Музыка будет сохраняться в " + p.outDir);
        }
    }
}

void Shutdown(void* self) {
    Plugin* p = static_cast<Plugin*>(self);
    AcquireSRWLockExclusive(&p->lock);
    p->stop = true;
    p->queue.clear();
    if (p->running) TerminateJobObject(p->running, 1);
    ReleaseSRWLockExclusive(&p->lock);
    WakeConditionVariable(&p->wake);
    if (p->worker.joinable()) p->worker.join();
    delete p;
}

std::wstring ModuleDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ModuleDir), &self);
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(self, buf, MAX_PATH * 2);
    std::wstring path(buf, n);
    return path.substr(0, path.find_last_of(L"\\/"));
}

}  // namespace

extern "C" PM_PLUGIN_EXPORT int PlayerMoonPluginInit(const PmHost* host, PmPlugin* plugin) {
    if (!host || host->apiVersion < 2) return 1;  // нужны страницы
    auto* p = new Plugin;
    p->host = host;
    p->pluginDir = ModuleDir();
    p->binDir = p->pluginDir + L"\\bin";
    p->dataDir = Wide(host->dataDir(host->ctx));
    LoadSettings(*p);
    CreateDirectoryW(Wide(p->outDir).c_str(), nullptr);
    // yt-dlp - программа на Python: просим вывод в UTF-8 независимо от кодовой страницы консоли
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
    SetEnvironmentVariableW(L"PYTHONUTF8", L"1");

    plugin->self = p;
    plugin->onPageAction = &OnPageAction;
    plugin->onMenuItem = &OnMenuItem;
    plugin->shutdown = &Shutdown;

    host->pageCreate(host->ctx, kPage, "Загрузки",
                     "Скопируйте в браузере ссылку на трек, альбом или плейлист YouTube Music, YouTube или "
                     "Яндекс Музыки и нажмите «Вставить ссылку»");
    ShowHeader(*p);
    host->addMenuItem(host->ctx, kMenuShow, "Загрузки");
    host->addMenuItem(host->ctx, kMenuChangeFolder, "Загрузки: выбрать папку...");
    host->addLibraryFolder(host->ctx, p->outDir.c_str());  // скачанное сразу видно в библиотеке
    p->worker = std::thread(&WorkerLoop, p);
    CheckTools(*p);
    return 0;
}
