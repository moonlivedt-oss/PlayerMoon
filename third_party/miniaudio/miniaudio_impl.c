/* Единица компиляции с реализацией miniaudio.
   Нужны только низкоуровневые части: устройство вывода и декодеры MP3/FLAC/WAV.
   Движок, граф узлов, менеджер ресурсов, кодирование и генераторы не собираются. */
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
