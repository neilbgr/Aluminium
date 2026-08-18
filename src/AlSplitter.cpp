#include "plugin.hpp"
#include "PanelTheme.hpp"
#include "AlPanel.hpp"
#include <algorithm>
#include <cmath>

// Splits a polyphonic MIDI-CV stream (V/OCT, GATE, VEL, AFT, RTRG — the poly
// outputs of Rack's core MIDI-CV module) into two note-range zones (A = below
// split point, B = at/above). Each zone independently outputs either:
//  - POLY: passthrough of the input channels belonging to that zone (same
//    channel indices as the input, GATE/RTRG masked off for channels that
//    don't belong), so no channel is ever reassigned and downstream poly
//    modules never see a spurious retrigger; or
//  - MONO: a single-channel reduction with a configurable note-priority rule
//    (last/highest/lowest). GATE stays high across overlapping held notes in
//    the zone (true legato — matches the non-retrigger GATE behavior of the
//    core module's own mono mode) and only drops when the zone empties.
//    RTRIG pulses only when a genuinely new key-press becomes the zone's
//    audible note; falling back to a previously-held note on release does
//    not retrigger, again mirroring the core module's own convention.
//
// Classification of an input channel into a zone happens once, on that
// channel's RTRG rising edge (a real note-on, per the core module's own
// semantics — including same-channel "Reuse" retriggers that never drop
// GATE), and is held fixed until that channel's GATE falls (note-off). Pitch
// bend/mod wheel are channel-wide MIDI messages, not per-note, so they're
// intentionally out of scope here — patch them directly from the MIDI-CV
// module to wherever they're needed. Portamento/glide is also out of scope —
// patch a Slew module after this one's MONO outputs if glide is wanted.

struct AlSplitter : Module {
    enum ParamIds {
        SPLIT_PARAM,
        LEARN_PARAM,
        ZONE_A_MODE_PARAM,
        ZONE_B_MODE_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        PITCH_INPUT,
        GATE_INPUT,
        VEL_INPUT,
        AFT_INPUT,
        RTRG_INPUT,
        NUM_INPUTS
    };
    enum OutputIds {
        ZONE_A_PITCH_OUTPUT,
        ZONE_A_GATE_OUTPUT,
        ZONE_A_VEL_OUTPUT,
        ZONE_A_AFT_OUTPUT,
        ZONE_A_RTRIG_OUTPUT,
        ZONE_B_PITCH_OUTPUT,
        ZONE_B_GATE_OUTPUT,
        ZONE_B_VEL_OUTPUT,
        ZONE_B_AFT_OUTPUT,
        ZONE_B_RTRIG_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightIds {
        LEARN_LIGHT,
        ZONE_A_MODE_LIGHT,
        ZONE_B_MODE_LIGHT,
        NUM_LIGHTS
    };

    enum Mode {
        MODE_POLY,
        MODE_MONO
    };

    enum Priority {
        PRIORITY_LAST,
        PRIORITY_HIGH,
        PRIORITY_LOW,
        NUM_PRIORITIES
    };

    struct Zone {
        int priority = PRIORITY_LAST;
        // Channel indices (into the shared PITCH/GATE/VEL/AFT/RTRG inputs)
        // currently held and classified into this zone, oldest first.
        std::vector<int> held;
        dsp::PulseGenerator retrigPulse;

        void remove(int c) {
            held.erase(std::remove(held.begin(), held.end(), c), held.end());
        }
    };

    Zone zoneA, zoneB;

    // Per input-channel state.
    int channelZone[16];         // -1 = unclassified, 0 = A, 1 = B
    bool prevGateHigh[16];
    dsp::SchmittTrigger rtrgTrigger[16];

    AlSplitter() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(SPLIT_PARAM, -4.f, 4.f, 0.f, "Split point", " V");
        configSwitch(LEARN_PARAM, 0.f, 1.f, 0.f, "Learn split point", {"Idle", "Listening for next note"});
        configSwitch(ZONE_A_MODE_PARAM, 0.f, 1.f, MODE_POLY, "Zone A mode", {"Polyphonic", "Monophonic"});
        configSwitch(ZONE_B_MODE_PARAM, 0.f, 1.f, MODE_POLY, "Zone B mode", {"Polyphonic", "Monophonic"});

        configInput(PITCH_INPUT, "V/OCT (poly, from MIDI-CV)");
        configInput(GATE_INPUT, "Gate (poly, from MIDI-CV)");
        configInput(VEL_INPUT, "Velocity (poly, from MIDI-CV)");
        configInput(AFT_INPUT, "Aftertouch (poly, from MIDI-CV)");
        configInput(RTRG_INPUT, "Retrigger (poly, from MIDI-CV)");

        configOutput(ZONE_A_PITCH_OUTPUT, "Zone A V/OCT");
        configOutput(ZONE_A_GATE_OUTPUT, "Zone A gate");
        configOutput(ZONE_A_VEL_OUTPUT, "Zone A velocity");
        configOutput(ZONE_A_AFT_OUTPUT, "Zone A aftertouch");
        configOutput(ZONE_A_RTRIG_OUTPUT, "Zone A retrigger");
        configOutput(ZONE_B_PITCH_OUTPUT, "Zone B V/OCT");
        configOutput(ZONE_B_GATE_OUTPUT, "Zone B gate");
        configOutput(ZONE_B_VEL_OUTPUT, "Zone B velocity");
        configOutput(ZONE_B_AFT_OUTPUT, "Zone B aftertouch");
        configOutput(ZONE_B_RTRIG_OUTPUT, "Zone B retrigger");

        resetChannelState();
    }

    void resetChannelState() {
        for (int c = 0; c < 16; c++) {
            channelZone[c] = -1;
            prevGateHigh[c] = false;
        }
        zoneA.held.clear();
        zoneB.held.clear();
    }

    void onReset() override {
        resetChannelState();
    }

    Zone& zoneOf(int z) {
        return (z == 0) ? zoneA : zoneB;
    }

    // Selects which held channel is currently "on top" (audible) for a zone,
    // per its priority rule. Returns -1 if the zone has no held notes.
    int topOf(const Zone& zone) {
        if (zone.held.empty())
            return -1;
        switch (zone.priority) {
            case PRIORITY_LAST:
                return zone.held.back();
            case PRIORITY_HIGH: {
                int best = zone.held[0];
                for (int c : zone.held)
                    if (inputs[PITCH_INPUT].getVoltage(c) > inputs[PITCH_INPUT].getVoltage(best))
                        best = c;
                return best;
            }
            case PRIORITY_LOW: {
                int best = zone.held[0];
                for (int c : zone.held)
                    if (inputs[PITCH_INPUT].getVoltage(c) < inputs[PITCH_INPUT].getVoltage(best))
                        best = c;
                return best;
            }
            default:
                return zone.held.back();
        }
    }

    void classify(int c, int zone) {
        // Defensive: a channel can only be classified into one zone at a
        // time, but guard against an unexpected duplicate entry (e.g. a
        // "Reuse" mode retrigger on a channel whose previous note-off was
        // somehow missed).
        zoneA.remove(c);
        zoneB.remove(c);
        channelZone[c] = zone;
        zoneOf(zone).held.push_back(c);
    }

    void release(int c) {
        int z = channelZone[c];
        if (z >= 0)
            zoneOf(z).remove(c);
        channelZone[c] = -1;
    }

    void process(const ProcessArgs& args) override {
        int channels = inputs[PITCH_INPUT].getChannels();
        if (channels == 0 && (!zoneA.held.empty() || !zoneB.held.empty()))
            resetChannelState();
        channels = std::min(channels, 16);

        float threshold = params[SPLIT_PARAM].getValue();
        // LEARN_PARAM's widget is a latch (VCVLightLatch): its value directly
        // IS the armed state, toggled by the click itself — clicking while
        // armed turns it back off instead of leaving it stuck waiting.
        bool learnArmed = params[LEARN_PARAM].getValue() > 0.5f;

        for (int c = 0; c < channels; c++) {
            float pitch = inputs[PITCH_INPUT].getVoltage(c);
            bool gateHigh = inputs[GATE_INPUT].getVoltage(c) >= 1.f;

            // Note-off: drop the channel from whichever zone holds it.
            if (!gateHigh && prevGateHigh[c]) {
                release(c);
            }

            bool rtrgFired = rtrgTrigger[c].process(inputs[RTRG_INPUT].getVoltage(c));
            // Defensive fallback: a channel can be gate-high with no
            // classification yet (e.g. patch loaded mid-note) without ever
            // seeing its RTRG edge.
            bool needsClassification = gateHigh && channelZone[c] < 0;

            if (rtrgFired || needsClassification) {
                if (learnArmed) {
                    threshold = pitch;
                    params[SPLIT_PARAM].setValue(pitch);
                    params[LEARN_PARAM].setValue(0.f);
                    learnArmed = false;
                }
                int zone = (pitch >= threshold) ? 1 : 0;
                Zone& z = zoneOf(zone);
                classify(c, zone);
                if (rtrgFired && topOf(z) == c) {
                    z.retrigPulse.trigger(1e-3f);
                }
            }

            prevGateHigh[c] = gateHigh;
        }

        lights[LEARN_LIGHT].setBrightness(learnArmed ? 1.f : 0.f);

        int zoneAMode = (int)params[ZONE_A_MODE_PARAM].getValue();
        int zoneBMode = (int)params[ZONE_B_MODE_PARAM].getValue();
        lights[ZONE_A_MODE_LIGHT].setBrightness(zoneAMode == MODE_MONO ? 1.f : 0.f);
        lights[ZONE_B_MODE_LIGHT].setBrightness(zoneBMode == MODE_MONO ? 1.f : 0.f);

        processZone(zoneA, zoneAMode, channels, args.sampleTime,
            ZONE_A_PITCH_OUTPUT, ZONE_A_GATE_OUTPUT, ZONE_A_VEL_OUTPUT, ZONE_A_AFT_OUTPUT, ZONE_A_RTRIG_OUTPUT, 0);
        processZone(zoneB, zoneBMode, channels, args.sampleTime,
            ZONE_B_PITCH_OUTPUT, ZONE_B_GATE_OUTPUT, ZONE_B_VEL_OUTPUT, ZONE_B_AFT_OUTPUT, ZONE_B_RTRIG_OUTPUT, 1);
    }

    void processZone(Zone& zone, int mode, int channels, float sampleTime,
                      int pitchOut, int gateOut, int velOut, int aftOut, int rtrigOut, int zoneIndex) {
        if (mode == MODE_MONO) {
            int top = topOf(zone);
            outputs[pitchOut].setChannels(1);
            outputs[gateOut].setChannels(1);
            outputs[velOut].setChannels(1);
            outputs[aftOut].setChannels(1);
            outputs[rtrigOut].setChannels(1);

            if (top >= 0) {
                outputs[pitchOut].setVoltage(inputs[PITCH_INPUT].getVoltage(top));
                outputs[velOut].setVoltage(inputs[VEL_INPUT].getVoltage(top));
                outputs[aftOut].setVoltage(inputs[AFT_INPUT].getVoltage(top));
                outputs[gateOut].setVoltage(10.f);
            }
            else {
                outputs[gateOut].setVoltage(0.f);
            }
            outputs[rtrigOut].setVoltage(zone.retrigPulse.process(sampleTime) ? 10.f : 0.f);
        }
        else {
            outputs[pitchOut].setChannels(channels);
            outputs[gateOut].setChannels(channels);
            outputs[velOut].setChannels(channels);
            outputs[aftOut].setChannels(channels);
            outputs[rtrigOut].setChannels(channels);

            for (int c = 0; c < channels; c++) {
                bool belongs = (channelZone[c] == zoneIndex);
                outputs[pitchOut].setVoltage(inputs[PITCH_INPUT].getVoltage(c), c);
                outputs[velOut].setVoltage(inputs[VEL_INPUT].getVoltage(c), c);
                outputs[aftOut].setVoltage(inputs[AFT_INPUT].getVoltage(c), c);
                outputs[gateOut].setVoltage(belongs ? inputs[GATE_INPUT].getVoltage(c) : 0.f, c);
                outputs[rtrigOut].setVoltage(belongs ? inputs[RTRG_INPUT].getVoltage(c) : 0.f, c);
            }
        }
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "zoneAPriority", json_integer(zoneA.priority));
        json_object_set_new(rootJ, "zoneBPriority", json_integer(zoneB.priority));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        if (json_t* j = json_object_get(rootJ, "zoneAPriority"))
            zoneA.priority = json_integer_value(j);
        if (json_t* j = json_object_get(rootJ, "zoneBPriority"))
            zoneB.priority = json_integer_value(j);
    }
};

// Live note-name + frequency readout for the split point, in the spirit of
// BogaudioModules' Reftone display (confirmed pattern: a small custom
// Widget with its own draw(), not part of the cached panel framebuffer,
// since its content changes continuously). Uses Rack's own bundled
// ShareTechMono font (asset::system) rather than shipping our own copy.
struct SplitNoteDisplay : TransparentWidget {
    AlSplitter* module;

    static constexpr const char* NOTE_NAMES[12] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };

    SplitNoteDisplay(AlSplitter* module, Vec size) : module(module) {
        box.size = size;
    }

    void draw(const DrawArgs& args) override {
        nvgBeginPath(args.vg);
        nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 2.f);
        nvgFillColor(args.vg, nvgRGBA(0x10, 0x10, 0x10, 0xff));
        nvgFill(args.vg);
        nvgStrokeColor(args.vg, nvgRGBA(0x50, 0x50, 0x50, 0xff));
        nvgStroke(args.vg);

        float voltage = module ? module->params[AlSplitter::SPLIT_PARAM].getValue() : 0.f;
        int totalSemi = (int)std::round(voltage * 12.f);
        int octaveOffset = (int)std::floor(totalSemi / 12.f);
        int noteIndex = totalSemi - octaveOffset * 12;
        int octave = 4 + octaveOffset;
        float freq = dsp::FREQ_C4 * std::pow(2.f, voltage);

        std::string noteStr = std::string(NOTE_NAMES[noteIndex]) + std::to_string(octave);
        std::string freqStr = string::f(freq >= 1000.f ? "%.0f Hz" : "%.1f Hz", freq);

        std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
        if (!font)
            return;

        nvgFontFaceId(args.vg, font->handle);
        nvgFillColor(args.vg, nvgRGBA(0x40, 0xff, 0x80, 0xee));
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

        nvgFontSize(args.vg, 22.f);
        nvgText(args.vg, box.size.x / 2.f, box.size.y * 0.36f, noteStr.c_str(), NULL);

        nvgFontSize(args.vg, 11.f);
        nvgText(args.vg, box.size.x / 2.f, box.size.y * 0.75f, freqStr.c_str(), NULL);
    }
};

constexpr const char* SplitNoteDisplay::NOTE_NAMES[12];

struct AlSplitterWidget : ModuleWidget {
    AlSplitterWidget(AlSplitter* module) {
        setModule(module);
        setPanel(new AlPanel(mm2px(Vec(60.96f, 128.5f)),
            Svg::load(asset::plugin(pluginInstance, "res/AlSplitter_Silk_Light.svg")),
            Svg::load(asset::plugin(pluginInstance, "res/AlSplitter_Silk_Dark.svg"))));

        addChild(createWidget<AlScrewComponent>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<AlScrewComponent>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<AlScrewComponent>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<AlScrewComponent>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // Placeholder coordinates — panel layout WIP in Inkscape. Grid: 12HP
        // wide, 4 columns, 8 rows (row 1 spans 2 row-heights for the knob +
        // split display). "Text" cells below are silkscreen labels, drawn on
        // the sérigraphie SVG layer, not runtime widgets — listed here only
        // as comments so the code and the artwork stay in sync.
        // col2 (24mm) carries no runtime widget — every row's 2nd column is
        // a silkscreen label ("Split", "Learn", "V/Oct", ...), not listed
        // here since it lives on the sérigraphie SVG layer, not in code.
        const float col1 = 9.f, col3 = 39.f, col4 = 54.f;
        const float row1 = 30.f, row2 = 48.f, row3 = 60.f, row4 = 72.f;
        const float row5 = 84.f, row6 = 96.f, row7 = 108.f, row8 = 120.f;

        // Row 1 (span 2): SPLIT_PARAM | "Split" | splitNoteDisplay (col3+col4)
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(col1, row1)), module, AlSplitter::SPLIT_PARAM));
        // col2: "Split" (silkscreen)
        {
            Vec size = mm2px(Vec(26.f, 18.f));
            auto* display = new SplitNoteDisplay(module, size);
            display->box.pos = mm2px(Vec((col3 + col4) / 2.f, row1)).minus(size.div(2));
            addChild(display);
        }

        // Row 2: LEARN_PARAM | "Learn" | "A" | "B"
        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<GreenLight>>>(
            mm2px(Vec(col1, row2)), module, AlSplitter::LEARN_PARAM, AlSplitter::LEARN_LIGHT));
        // col2: "Learn", col3: "A", col4: "B" (silkscreen)

        // Row 3: PITCH_INPUT | "V/Oct" | ZONE_A_PITCH_OUTPUT | ZONE_B_PITCH_OUTPUT
        addInput(createInputCentered<AlPortComponent>(mm2px(Vec(col1, row3)), module, AlSplitter::PITCH_INPUT));
        addOutput(createOutputCentered<AlPortComponent>(mm2px(Vec(col3, row3)), module, AlSplitter::ZONE_A_PITCH_OUTPUT));
        addOutput(createOutputCentered<AlPortComponent>(mm2px(Vec(col4, row3)), module, AlSplitter::ZONE_B_PITCH_OUTPUT));

        // Row 4: GATE_INPUT | "Gate" | ZONE_A_GATE_OUTPUT | ZONE_B_GATE_OUTPUT
        addInput(createInputCentered<AlPortComponent>(mm2px(Vec(col1, row4)), module, AlSplitter::GATE_INPUT));
        addOutput(createOutputCentered<AlPortComponent>(mm2px(Vec(col3, row4)), module, AlSplitter::ZONE_A_GATE_OUTPUT));
        addOutput(createOutputCentered<AlPortComponent>(mm2px(Vec(col4, row4)), module, AlSplitter::ZONE_B_GATE_OUTPUT));

        // Row 5: VEL_INPUT | "Velocity" | ZONE_A_VEL_OUTPUT | ZONE_B_VEL_OUTPUT
        addInput(createInputCentered<AlPortComponent>(mm2px(Vec(col1, row5)), module, AlSplitter::VEL_INPUT));
        addOutput(createOutputCentered<AlPortComponent>(mm2px(Vec(col3, row5)), module, AlSplitter::ZONE_A_VEL_OUTPUT));
        addOutput(createOutputCentered<AlPortComponent>(mm2px(Vec(col4, row5)), module, AlSplitter::ZONE_B_VEL_OUTPUT));

        // Row 6: AFT_INPUT | "Aftertouch" | ZONE_A_AFT_OUTPUT | ZONE_B_AFT_OUTPUT
        addInput(createInputCentered<AlPortComponent>(mm2px(Vec(col1, row6)), module, AlSplitter::AFT_INPUT));
        addOutput(createOutputCentered<AlPortComponent>(mm2px(Vec(col3, row6)), module, AlSplitter::ZONE_A_AFT_OUTPUT));
        addOutput(createOutputCentered<AlPortComponent>(mm2px(Vec(col4, row6)), module, AlSplitter::ZONE_B_AFT_OUTPUT));

        // Row 7: RTRG_INPUT | "Retrigger" | ZONE_A_RTRIG_OUTPUT | ZONE_B_RTRIG_OUTPUT
        addInput(createInputCentered<AlPortComponent>(mm2px(Vec(col1, row7)), module, AlSplitter::RTRG_INPUT));
        addOutput(createOutputCentered<AlPortComponent>(mm2px(Vec(col3, row7)), module, AlSplitter::ZONE_A_RTRIG_OUTPUT));
        addOutput(createOutputCentered<AlPortComponent>(mm2px(Vec(col4, row7)), module, AlSplitter::ZONE_B_RTRIG_OUTPUT));

        // Row 8: (empty) | "Mono" | ZONE_A_MODE_PARAM | ZONE_B_MODE_PARAM
        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<GreenLight>>>(
            mm2px(Vec(col3, row8)), module, AlSplitter::ZONE_A_MODE_PARAM, AlSplitter::ZONE_A_MODE_LIGHT));
        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<GreenLight>>>(
            mm2px(Vec(col4, row8)), module, AlSplitter::ZONE_B_MODE_PARAM, AlSplitter::ZONE_B_MODE_LIGHT));
    }

    void appendContextMenu(Menu* menu) override {
        AlSplitter* module = dynamic_cast<AlSplitter*>(this->module);
        assert(module);

        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexSubmenuItem("Zone A priority (when monophonic)",
            {"Last note", "Highest note", "Lowest note"},
            [=]() { return module->zoneA.priority; },
            [=](int p) { module->zoneA.priority = p; }
        ));
        menu->addChild(createIndexSubmenuItem("Zone B priority (when monophonic)",
            {"Last note", "Highest note", "Lowest note"},
            [=]() { return module->zoneB.priority; },
            [=](int p) { module->zoneB.priority = p; }
        ));

        appendAluminiumThemeMenu(menu);
    }
};

Model* modelAlSplitter = createModel<AlSplitter, AlSplitterWidget>("AlSplitter");
