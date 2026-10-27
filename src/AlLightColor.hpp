#pragma once
#include "plugin.hpp"

using namespace rack;

// Per-control light color (Fader cap band, Buttons button), picked from the
// control's own right-click menu and saved with the patch. Rack's own color
// scheme (componentlibrary.hpp), white first as the default. Only the
// widget uses the colors: it sets its light's base color in step(), so the
// module keeps driving a plain brightness.

static const int AL_NUM_LIGHT_COLORS = 8;

static const char* AL_LIGHT_COLOR_NAMES[AL_NUM_LIGHT_COLORS] = {
    "White", "Red", "Orange", "Yellow", "Green", "Cyan", "Blue", "Purple",
};

inline NVGcolor alLightColor(int index) {
    static const NVGcolor colors[AL_NUM_LIGHT_COLORS] = {
        componentlibrary::SCHEME_WHITE,
        componentlibrary::SCHEME_RED,
        componentlibrary::SCHEME_ORANGE,
        componentlibrary::SCHEME_YELLOW,
        componentlibrary::SCHEME_GREEN,
        componentlibrary::SCHEME_CYAN,
        componentlibrary::SCHEME_BLUE,
        componentlibrary::SCHEME_PURPLE,
    };
    return colors[clamp(index, 0, AL_NUM_LIGHT_COLORS - 1)];
}

inline int alLightColorFromJson(json_t* j) {
    return j ? clamp((int) json_integer_value(j), 0, AL_NUM_LIGHT_COLORS - 1) : 0;
}

#ifndef HEADLESS
inline void alAppendLightColorMenu(Menu* menu, std::function<int()> get, std::function<void(int)> set,
                                   const std::string& text = "Light color") {
    menu->addChild(createIndexSubmenuItem(text,
        std::vector<std::string>(AL_LIGHT_COLOR_NAMES, AL_LIGHT_COLOR_NAMES + AL_NUM_LIGHT_COLORS),
        get, set
    ));
}

// Points `light` at the chosen color (a 1-color ModuleLightWidget).
inline void alApplyLightColor(app::ModuleLightWidget* light, int index) {
    NVGcolor c = alLightColor(index);
    if (light && !light->baseColors.empty() && !color::isEqual(light->baseColors[0], c)) {
        light->baseColors[0] = c;
    }
}
#endif
