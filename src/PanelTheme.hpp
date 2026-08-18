#pragma once
#include <rack.hpp>
#include "plugin.hpp"

using namespace rack;

// Aluminium-wide panel theme, deliberately independent of Rack's own "Dark
// panels" setting (rack::settings::preferDarkPanels). Confirmed by reading
// Rack's own app/SvgScrew.hpp and app/SvgPort.hpp: the stock ThemedSvgScrew/
// ThemedSvgPort classes hardcode reading that one global in their step(), so
// there's no way to make stock Themed* components (ThemedScrew,
// ThemedPJ301MPort, ...) follow a different, per-plugin choice. Aluminium
// instead offers Follow Rack / Light / Dark, and ships its own screw/port
// component wrappers (AlScrew/AlPort below) that resolve against this
// module's choice instead, so they always match our own brushed-aluminium
// panel artwork exactly — even in the "Light"/"Dark" override cases where it
// may briefly disagree with Rack's own global.
//
// Persisted process-wide (like BogaudioModules' Skins class, in its own file
// under Rack's user asset dir) rather than per-module/per-patch, since it's
// meant to be one consistent look across every open Aluminium module.

enum AluminiumThemeMode {
    ALUMINIUM_THEME_FOLLOW_RACK,
    ALUMINIUM_THEME_LIGHT,
    ALUMINIUM_THEME_DARK,
    NUM_ALUMINIUM_THEME_MODES
};

extern int aluminiumThemeMode;

inline bool aluminiumDark() {
    switch (aluminiumThemeMode) {
        case ALUMINIUM_THEME_LIGHT: return false;
        case ALUMINIUM_THEME_DARK: return true;
        default: return settings::preferDarkPanels;
    }
}

void loadAluminiumThemeMode();
void setAluminiumThemeMode(int mode);

// Mirrors app::ThemedSvgScrew / app::ThemedSvgPort exactly (same shape,
// confirmed against Rack's source), but resolves against aluminiumDark()
// instead of settings::preferDarkPanels directly.
struct AlScrew : app::SvgScrew {
    std::shared_ptr<window::Svg> lightSvg;
    std::shared_ptr<window::Svg> darkSvg;

    void setSvg(std::shared_ptr<window::Svg> light, std::shared_ptr<window::Svg> dark) {
        lightSvg = light;
        darkSvg = dark;
        SvgScrew::setSvg(aluminiumDark() ? darkSvg : lightSvg);
    }

    void step() override {
        SvgScrew::setSvg(aluminiumDark() ? darkSvg : lightSvg);
        SvgScrew::step();
    }
};

struct AlPort : app::SvgPort {
    std::shared_ptr<window::Svg> lightSvg;
    std::shared_ptr<window::Svg> darkSvg;

    void setSvg(std::shared_ptr<window::Svg> light, std::shared_ptr<window::Svg> dark) {
        lightSvg = light;
        darkSvg = dark;
        SvgPort::setSvg(aluminiumDark() ? darkSvg : lightSvg);
    }

    void step() override {
        SvgPort::setSvg(aluminiumDark() ? darkSvg : lightSvg);
        SvgPort::step();
    }
};

// Concrete components wired to Aluminium's own artwork. Currently placeholder
// copies of Rack's stock screw/port SVGs (res/Screw_*.svg, res/Port_*.svg) —
// swap those files for the brushed-aluminium versions when ready, no code
// change needed.
struct AlScrewComponent : AlScrew {
    AlScrewComponent() {
        setSvg(Svg::load(asset::plugin(pluginInstance, "res/Screw_Light.svg")),
               Svg::load(asset::plugin(pluginInstance, "res/Screw_Dark.svg")));
    }
};

struct AlPortComponent : AlPort {
    AlPortComponent() {
        setSvg(Svg::load(asset::plugin(pluginInstance, "res/Port_Light.svg")),
               Svg::load(asset::plugin(pluginInstance, "res/Port_Dark.svg")));
    }
};

// Appends the "Aluminium theme" submenu (Follow Rack/Light/Dark) shared by
// every Aluminium module's right-click menu.
inline void appendAluminiumThemeMenu(Menu* menu) {
    menu->addChild(new MenuSeparator);
    menu->addChild(createIndexSubmenuItem("Aluminium theme",
        {"Follow Rack", "Light", "Dark"},
        [=]() { return aluminiumThemeMode; },
        [=](int mode) { setAluminiumThemeMode(mode); }
    ));
}
