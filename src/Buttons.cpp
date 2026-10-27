#include "plugin.hpp"
#include "PanelTheme.hpp"
#include "AlPanel.hpp"
#include "AlLabel.hpp"
#include "AlLightColor.hpp"
#include "ButtonLogic.hpp"

// Two lit buttons stacked in one 3HP panel, each with its own label, mode,
// gate/trigger input and output. The input acts as a remote press of its
// button (see ButtonLogic.hpp for the three modes); the mode is picked with
// a 3-position switch under the button. Labels are purely cosmetic, same as
// PadX's.

static const int NUM_BUTTONS = 2;

// The buttons themselves run every sample (unlike Fader): a 1-sample trigger
// must not be missed. Only the lights are refreshed once every
// BUTTON_LIGHT_DIVISION samples (0.33 ms at 48 kHz, far below a UI frame).
static const int BUTTON_LIGHT_DIVISION = 16;

static const char* BUTTON_MODE_NAMES[NUM_BUTTON_MODES] = {
    "Momentary",
    "Latch",
    "Toggle trigger",
};

// Everything one button owns. Ids are per button: param/input/output/light
// `i` all belong to button `i`; its mode switch is param MODE_PARAM + i.
struct ButtonChannel {
    ButtonLogic logic;
    AlLabel label;
    // Index into AL_LIGHT_COLOR_NAMES (AlLightColor.hpp).
    int colorIndex = 0;
    // Color while off: -1 = a faint glow of the on color (default), else an
    // index into AL_LIGHT_COLOR_NAMES, lit as brightly as the on color.
    int offColorIndex = -1;
};

struct Buttons : Module {
    static const int MODE_PARAM = NUM_BUTTONS;

    ButtonChannel channels[NUM_BUTTONS];
    dsp::ClockDivider lightDivider;

    Buttons() {
        config(2 * NUM_BUTTONS, NUM_BUTTONS, NUM_BUTTONS, NUM_BUTTONS);
        for (int i = 0; i < NUM_BUTTONS; i++) {
            configButton(i, defaultName(i));
            configSwitch(MODE_PARAM + i, 0.f, NUM_BUTTON_MODES - 1, BUTTON_MOMENTARY, defaultName(i) + " mode",
                std::vector<std::string>(BUTTON_MODE_NAMES, BUTTON_MODE_NAMES + NUM_BUTTON_MODES))->randomizeEnabled = false;
            configInput(i, defaultName(i));
            configOutput(i, defaultName(i));
            configBypass(i, i);
        }
        lightDivider.setDivision(BUTTON_LIGHT_DIVISION);
        updatePortNames();
    }

    static std::string defaultName(int i) {
        return string::f("Button %d", i + 1);
    }

    // Same as PadX: ports and params are configured once, before the labels
    // have their final value, so their tooltip names are kept in sync by hand.
    void updatePortNames() {
        for (int i = 0; i < NUM_BUTTONS; i++) {
            std::string base = channels[i].label.text.empty() ? defaultName(i) : channels[i].label.text;
            if (ParamQuantity* pq = getParamQuantity(i)) {
                pq->name = base;
            }
            if (ParamQuantity* pq = getParamQuantity(MODE_PARAM + i)) {
                pq->name = base + " mode";
            }
            if (PortInfo* info = getInputInfo(i)) {
                info->name = base;
            }
            if (PortInfo* info = getOutputInfo(i)) {
                info->name = base;
            }
        }
    }

    int modeParam(int i) {
        return clamp((int) std::round(params[MODE_PARAM + i].getValue()), 0, NUM_BUTTON_MODES - 1);
    }

    void onReset() override {
        for (int i = 0; i < NUM_BUTTONS; i++) {
            channels[i].logic.state = false;
            channels[i].logic.resetInput();
        }
    }

    void process(const ProcessArgs& args) override {
        bool updateLights = lightDivider.process();
        for (int i = 0; i < NUM_BUTTONS; i++) {
            ButtonLogic& logic = channels[i].logic;
            int mode = modeParam(i);
            if (mode != logic.mode) {
                logic.setMode(mode);
            }
            outputs[i].setVoltage(logic.step(params[i].getValue() > 0.5f, inputs[i].getVoltage(), args.sampleTime));
            if (updateLights) {
                // Off isn't fully dark: a faint glow in the button's own
                // color shows where (and which color) it is — or, with an
                // off color, that color at full brightness (the widget picks
                // the color, see AlLitButton).
                bool glow = !logic.state && channels[i].offColorIndex < 0;
                lights[i].setBrightness(glow ? 0.08f : 1.f);
            }
        }
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_t* buttonsJ = json_array();
        for (int i = 0; i < NUM_BUTTONS; i++) {
            const ButtonChannel& ch = channels[i];
            json_t* buttonJ = json_object();
            ch.label.toJson(buttonJ);
            json_object_set_new(buttonJ, "color", json_integer(ch.colorIndex));
            json_object_set_new(buttonJ, "offColor", json_integer(ch.offColorIndex));
            // A momentary state is only ever "held right now" — not saved.
            json_object_set_new(buttonJ, "state", json_boolean(ch.logic.mode != BUTTON_MOMENTARY && ch.logic.state));
            json_array_append_new(buttonsJ, buttonJ);
        }
        json_object_set_new(rootJ, "buttons", buttonsJ);
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* buttonsJ = json_object_get(rootJ, "buttons");
        if (!buttonsJ || !json_is_array(buttonsJ)) {
            return;
        }
        for (int i = 0; i < NUM_BUTTONS && i < (int) json_array_size(buttonsJ); i++) {
            json_t* buttonJ = json_array_get(buttonsJ, i);
            ButtonLogic& logic = channels[i].logic;
            channels[i].label.fromJson(buttonJ);
            channels[i].colorIndex = alLightColorFromJson(json_object_get(buttonJ, "color"));
            // Absent in patches saved before it existed: the faint glow.
            json_t* offColorJ = json_object_get(buttonJ, "offColor");
            channels[i].offColorIndex = offColorJ ? clamp((int) json_integer_value(offColorJ), -1, AL_NUM_LIGHT_COLORS - 1) : -1;
            // The mode switch's own value is already loaded (Rack restores
            // params before dataFromJson); apply it before the saved state so
            // process() doesn't see a mode change and clear it.
            logic.setMode(modeParam(i));
            if (json_t* j = json_object_get(buttonJ, "state")) {
                logic.state = logic.mode != BUTTON_MOMENTARY && json_boolean_value(j);
            }
            logic.resetInput();
        }
        updatePortNames();
    }
};

#ifndef HEADLESS
// 11 mm button (Rack's stock VCVButton is ~6 mm), still fitting 3HP.
// Momentary: latching is ButtonLogic's job, so the mode can change without
// swapping the widget.
struct AlButton : app::SvgSwitch {
    AlButton() {
        momentary = true;
        addFrame(Svg::load(asset::plugin(pluginInstance, "res/Button_0.svg")));
        addFrame(Svg::load(asset::plugin(pluginInstance, "res/Button_1.svg")));
    }
};

// 8.8 mm light over the button's cap: a thin rim (1.1 mm: 0.4 mm dark
// ring + 0.7 mm of cap) shows around it.
template <typename TBase>
struct AlButtonLight : TBase {
    AlButtonLight() {
        this->box.size = mm2px(Vec(8.8f, 8.8f));
    }
};

// Rack's own LightButton (componentlibrary.hpp) centers the light on the
// button. Its right-click menu holds the on/off colors and Toggle trigger's
// light resync.
struct AlLitButton : LightButton<AlButton, AlButtonLight<WhiteLight>> {
    Buttons* buttons() {
        ParamQuantity* pq = getParamQuantity();
        return pq ? dynamic_cast<Buttons*>(pq->module) : nullptr;
    }

    void step() override {
        LightButton<AlButton, AlButtonLight<WhiteLight>>::step();
        if (Buttons* mod = buttons()) {
            const ButtonChannel& ch = mod->channels[paramId];
            bool off = !ch.logic.state && ch.offColorIndex >= 0;
            alApplyLightColor(light, off ? ch.offColorIndex : ch.colorIndex);
        }
    }

    void appendContextMenu(Menu* menu) override {
        Buttons* mod = buttons();
        if (!mod) {
            return;
        }
        int i = paramId;
        menu->addChild(new MenuSeparator);
        alAppendLightColorMenu(menu,
            [=]() { return mod->channels[i].colorIndex; },
            [=](int k) { mod->channels[i].colorIndex = k; },
            "On color"
        );
        std::vector<std::string> offColorNames = {"Faint glow of on color"};
        offColorNames.insert(offColorNames.end(), AL_LIGHT_COLOR_NAMES, AL_LIGHT_COLOR_NAMES + AL_NUM_LIGHT_COLORS);
        menu->addChild(createIndexSubmenuItem("Off color", offColorNames,
            [=]() { return mod->channels[i].offColorIndex + 1; },
            [=](int k) { mod->channels[i].offColorIndex = k - 1; }
        ));
        if (mod->channels[i].logic.mode == BUTTON_TOGGLE_TRIGGER) {
            // Resync the light with a downstream switch toggled from
            // elsewhere, without sending it a trigger.
            menu->addChild(createMenuItem("Flip light (no trigger)", "",
                [=]() { mod->channels[i].logic.flip(); }
            ));
        }
    }
};

struct ButtonsWidget : ModuleWidget {

    explicit ButtonsWidget(Buttons* module) {
        setModule(module);

        const float panelWidth = 15.24f;
        const float centerX = panelWidth / 2.f;

        setPanel(new AlPanel(mm2px(Vec(panelWidth, 128.5f)),
            Svg::load(asset::plugin(pluginInstance, "res/Buttons_Silk_Light.svg")),
            Svg::load(asset::plugin(pluginInstance, "res/Buttons_Silk_Dark.svg"))));

        addChild(createWidget<AlScrewComponent>(Vec(0, 0)));
        addChild(createWidget<AlScrewComponent>(Vec(box.size.x - 1 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<AlScrewComponent>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<AlScrewComponent>(Vec(box.size.x - 1 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // Row 1, in mm; row 2 is the same, `rowOffsetY` lower, so its IN/OUT
        // land on the last two port rows of Zones/Pads/Fader (103/114).
        const float labelY = 14.f, modeY = 36.f, inputY = 46.f, outputY = 57.f;
        // Button centered in the gap between the label's bottom edge (6 mm
        // tall field) and the mode switch's top edge (CKSSThreeHorizontal is
        // 13.457 px = ~4.56 mm tall): ~25.4 mm.
        const float labelBottom = labelY + 6.f / 2.f;
        const float modeTop = modeY - 13.457f / RACK_GRID_WIDTH * 5.08f / 2.f;
        const float buttonY = (labelBottom + modeTop) / 2.f;
        const float rowOffsetY = 57.f;
        // As wide as 3HP allows: ~6 characters, same as Fader's.
        const float labelWidth = 13.2f;

        for (int i = 0; i < NUM_BUTTONS; i++) {
            const float dy = rowOffsetY * i;
            alAddLabelField(this, Vec(centerX, labelY + dy), labelWidth,
                module ? &module->channels[i].label : nullptr,
                [=]() { module->updatePortNames(); });
            addParam(createLightParamCentered<AlLitButton>(mm2px(Vec(centerX, buttonY + dy)), module, i, i));
            // Left to right: Momentary, Latch, Toggle trigger.
            addParam(createParamCentered<CKSSThreeHorizontal>(mm2px(Vec(centerX, modeY + dy)), module, Buttons::MODE_PARAM + i));
            addInput(createInputCentered<AlPortComponentIn>(mm2px(Vec(centerX, inputY + dy)), module, i));
            addOutput(createOutputCentered<AlPortComponentOut>(mm2px(Vec(centerX, outputY + dy)), module, i));
        }
    }

    void appendContextMenu(Menu* menu) override {
        appendAluminiumThemeMenu(menu);
    }
};
#else
struct ButtonsWidget : ModuleWidget {
    explicit ButtonsWidget(Buttons* module) {
        setModule(module);
        for (int i = 0; i < NUM_BUTTONS; i++) {
            addParam(createParam<VCVButton>({}, module, i));
            addParam(createParam<CKSSThreeHorizontal>({}, module, Buttons::MODE_PARAM + i));
            addInput(createInput<PJ301MPort>({}, module, i));
            addOutput(createOutput<PJ301MPort>({}, module, i));
        }
    }
};
#endif

Model* modelButtons = createModel<Buttons, ButtonsWidget>("Buttons");
