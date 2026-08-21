#pragma once
#include "plugin.hpp"
#include "PanelTheme.hpp"

using namespace rack;

// Aluminium's panel, shared by every module in this plugin:
//   1. a flat background color — pseudo-black or pseudo-white depending on
//      the Aluminium theme (see PanelTheme.hpp);
//   2. a light/dark silkscreen SVG on top, per module, for port outlines,
//      labels, and (going forward) the brushed-metal look itself, done in
//      the SVG artwork instead of a separate raster texture layer.
// Both live inside one FramebufferWidget, mirroring exactly how Rack's own
// app::SvgPanel wraps its SvgWidget (see src/app/SvgPanel.cpp) — cached to a
// texture and only re-rendered on zoom/scale change or when the Aluminium
// theme flips, not every frame.

struct AlPanelBackground : widget::Widget {
    AlPanelBackground(Vec size) {
        box.size = size;
    }

    void draw(const DrawArgs& args) override {
        NVGcolor bg = aluminiumDark() ? nvgRGB(0x1e, 0x1e, 0x1e) : nvgRGB(0xe8, 0xe8, 0xe8);

        nvgBeginPath(args.vg);
        nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
        nvgFillColor(args.vg, bg);
        nvgFill(args.vg);
    }
};

struct AlPanel : widget::Widget {
    widget::FramebufferWidget* fb;
    widget::SvgWidget* silkscreen;
    std::shared_ptr<window::Svg> lightSilkscreen;
    std::shared_ptr<window::Svg> darkSilkscreen;
    int appliedDark = -1;

    AlPanel(Vec size, std::shared_ptr<window::Svg> light, std::shared_ptr<window::Svg> dark)
        : lightSilkscreen(light), darkSilkscreen(dark) {
        box.size = size;

        fb = new widget::FramebufferWidget;
        fb->box.size = size;
        addChild(fb);

        fb->addChild(new AlPanelBackground(size));

        silkscreen = new widget::SvgWidget;
        fb->addChild(silkscreen);
    }

    void step() override {
        bool dark = aluminiumDark();
        int wanted = dark ? 1 : 0;
        if (wanted != appliedDark) {
            appliedDark = wanted;
            silkscreen->setSvg(dark ? darkSilkscreen : lightSilkscreen);
            fb->dirty = true;
        }

        // Small details (silkscreen lines/text) draw poorly at low DPI —
        // oversample the cached framebuffer, same trick as Rack's own
        // SvgPanel::step().
        fb->oversample = (APP->window->pixelRatio < 2.0) ? 2.0 : 1.0;

        Widget::step();
    }
};
