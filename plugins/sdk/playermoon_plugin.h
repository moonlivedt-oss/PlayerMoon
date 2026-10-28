/*
 * PlayerMoon: интерфейс плагинов, версия 1.
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
 * - Все вызовы (в обе стороны) идут в потоке окна. Долгую работу плагин делает в своём потоке,
 *   а функции PmHost из чужого потока не вызывает.
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

#define PM_PLUGIN_API_VERSION 1
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

/* Что плеер даёт плагину. ctx передавать первым аргументом. */
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
} PmHost;

/* Что плагин даёт плееру. Любой обработчик может быть NULL. */
typedef struct PmPlugin {
    void* self; /* передаётся первым аргументом в обработчики */
    /* Начался другой трек; track == NULL - плейлист пуст */
    void (*onTrackChanged)(void* self, const PmTrack* track);
    /* Пуск (1) или пауза (0) */
    void (*onPlayState)(void* self, int playing);
    void (*onMenuItem)(void* self, int id);
    /* Перед выгрузкой: освободить всё своё */
    void (*shutdown)(void* self);
} PmPlugin;

/* Точка входа. Заполнить plugin и вернуть 0; другое значение - отказ (плагин не загрузится).
   host->apiVersion можно сравнить с PM_PLUGIN_API_VERSION. */
typedef int (*PmPluginInit)(const PmHost* host, PmPlugin* plugin);

#ifdef __cplusplus
}
#endif

#endif /* PLAYERMOON_PLUGIN_H */
