/*
 * PlayerMoon: интерфейс плагинов, версия 2 (версия 1 - подмножество, плагины v1 работают как раньше).
 *
 * Плагин - это DLL в своей папке внутри plugins/ рядом с PlayerMoon.exe (или в
 * %APPDATA%\PlayerMoon\plugins) плюс файл plugin.json с описанием:
 *
 *   {
 *     "id": "history",                      латиница, цифры, '-', '_'; совпадает с именем папки
 *     "name": "Журнал прослушиваний",
 *     "version": "1.0",
 *     "description": "Что делает плагин",
 *     "author": "Кто сделал",
 *     "library": "history",                 имя DLL без расширения
 *     "apiVersion": 1
 *   }
 *
 * DLL экспортирует функцию PlayerMoonPluginInit (см. PmPluginInit ниже). Интерфейс на C, чтобы
 * плагин можно было собрать любым компилятором; строки везде UTF-8.
 *
 * Правила:
 * - Все вызовы (в обе стороны) идут в потоке окна. Долгую работу плагин делает в своём потоке;
 *   из него можно звать только host->post (версия 2), остальное - уже внутри post.
 * - Указатели, которые передаёт плеер (PmTrack и строки в нём), живут только до возврата из вызова.
 * - Новые плагины по умолчанию выключены: пользователь включает их в разделе «Плагины».
 * - Плагин работает внутри процесса плеера с правами пользователя: ставьте только те, которым доверяете.
 */
#ifndef PLAYERMOON_PLUGIN_H
#define PLAYERMOON_PLUGIN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define PM_PLUGIN_EXPORT __declspec(dllexport)
#else
#define PM_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#define PM_PLUGIN_API_VERSION 2
#define PM_PLUGIN_INIT_NAME "PlayerMoonPluginInit"

/* Трек. path - полный путь к файлу; пустые теги - пустые строки, не NULL. */
typedef struct PmTrack {
    const char* path;
    const char* title; /* из тегов или имя файла */
    const char* artist;
    const char* album;
    double duration; /* секунды, 0 - неизвестна */
} PmTrack;

/* Состояние воспроизведения */
typedef struct PmPlayback {
    int playing;     /* 1 - играет */
    int index;       /* номер трека в плейлисте, -1 - нет трека */
    int count;       /* треков в плейлисте */
    double position; /* секунды */
    double duration; /* секунды */
    float volume;    /* 0..1 */
} PmPlayback;

/* Команды для PmHost.command */
enum {
    PM_CMD_TOGGLE = 1,
    PM_CMD_PLAY = 2,
    PM_CMD_PAUSE = 3,
    PM_CMD_NEXT = 4,
    PM_CMD_PREV = 5
};

/* ---------- Версия 2: страницы плагина в панели плеера ----------
 * Страница - своя вкладка рядом с «Очередь» и «Плагины». Плеер рисует её в стиле текущей темы:
 * кнопки в шапке, строки с названием, подписью, состоянием, полосой прогресса и кнопками справа.
 * Плагин описывает данные, плеер отвечает за вид и анимации. Нажатия приходят в onPageAction. */

/* Кнопка (в шапке страницы или в строке). id - свой номер плагина, label - подпись. */
typedef struct PmAction {
    int id;
    const char* label;
} PmAction;

/* Состояние строки - от него зависит цвет метки слева и полосы */
enum {
    PM_ROW_NORMAL = 0,
    PM_ROW_ACTIVE = 1, /* идёт работа: полоса прогресса цвета акцента */
    PM_ROW_DONE = 2,
    PM_ROW_ERROR = 3,
    PM_ROW_WAITING = 4 /* в очереди */
};

typedef struct PmPageRow {
    int id;               /* свой номер строки; строка с тем же id заменяется */
    const char* title;
    const char* subtitle; /* серым под названием */
    const char* status;   /* справа над кнопками: «42%  2.1 МБ/с» */
    float progress;       /* 0..1; меньше 0 - без полосы */
    int state;            /* PM_ROW_* */
    const PmAction* actions;
    int actionCount;      /* не больше 3 */
} PmPageRow;

/* Что плеер даёт плагину. ctx передавать первым аргументом.
 * Поля версии 2 идут после полей версии 1; при host->apiVersion < 2 их нет. */
typedef struct PmHost {
    uint32_t apiVersion;
    void* ctx;
    /* Строка в журнал плеера (plugins.log рядом с state.json) */
    void (*log)(void* ctx, const char* message);
    /* Папка для данных плагина; уже создана. Строка живёт, пока плагин загружен. */
    const char* (*dataDir)(void* ctx);
    void (*getPlayback)(void* ctx, PmPlayback* out);
    void (*command)(void* ctx, int command);
    void (*seek)(void* ctx, double seconds);
    /* Пункт в подменю «Плагины» контекстного меню. id - свой номер плагина (0..999).
       Возвращает 1, если пункт добавлен. Нажатие придёт в PmPlugin.onMenuItem. */
    int (*addMenuItem)(void* ctx, int id, const char* title);

    /* ----- версия 2 ----- */
    /* Выполнить fn(data) в потоке окна. ЕДИНСТВЕННАЯ функция, которую можно звать из своих потоков.
       Если плагин выгружается раньше, вызов отменяется (fn не вызывается). */
    void (*post)(void* ctx, void (*fn)(void* data), void* data);
    /* Страница: создать или переименовать. emptyText - что показать, когда строк нет. */
    int (*pageCreate)(void* ctx, int pageId, const char* title, const char* emptyText);
    /* Кнопки в шапке страницы (не больше 4) */
    void (*pageSetActions)(void* ctx, int pageId, const PmAction* actions, int count);
    /* Добавить строку (новые - сверху) или заменить строку с тем же id */
    void (*pageSetRow)(void* ctx, int pageId, const PmPageRow* row);
    void (*pageRemoveRow)(void* ctx, int pageId, int rowId);
    /* Показать свою страницу (открывает панель) */
    void (*pageShow)(void* ctx, int pageId);
    /* Короткое уведомление внизу окна */
    void (*toast)(void* ctx, const char* text);
    /* Текст из буфера обмена (UTF-8). Возвращает длину; 0 - пусто или не текст. */
    int (*clipboardText)(void* ctx, char* buffer, int size);
    /* Открыть файл, папку или адрес в программе по умолчанию */
    void (*openPath)(void* ctx, const char* pathOrUrl);
    /* Добавить файлы в конец плейлиста (play = 1 - сразу играть первый) */
    void (*addFiles)(void* ctx, const char* const* paths, int count, int play);
    /* Добавить папку в библиотеку (плеер будет следить за ней) */
    void (*addLibraryFolder)(void* ctx, const char* path);
    /* Диалог выбора папки. Возвращает длину пути или 0 при отмене. */
    int (*pickFolder)(void* ctx, const char* title, char* buffer, int size);
} PmHost;

/* Что плагин даёт плееру. Любой обработчик может быть NULL. */
typedef struct PmPlugin {
    void* self; /* передаётся первым аргументом в обработчики */
    /* Начался другой трек; track == NULL - плейлист пуст */
    void (*onTrackChanged)(void* self, const PmTrack* track);
    /* Пуск (1) или пауза (0) */
    void (*onPlayState)(void* self, int playing);
    void (*onMenuItem)(void* self, int id);
    /* Перед выгрузкой: остановить свои потоки (дождаться их!) и освободить всё своё */
    void (*shutdown)(void* self);

    /* ----- версия 2 ----- */
    /* Нажата кнопка на странице: rowId = 0 - кнопка шапки */
    void (*onPageAction)(void* self, int pageId, int rowId, int actionId);
} PmPlugin;

/* Точка входа. Заполнить plugin и вернуть 0; другое значение - отказ (плагин не загрузится).
   host->apiVersion можно сравнить с PM_PLUGIN_API_VERSION. */
typedef int (*PmPluginInit)(const PmHost* host, PmPlugin* plugin);

#ifdef __cplusplus
}
#endif

#endif /* PLAYERMOON_PLUGIN_H */
