#include "plugin.hpp"
#include "PanelTheme.hpp"

Plugin *pluginInstance;

void init(Plugin *p) {
    pluginInstance = p;
    loadAluminiumThemeMode();
    p->addModel(modelAlSplitter);
    p->addModel(modelAlGate);
    p->addModel(modelAlVelocity);
    p->addModel(modelAlAftertouch);
    p->addModel(modelAlRetrigger);
}
