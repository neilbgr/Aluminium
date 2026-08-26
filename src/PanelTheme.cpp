#include "PanelTheme.hpp"
#include <cstdio>

int aluminiumThemeMode = ALUMINIUM_THEME_FOLLOW_RACK;

void loadAluminiumThemeMode() {
    std::string path = asset::user("Aluminium.json");
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) {
        return;
    }

    json_error_t error;
    json_t* rootJ = json_loadf(f, 0, &error);
    std::fclose(f);
    if (!rootJ) {
        return;
    }

    if (json_t* modeJ = json_object_get(rootJ, "themeMode")) {
        int mode = json_integer_value(modeJ);
        if (mode >= 0 && mode < NUM_ALUMINIUM_THEME_MODES) {
            aluminiumThemeMode = mode;
        }
    }
    json_decref(rootJ);
}

void setAluminiumThemeMode(int mode) {
    aluminiumThemeMode = mode;

    json_t* rootJ = json_object();
    json_object_set_new(rootJ, "themeMode", json_integer(mode));
    std::string path = asset::user("Aluminium.json");
    FILE* f = std::fopen(path.c_str(), "w");
    if (f) {
        json_dumpf(rootJ, f, JSON_INDENT(2));
        std::fclose(f);
    }
    json_decref(rootJ);
}
