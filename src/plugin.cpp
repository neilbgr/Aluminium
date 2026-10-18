#include "plugin.hpp"
#include "PanelTheme.hpp"

Plugin *pluginInstance;

void init(Plugin *p) {
    pluginInstance = p;
    loadAluminiumThemeMode();
    p->addModel(modelZones);
    p->addModel(modelPads);
    p->addModel(modelPadX);
    p->addModel(modelFader);
    p->addModel(modelFaders2);
    p->addModel(modelFaders4);
    p->addModel(modelButtons);
}
