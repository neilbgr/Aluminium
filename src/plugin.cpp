#include "plugin.hpp"
#include "PanelTheme.hpp"

Plugin *pluginInstance;

void init(Plugin *p) {
    pluginInstance = p;
    loadAluminiumThemeMode();
    p->addModel(modelAlSplitter);
}
