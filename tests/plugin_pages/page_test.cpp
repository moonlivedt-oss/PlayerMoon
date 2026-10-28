// Тестовый плагин версии 2: страница, кнопки, строки, post из своего потока.
// Собирается только для тестов (tests/test_plugins.cpp).
#include <thread>

#include "plugins/sdk/playermoon_plugin.h"

namespace {

struct State {
    const PmHost* host = nullptr;
    std::thread worker;
};

void AddRow(void* data) {
    auto* s = static_cast<State*>(data);
    PmAction act{7, "Убрать"};
    PmPageRow row{42, "Из потока", "подпись", "50%", 0.5f, PM_ROW_ACTIVE, &act, 1};
    s->host->pageSetRow(s->host->ctx, 1, &row);
}

void OnAction(void* self, int pageId, int rowId, int actionId) {
    auto* s = static_cast<State*>(self);
    if (pageId != 1) return;
    if (rowId == 0 && actionId == 1) {
        // Работа в своём потоке, результат - через post в поток окна
        if (s->worker.joinable()) s->worker.join();
        s->worker = std::thread([s] { s->host->post(s->host->ctx, &AddRow, s); });
    } else if (rowId == 42 && actionId == 7) {
        s->host->pageRemoveRow(s->host->ctx, 1, 42);
        s->host->toast(s->host->ctx, "убрано");
    }
}

void Shutdown(void* self) {
    auto* s = static_cast<State*>(self);
    if (s->worker.joinable()) s->worker.join();
    delete s;
}

}  // namespace

extern "C" PM_PLUGIN_EXPORT int PlayerMoonPluginInit(const PmHost* host, PmPlugin* plugin) {
    if (host->apiVersion < 2) return 1;
    auto* s = new State;
    s->host = host;
    plugin->self = s;
    plugin->onPageAction = &OnAction;
    plugin->shutdown = &Shutdown;
    host->pageCreate(host->ctx, 1, "Тест", "Пусто");
    PmAction head[2] = {{1, "Добавить"}, {2, "Ещё"}};
    host->pageSetActions(host->ctx, 1, head, 2);
    char buf[64];
    const int n = host->clipboardText(host->ctx, buf, sizeof(buf));
    PmPageRow row{1, n > 0 ? buf : "нет буфера", "", "", -1, PM_ROW_DONE, nullptr, 0};
    host->pageSetRow(host->ctx, 1, &row);
    return 0;
}
