# Плагины PlayerMoon

Плагин - это папка внутри `plugins` рядом с `PlayerMoon.exe` (или в `%APPDATA%\PlayerMoon\plugins`, если
папка программы недоступна на запись). В папке:

```
plugins/
  my-plugin/
    plugin.json
    my-plugin.dll
```

## plugin.json

```json
{
  "id": "my-plugin",
  "name": "Мой плагин",
  "version": "1.0",
  "description": "Одна строка о том, что делает плагин",
  "author": "Имя",
  "library": "my-plugin",
  "apiVersion": 1
}
```

- `id` - латиница, цифры, `-` и `_`; должен быть уникальным.
- `library` - имя DLL без расширения, файл лежит в той же папке.
- `apiVersion` больше версии плеера - плагин показывается с ошибкой и не загружается.

## Код

Интерфейс на C: [sdk/playermoon_plugin.h](sdk/playermoon_plugin.h). DLL экспортирует одну функцию:

```c
#include "playermoon_plugin.h"

static void OnTrack(void* self, const PmTrack* track) {
    if (track) { /* track->title, track->artist, track->path - UTF-8 */ }
}

PM_PLUGIN_EXPORT int PlayerMoonPluginInit(const PmHost* host, PmPlugin* plugin) {
    plugin->self = NULL;
    plugin->onTrackChanged = OnTrack;
    host->addMenuItem(host->ctx, 1, "Пункт моего плагина");
    return 0; /* не 0 - отказ, плагин не загрузится */
}
```

Что плеер даёт плагину (`PmHost`): журнал (`plugins.log`), папку для данных, состояние воспроизведения,
команды (пуск, пауза, следующий, предыдущий), перемотку и пункты в подменю «Плагины».

События (`PmPlugin`): смена трека, пуск/пауза, нажатие своего пункта меню, выгрузка.

Правила:

- Всё вызывается в потоке окна. Долгую работу делайте в своём потоке и не вызывайте `PmHost` из него.
- Строки и `PmTrack` действительны только во время вызова - копируйте, если нужно сохранить.
- В `shutdown` освободите всё своё: после него DLL выгружается (выключение в разделе «Плагины»).

Сборка примера под Windows - вместе с плеером (`build.cmd`); отдельно, например:

```bash
x86_64-w64-mingw32-g++ -std=c++17 -shared -O2 -I. plugins/history/history.cpp -o history.dll -static -lshell32
```

## Безопасность

Плагин - обычная DLL внутри процесса плеера с правами пользователя. Поэтому новые плагины выключены, а при
включении плеер спрашивает подтверждение. Ставьте только плагины из источников, которым доверяете.
