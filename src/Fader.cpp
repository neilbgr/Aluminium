#include "plugin.hpp"
#include "PanelTheme.hpp"
#include "AlPanel.hpp"
#include "FaderTakeover.hpp"
#include "AlLabel.hpp"
#include "AlLightColor.hpp"

// "Digital mixing desk" faders: each slider's position is its output voltage
// (in a selectable unipolar or bipolar range), and its CV input — e.g. a
// MIDI-CV CC output — moves the slider itself whenever it changes, so the two
// never fight: whichever moved last wins (see FaderTakeover.hpp). Labels are
// purely cosmetic, same as PadX's.
//
// Fader, Faders2 and Faders4 are the same module (FaderBank) with 1, 2 or 4
// identical 3HP columns; each column has its own label, range and takeover
// threshold, set from its slider's right-click menu.

struct FaderRange {
    float min;
    float max;
    const char* name;
};

static const FaderRange FADER_RANGES[] = {
    {0.f, 10.f, "0 V to 10 V"},
    {0.f, 5.f, "0 V to 5 V"},
    {0.f, 1.f, "0 V to 1 V"},
    {-10.f, 10.f, "-10 V to 10 V"},
    {-5.f, 5.f, "-5 V to 5 V"},
    {-1.f, 1.f, "-1 V to 1 V"},
};
static const int NUM_FADER_RANGES = sizeof(FADER_RANGES) / sizeof(FADER_RANGES[0]);

// How far the input has to move (as a fraction of the selected range) before
// it takes the fader back from the slider. A 7-bit MIDI CC step is 1/127 of
// the range (~0.79%), so every option but the last still catches a single
// CC step.
struct FaderThreshold {
    float fraction;
    const char* name;
};

static const FaderThreshold FADER_THRESHOLDS[] = {
    {0.f, "Any change"},
    {0.001f, "0.1% of range"},
    {0.004f, "0.4% of range (half a MIDI CC step)"},
    {0.02f, "2% of range"},
};
static const int NUM_FADER_THRESHOLDS = sizeof(FADER_THRESHOLDS) / sizeof(FADER_THRESHOLDS[0]);

static const int MAX_FADERS = 4;

// Everything one column owns. Ids are per column: param/input/output/light
// `i` all belong to fader `i` (so a 1-fader bank keeps Fader's original ids).
struct FaderChannel {
    int rangeIndex = 0;
    int thresholdIndex = 1;
    FaderTakeover takeover;
    // Set by the slider widget (UI thread) while the mouse holds it.
    bool dragging = false;

    AlLabel label;
    // Cap band color, index into AL_LIGHT_COLOR_NAMES (AlLightColor.hpp).
    int colorIndex = 0;
};

struct FaderBank : Module {
    int numFaders;
    FaderChannel channels[MAX_FADERS];
    // A CV fader doesn't need audio-rate updates: 32 samples is 0.7 ms at
    // 48 kHz, and an output keeps its voltage between writes.
    dsp::ClockDivider processDivider;

    // Shows a slider in volts of its own range (stored normalized, 0..1, so
    // a range change doesn't need to rescale the param itself).
    struct FaderQuantity : ParamQuantity {
        float getDisplayValue() override {
            FaderBank* bank = dynamic_cast<FaderBank*>(module);
            if (!bank) {
                return ParamQuantity::getDisplayValue();
            }
            const FaderRange& r = bank->range(paramId);
            return r.min + getValue() * (r.max - r.min);
        }

        void setDisplayValue(float displayValue) override {
            FaderBank* bank = dynamic_cast<FaderBank*>(module);
            if (!bank) {
                ParamQuantity::setDisplayValue(displayValue);
                return;
            }
            const FaderRange& r = bank->range(paramId);
            setValue((displayValue - r.min) / (r.max - r.min));
        }
    };

    explicit FaderBank(int faders) : numFaders(faders) {
        config(numFaders, numFaders, numFaders, numFaders);
        for (int i = 0; i < numFaders; i++) {
            configParam<FaderQuantity>(i, 0.f, 1.f, 0.f, defaultName(i), " V")->displayPrecision = 3;
            configInput(i, defaultName(i));
            configOutput(i, defaultName(i));
            configBypass(i, i);
        }
        processDivider.setDivision(32);
        updatePortNames();
    }

    std::string defaultName(int i) const {
        return numFaders == 1 ? "Fader" : string::f("Fader %d", i + 1);
    }

    const FaderRange& range(int i) const {
        return FADER_RANGES[channels[i].rangeIndex];
    }

    void setRange(int i, int index) {
        channels[i].rangeIndex = clamp(index, 0, NUM_FADER_RANGES - 1);
        // Double-click reset goes to 0V: the bottom of a unipolar range, the
        // middle of a bipolar one.
        if (ParamQuantity* pq = getParamQuantity(i)) {
            pq->defaultValue = range(i).min < 0.f ? 0.5f : 0.f;
        }
    }

    void setThreshold(int i, int index) {
        channels[i].thresholdIndex = clamp(index, 0, NUM_FADER_THRESHOLDS - 1);
    }

    // Same as PadX: ports and params are configured once, before the labels
    // have their final value, so their tooltip names are kept in sync by hand.
    void updatePortNames() {
        for (int i = 0; i < numFaders; i++) {
            std::string base = channels[i].label.text.empty() ? defaultName(i) : channels[i].label.text;
            if (ParamQuantity* pq = getParamQuantity(i)) {
                pq->name = base;
            }
            if (PortInfo* info = getInputInfo(i)) {
                info->name = base;
            }
            if (PortInfo* info = getOutputInfo(i)) {
                info->name = base;
            }
        }
    }

    void onReset() override {
        for (int i = 0; i < numFaders; i++) {
            channels[i].takeover.reset();
        }
    }

    void process(const ProcessArgs&) override {
        if (!processDivider.process()) {
            return;
        }
        for (int i = 0; i < numFaders; i++) {
            FaderChannel& ch = channels[i];
            const FaderRange& r = range(i);
            float span = r.max - r.min;
            float slider = params[i].getValue();
            float applied = ch.takeover.step(slider, ch.dragging,
                inputs[i].isConnected(), inputs[i].getVoltage(),
                r.min, r.max, FADER_THRESHOLDS[ch.thresholdIndex].fraction * span);
            if (applied != slider) {
                params[i].setValue(applied);
            }
            outputs[i].setVoltage(r.min + applied * span);
            // Medium intensity: a 1 mm band at full brightness glares.
            lights[i].setBrightness(0.6f);
        }
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_t* fadersJ = json_array();
        for (int i = 0; i < numFaders; i++) {
            const FaderChannel& ch = channels[i];
            json_t* faderJ = json_object();
            ch.label.toJson(faderJ);
            json_object_set_new(faderJ, "color", json_integer(ch.colorIndex));
            json_object_set_new(faderJ, "range", json_integer(ch.rangeIndex));
            json_object_set_new(faderJ, "threshold", json_integer(ch.thresholdIndex));
            json_array_append_new(fadersJ, faderJ);
        }
        json_object_set_new(rootJ, "faders", fadersJ);
        return rootJ;
    }

    void faderFromJson(int i, json_t* faderJ) {
        if (channels[i].label.fromJson(faderJ)) {
            updatePortNames();
        }
        channels[i].colorIndex = alLightColorFromJson(json_object_get(faderJ, "color"));
        if (json_t* j = json_object_get(faderJ, "range")) {
            setRange(i, json_integer_value(j));
        }
        if (json_t* j = json_object_get(faderJ, "threshold")) {
            setThreshold(i, json_integer_value(j));
        }
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* fadersJ = json_object_get(rootJ, "faders");
        if (fadersJ && json_is_array(fadersJ)) {
            for (int i = 0; i < numFaders && i < (int) json_array_size(fadersJ); i++) {
                faderFromJson(i, json_array_get(fadersJ, i));
            }
        }
        else {
            // Single-fader patches saved before banks existed kept the
            // fields at the root.
            faderFromJson(0, rootJ);
        }
        onReset();
    }
};

struct Fader : FaderBank {
    Fader() : FaderBank(1) {}
};

struct Faders2 : FaderBank {
    Faders2() : FaderBank(2) {}
};

struct Faders4 : FaderBank {
    Faders4() : FaderBank(4) {}
};

#ifndef HEADLESS
// The Range / Input takeover threshold / Light color submenus of fader `i`, shared by its
// slider's right-click menu and (on the single Fader) the module's own.
static void appendFaderMenu(Menu* menu, FaderBank* bank, int i) {
    std::vector<std::string> rangeNames;
    for (int k = 0; k < NUM_FADER_RANGES; k++) {
        rangeNames.push_back(FADER_RANGES[k].name);
    }
    std::vector<std::string> thresholdNames;
    for (int k = 0; k < NUM_FADER_THRESHOLDS; k++) {
        thresholdNames.push_back(FADER_THRESHOLDS[k].name);
    }
    menu->addChild(createIndexSubmenuItem("Range", rangeNames,
        [=]() { return bank->channels[i].rangeIndex; },
        [=](int k) { bank->setRange(i, k); }
    ));
    menu->addChild(createIndexSubmenuItem("Input takeover threshold", thresholdNames,
        [=]() { return bank->channels[i].thresholdIndex; },
        [=](int k) { bank->setThreshold(i, k); }
    ));
    alAppendLightColorMenu(menu,
        [=]() { return bank->channels[i].colorIndex; },
        [=](int k) { bank->channels[i].colorIndex = k; }
    );
}

// Theme-independent, like Aluminium's ports. 76 mm track.
struct AlFaderSliderBase : app::SvgSlider {
    AlFaderSliderBase() {
        setBackgroundSvg(Svg::load(asset::plugin(pluginInstance, "res/Fader_Track.svg")));
        setHandleSvg(Svg::load(asset::plugin(pluginInstance, "res/Fader_Handle.svg")));
        // Handle center travels between the track's slot ends (1 mm and
        // 75 mm from its top), bottom = 0.
        setHandlePosCentered(mm2px(Vec(5.5f, 72.f)), mm2px(Vec(5.5f, 4.f)));
    }

    FaderBank* bank() {
        ParamQuantity* pq = getParamQuantity();
        return pq ? dynamic_cast<FaderBank*>(pq->module) : nullptr;
    }

    void setDragging(bool held) {
        if (FaderBank* b = bank()) {
            b->channels[paramId].dragging = held;
        }
    }

    void onDragStart(const DragStartEvent& e) override {
        SvgSlider::onDragStart(e);
        if (e.button == GLFW_MOUSE_BUTTON_LEFT) {
            setDragging(true);
        }
    }

    void onDragEnd(const DragEndEvent& e) override {
        SvgSlider::onDragEnd(e);
        if (e.button == GLFW_MOUSE_BUTTON_LEFT) {
            setDragging(false);
        }
    }

    void appendContextMenu(Menu* menu) override {
        if (FaderBank* b = bank()) {
            menu->addChild(new MenuSeparator);
            appendFaderMenu(menu, b, paramId);
        }
    }
};

// Lit band across the cap, kept centered on the handle by Rack's own
// LightSlider (componentlibrary.hpp). 1 mm tall (~3 px at 100% zoom, still
// ~1 px at 33%): a thinner, sub-pixel line gets antialiased over 1 or 2
// pixel rows depending on where the handle sits, so its brightness visibly
// changed along the travel — any thickness does eventually at a deep enough
// zoom-out, this one just pushes that past where it's noticeable.
struct AlFaderHandleLight : RectangleLight<WhiteLight> {
    AlFaderHandleLight() {
        box.size = mm2px(Vec(8.4f, 1.f));
    }
};

struct AlFaderSlider : LightSlider<AlFaderSliderBase, AlFaderHandleLight> {
    void step() override {
        LightSlider<AlFaderSliderBase, AlFaderHandleLight>::step();
        if (FaderBank* b = bank()) {
            alApplyLightColor(light, b->channels[paramId].colorIndex);
        }
    }
};

struct FaderBankWidget : ModuleWidget {
    int numFaders;

    FaderBankWidget(FaderBank* module, int faders, const char* silkName) : numFaders(faders) {
        setModule(module);

        // One 3HP column per fader.
        const float columnWidth = 15.24f;
        const float panelWidth = columnWidth * numFaders;

        setPanel(new AlPanel(mm2px(Vec(panelWidth, 128.5f)),
            Svg::load(asset::plugin(pluginInstance, string::f("res/%s_Silk_Light.svg", silkName))),
            Svg::load(asset::plugin(pluginInstance, string::f("res/%s_Silk_Dark.svg", silkName)))));

        addChild(createWidget<AlScrewComponent>(Vec(faders > 1 ? RACK_GRID_WIDTH : 0, 0)));
        addChild(createWidget<AlScrewComponent>(Vec(box.size.x - (faders > 1 ? 2 : 1) * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<AlScrewComponent>(Vec(faders > 1 ? RACK_GRID_WIDTH : 0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<AlScrewComponent>(Vec(box.size.x - (faders > 1 ? 2 : 1) * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        for (int i = 0; i < numFaders; i++) {
            const float centerX = columnWidth * (i + 0.5f);

            // Same position as PadX's label, as wide as a 3HP column
            // allows: ~6 characters.
            const float labelCenterY = 14.f, labelWidth = 13.2f;
            alAddLabelField(this, Vec(centerX, labelCenterY), labelWidth,
                module ? &module->channels[i].label : nullptr,
                [=]() { module->updatePortNames(); });

            // Between the label and the ports: 20.5 to 96.5 mm.
            addParam(createLightParamCentered<AlFaderSlider>(mm2px(Vec(centerX, 58.5f)), module, i, i));

            // Same Y as the last two port rows of Zones (row7/row8) and Pads.
            const float inputY = 103.f, outputY = 114.f;
            addInput(createInputCentered<AlPortComponentIn>(mm2px(Vec(centerX, inputY)), module, i));
            addOutput(createOutputCentered<AlPortComponentOut>(mm2px(Vec(centerX, outputY)), module, i));
        }
    }

    void appendContextMenu(Menu* menu) override {
        // The single Fader keeps its settings in the module menu too; a bank
        // has one set per fader, on each slider's own right-click menu.
        FaderBank* mod = dynamic_cast<FaderBank*>(module);
        if (mod && numFaders == 1) {
            menu->addChild(new MenuSeparator);
            appendFaderMenu(menu, mod, 0);
        }
        appendAluminiumThemeMenu(menu);
    }
};
#else
struct FaderBankWidget : ModuleWidget {
    FaderBankWidget(FaderBank* module, int faders, const char*) {
        setModule(module);
        for (int i = 0; i < faders; i++) {
            addParam(createParam<VCVSlider>({}, module, i));
            addInput(createInput<PJ301MPort>({}, module, i));
            addOutput(createOutput<PJ301MPort>({}, module, i));
        }
    }
};
#endif

struct FaderWidget : FaderBankWidget {
    explicit FaderWidget(Fader* module) : FaderBankWidget(module, 1, "Fader") {}
};

struct Faders2Widget : FaderBankWidget {
    explicit Faders2Widget(Faders2* module) : FaderBankWidget(module, 2, "Faders2") {}
};

struct Faders4Widget : FaderBankWidget {
    explicit Faders4Widget(Faders4* module) : FaderBankWidget(module, 4, "Faders4") {}
};

Model* modelFader = createModel<Fader, FaderWidget>("Fader");
Model* modelFaders2 = createModel<Faders2, Faders2Widget>("Faders2");
Model* modelFaders4 = createModel<Faders4, Faders4Widget>("Faders4");
