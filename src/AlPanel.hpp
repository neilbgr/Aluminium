#pragma once
#include "plugin.hpp"
#include "PanelTheme.hpp"

using namespace rack;

// Aluminium's 3-layer panel, shared by every module in this plugin:
//   1. a flat background color — pseudo-black or pseudo-white depending on
//      the Aluminium theme (see PanelTheme.hpp), not pure #000/#fff so the
//      brushed texture never crushes to full black/white;
//   2. the shared brushed-metal grain (res/Brushed.png) tiled via
//      NVG_IMAGE_REPEATX/REPEATY (baked into window::Image::load already —
//      confirmed in Rack's own src/window/Window.cpp) — one texture reused
//      across every Aluminium module regardless of its HP width, since its
//      black/white pixels carry their own alpha and read correctly over
//      either background color;
//   3. a light/dark silkscreen SVG on top, per module, for port outlines
//      and labels.
// All three live inside one FramebufferWidget, mirroring exactly how Rack's
// own app::SvgPanel wraps its SvgWidget (see src/app/SvgPanel.cpp) — cached
// to a texture and only re-rendered on zoom/scale change or when the
// Aluminium theme flips, not every frame.

struct AlPanelBackground : widget::Widget {
    std::shared_ptr<window::Image> brushed;

    AlPanelBackground(Vec size) {
        box.size = size;
        brushed = APP->window->loadImage(asset::plugin(pluginInstance, "res/Brushed.png"));
    }

    // Rack's own Context::~Context() (src/context.cpp) deletes `window`
    // (tearing down the GL/NanoVG context) BEFORE deleting `scene` (which
    // owns this widget) — confirmed by reading that destructor. Releasing
    // `brushed` only in our own destructor therefore calls nvgDeleteImage()
    // on an already-dead context and crashes (SIGSEGV, seen in a real user
    // log). Mirror FramebufferWidget::onContextDestroy/onContextCreate
    // instead: release/reload GPU-backed resources on these events, which
    // fire before/after the context actually changes, not on C++ teardown
    // order.
    void onContextDestroy(const ContextDestroyEvent& e) override {
        brushed = nullptr;
        Widget::onContextDestroy(e);
    }

    void onContextCreate(const ContextCreateEvent& e) override {
        brushed = APP->window->loadImage(asset::plugin(pluginInstance, "res/Brushed.png"));
        Widget::onContextCreate(e);
    }

    void draw(const DrawArgs& args) override {
        NVGcolor bg = aluminiumDark() ? nvgRGB(0x1e, 0x1e, 0x1e) : nvgRGB(0xe8, 0xe8, 0xe8);

        nvgBeginPath(args.vg);
        nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
        nvgFillColor(args.vg, bg);
        nvgFill(args.vg);

        if (brushed && brushed->handle >= 0) {
            // Tile size is independent of this panel's own width on purpose
            // (see above) — ~40mm reads as a plausible brushed-metal grain
            // scale regardless of module HP.
            float tile = mm2px(40.f);
            NVGpaint paint = nvgImagePattern(args.vg, 0.f, 0.f, tile, tile, 0.f, brushed->handle, 1.f);
            nvgBeginPath(args.vg);
            nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
            nvgFillPaint(args.vg, paint);
            nvgFill(args.vg);
        }
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
