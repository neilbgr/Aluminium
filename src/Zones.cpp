#include "plugin.hpp"
#include "PanelTheme.hpp"
#include "AlPanel.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

// Splits a polyphonic MIDI-CV stream (V/OCT, GATE, VEL, AFT, RTRG — the poly
// outputs of Rack's core MIDI-CV module) into two note-range zones (A = below
// split point, B = at/above). Each zone independently outputs either:
//  - POLY: passthrough of the input channels belonging to that zone (same
//    channel indices as the input, so no channel is ever reassigned). GATE
//    and RTRG drop to 0 immediately for a channel that doesn't currently
//    belong; PITCH/VEL/AFT instead hold at that channel's *last* real value
//    from when it did belong, rather than mirroring the live input (which,
//    once the channel gets reused, is some other note entirely — see the
//    Zone::lastPitch/lastVel/lastAft comment) or snapping to 0 (which would
//    audibly retune a still-releasing envelope's tail downstream, since
//    GATE falling doesn't mean the note has actually gone silent yet). A
//    channel that has never belonged to a zone reads silent (0V) there,
//    same as if it had been zeroed; or
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
// GATE), and is held fixed until that channel's GATE falls (note-off), OR
// until that channel's own pitch drifts across the split point while GATE
// stays high with no RTRG edge (see the fallback in process() below) — the
// case a genuinely monophonic CV/Gate source (no separate retrigger line at
// all, e.g. a real analog mono keyboard) hits on legato overlap: channel 0's
// pitch changes to the new note but GATE never drops, so without this
// fallback the old zone would keep reporting the new note's pitch forever
// instead of handing it to the zone it now belongs in. Pitch bend/mod wheel
// are channel-wide MIDI messages, not per-note, so they're intentionally out
// of scope here — patch them directly from the MIDI-CV module to wherever
// they're needed. Portamento/glide is also out of scope — patch a Slew
// module after this one's MONO outputs if glide is wanted.
//
// DUO switch (Moog Subsequent 37 "DUO MODE", KB CTRL = HI): overrides both
// zones' modes and ignores the split point entirely. Zone B's outputs become
// a monophonic voice playing the highest held note on the whole keyboard,
// Zone A's the other held notes reduced per Zone A's priority menu (default
// Last note — the Sub37's KB PRIORITY drives OSC 1 the same way), so with
// C+G held, pressing E moves voice A to E. Each voice carries its own note's VEL/AFT, including
// after falling back to a still-held note on release. GATE behaves like MONO
// (true legato). RTRIG fires when a voice's gate rises or when it switches
// to a genuinely new key-press, never on a mere fallback unless the "retrigger
// on fallback" menu option is on. With a single key held, both voices play
// it (paraphonic, as on the Sub37) — or, with "A only" selected in the menu,
// voice B's gate stays low until a second key is held (duophonic), its
// PITCH/VEL/AFT holding their last values so a releasing envelope downstream
// doesn't glide.

static const char* NOTE_NAMES[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

// 0V = C4, matching the module's own note-name readout below.
static std::string voltageToNoteName(float voltage) {
    int totalSemi = (int)std::round(voltage * 12.f);
    int octaveOffset = (int)std::floor(totalSemi / 12.f);
    int noteIndex = totalSemi - octaveOffset * 12;
    int octave = 4 + octaveOffset;
    return std::string(NOTE_NAMES[noteIndex]) + std::to_string(octave);
}

// Parses note names ("C4", "c#4", "Db-1", "A#3", ...) into a V/OCT voltage.
// Returns false, leaving *voltage untouched, if s isn't a recognizable note
// name (letter A-G, optional #/b accidentals, octave number) — the caller
// falls back to plain numeric voltage parsing in that case.
static bool noteNameToVoltage(const std::string& s, float* voltage) {
    size_t i = 0;
    while (i < s.size() && std::isspace((unsigned char)s[i])) {
        i++;
    }
    if (i >= s.size()) {
        return false;
    }

    char letter = std::toupper((unsigned char)s[i]);
    static const int semitoneOfLetter[7] = {9, 11, 0, 2, 4, 5, 7}; // A..G
    if (letter < 'A' || letter > 'G') {
        return false;
    }
    int semitone = semitoneOfLetter[letter - 'A'];
    i++;

    while (i < s.size() && (s[i] == '#' || s[i] == 'b' || s[i] == 'B')) {
        semitone += (s[i] == '#') ? 1 : -1;
        i++;
    }

    while (i < s.size() && std::isspace((unsigned char)s[i])) {
        i++;
    }
    if (i >= s.size()) {
        return false;
    }

    const char* start = s.c_str() + i;
    char* end = nullptr;
    long octave = std::strtol(start, &end, 10);
    if (end == start) {
        return false;
    }
    while (*end && std::isspace((unsigned char)*end)) {
        end++;
    }
    if (*end != '\0') {
        return false;
    }

    int totalSemi = semitone + (int)(octave - 4) * 12;
    *voltage = totalSemi / 12.f;
    return true;
}

// Lets the SPLIT_PARAM knob's right-click text field (and its tooltip) be
// read/typed as a note name ("C4") instead of a raw V/OCT voltage.
struct SplitParamQuantity : ParamQuantity {
    std::string getDisplayValueString() override {
        return voltageToNoteName(getValue());
    }

    void setDisplayValueString(std::string s) override {
        float v;
        if (noteNameToVoltage(s, &v)) {
            setValue(math::clamp(v, getMinValue(), getMaxValue()));
        }
        else {
            ParamQuantity::setDisplayValueString(s);
        }
    }
};

struct Zones : Module {
    enum ParamIds {
        SPLIT_PARAM,
        LEARN_PARAM,
        ZONE_A_MODE_PARAM,
        ZONE_B_MODE_PARAM,
        DUO_PARAM,
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
        DUO_LIGHT,
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
        // POLY mode only: each channel's last PITCH/VEL/AFT while it belonged
        // to *this* zone, held onto once it stops belonging (GATE/RTRIG still
        // drop to 0 right away) instead of snapping to 0 or drifting to
        // whatever the now-reassigned live input channel shows — see
        // processZone(). Zero-initialized, so a channel that has never
        // belonged to this zone reads silent (0V) until it actually does.
        float lastPitch[16] = {};
        float lastVel[16] = {};
        float lastAft[16] = {};

        void remove(int c) {
            held.erase(std::remove(held.begin(), held.end(), c), held.end());
        }
    };

    Zone zoneA, zoneB;

    // DUO mode voice: A = lowest held note, B = highest. PITCH/VEL/AFT keep
    // their last values while the voice is silent (see the header comment).
    struct DuoVoice {
        int channel = -1; // -1 = gate low
        float pitch = 0.f;
        float vel = 0.f;
        float aft = 0.f;
        dsp::PulseGenerator retrigPulse;
    };

    DuoVoice duoA, duoB;
    // Menu options, DUO mode only.
    bool duoSingleKeyBoth = true; // false = voice B silent with one key held
    bool duoRetrigOnFallback = false;

    // Per input-channel state.
    int channelZone[16];         // -1 = unclassified, 0 = A, 1 = B
    bool prevGateHigh[16];
    // True for the specific channel whose note-on was consumed to set the
    // split point via LEARN — that note itself never plays through (only
    // notes played after LEARN turns off do), so it stays excluded from
    // topOf()/passthrough until its own note-off releases the channel.
    bool learnSuppressed[16];
    dsp::SchmittTrigger rtrgTrigger[16];
    // Global note-on order across both zones (each zone's `held` is only
    // ordered within itself), for DUO voice A's "Last note" priority.
    uint64_t pressOrder[16] = {};
    uint64_t pressCounter = 0;

    Zones() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam<SplitParamQuantity>(SPLIT_PARAM, -4.f, 4.f, 0.f, "Split point");
        configSwitch(LEARN_PARAM, 0.f, 1.f, 0.f, "Learn split point", {"Idle", "Listening for next note"});
        configSwitch(ZONE_A_MODE_PARAM, 0.f, 1.f, MODE_POLY, "Poly/mono Zone A", {"Polyphonic", "Monophonic"});
        configSwitch(ZONE_B_MODE_PARAM, 0.f, 1.f, MODE_POLY, "Poly/mono Zone B", {"Polyphonic", "Monophonic"});
        configSwitch(DUO_PARAM, 0.f, 1.f, 0.f, "Duo mode", {"Off", "On (B = highest note, A = other notes per Zone A priority)"});

        configInput(PITCH_INPUT, "Pitch (poly)");
        configInput(GATE_INPUT, "Gate (poly)");
        configInput(VEL_INPUT, "Velocity (poly)");
        configInput(AFT_INPUT, "Aftertouch (poly)");
        configInput(RTRG_INPUT, "Retrigger (poly)");

        configOutput(ZONE_A_PITCH_OUTPUT, "Pitch Zone A");
        configOutput(ZONE_A_GATE_OUTPUT, "Gate Zone A");
        configOutput(ZONE_A_VEL_OUTPUT, "Velocity Zone A");
        configOutput(ZONE_A_AFT_OUTPUT, "Aft.tch. Zone A");
        configOutput(ZONE_A_RTRIG_OUTPUT, "Retrig Zone A");
        configOutput(ZONE_B_PITCH_OUTPUT, "Pitch Zone B");
        configOutput(ZONE_B_GATE_OUTPUT, "Gate Zone B");
        configOutput(ZONE_B_VEL_OUTPUT, "Velocity Zone B");
        configOutput(ZONE_B_AFT_OUTPUT, "Aft.tch. Zone B");
        configOutput(ZONE_B_RTRIG_OUTPUT, "Retrig Zone B");

        resetChannelState();
    }

    void resetChannelState() {
        for (int c = 0; c < 16; c++) {
            channelZone[c] = -1;
            prevGateHigh[c] = false;
            learnSuppressed[c] = false;
        }
        zoneA.held.clear();
        zoneB.held.clear();
        for (int c = 0; c < 16; c++) {
            zoneA.lastPitch[c] = zoneA.lastVel[c] = zoneA.lastAft[c] = 0.f;
            zoneB.lastPitch[c] = zoneB.lastVel[c] = zoneB.lastAft[c] = 0.f;
        }
        duoA.channel = -1;
        duoB.channel = -1;
    }

    void onReset() override {
        resetChannelState();
    }

    Zone& zoneOf(int z) {
        return (z == 0) ? zoneA : zoneB;
    }

    // Selects which held channel is currently "on top" (audible) for a zone,
    // per its priority rule. Returns -1 if the zone has no (non-suppressed)
    // held notes.
    int topOf(const Zone& zone) {
        switch (zone.priority) {
            case PRIORITY_HIGH:
            case PRIORITY_LOW: {
                int best = -1;
                for (int c : zone.held) {
                    if (learnSuppressed[c]) {
                        continue;
                    }
                    bool better = (best < 0)
                        || (zone.priority == PRIORITY_HIGH
                            ? inputs[PITCH_INPUT].getVoltage(c) > inputs[PITCH_INPUT].getVoltage(best)
                            : inputs[PITCH_INPUT].getVoltage(c) < inputs[PITCH_INPUT].getVoltage(best));
                    if (better) {
                        best = c;
                    }
                }
                return best;
            }
            default: // PRIORITY_LAST
                for (auto it = zone.held.rbegin(); it != zone.held.rend(); ++it) {
                    if (!learnSuppressed[*it]) {
                        return *it;
                    }
                }
                return -1;
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
        pressOrder[c] = ++pressCounter;
    }

    void release(int c) {
        int z = channelZone[c];
        if (z >= 0) {
            zoneOf(z).remove(c);
        }
        channelZone[c] = -1;
    }

    void process(const ProcessArgs& args) override {
        int channels = inputs[PITCH_INPUT].getChannels();
        if (channels == 0 && (!zoneA.held.empty() || !zoneB.held.empty())) {
            resetChannelState();
        }
        channels = std::min(channels, 16);

        float threshold = params[SPLIT_PARAM].getValue();
        // LEARN_PARAM's widget is a latch (VCVLightLatch): its value directly
        // IS the armed state, toggled by the click itself — clicking while
        // armed turns it back off instead of leaving it stuck waiting.
        bool learnArmed = params[LEARN_PARAM].getValue() > 0.5f;
        bool duo = params[DUO_PARAM].getValue() > 0.5f;
        // The split point is meaningless in DUO mode, so LEARN can't arm.
        if (duo && learnArmed) {
            params[LEARN_PARAM].setValue(0.f);
            learnArmed = false;
        }
        // Channels with a genuine note-on (RTRG edge) this sample — DUO mode
        // only retriggers a voice for those.
        bool pressed[16] = {};

        for (int c = 0; c < channels; c++) {
            float pitch = inputs[PITCH_INPUT].getVoltage(c);
            bool gateHigh = inputs[GATE_INPUT].getVoltage(c) >= 1.f;

            // Note-off: drop the channel from whichever zone holds it.
            if (!gateHigh && prevGateHigh[c]) {
                release(c);
            }

            bool rtrgFired = rtrgTrigger[c].process(inputs[RTRG_INPUT].getVoltage(c));
            pressed[c] = rtrgFired;
            // Defensive fallback: a channel can be gate-high with no
            // classification yet (e.g. patch loaded mid-note) without ever
            // seeing its RTRG edge.
            bool needsClassification = gateHigh && channelZone[c] < 0;

            if (rtrgFired || needsClassification) {
                bool isLearnNote = false;
                if (learnArmed) {
                    threshold = pitch;
                    params[SPLIT_PARAM].setValue(pitch);
                    params[LEARN_PARAM].setValue(0.f);
                    learnArmed = false;
                    isLearnNote = true;
                }
                int zone = (pitch >= threshold) ? 1 : 0;
                Zone& z = zoneOf(zone);
                classify(c, zone);
                // The note that just set the split point stays excluded from
                // topOf()/passthrough (see learnSuppressed) until its own
                // note-off — only notes played after LEARN turns off actually
                // reach the outputs.
                learnSuppressed[c] = isLearnNote;
                if (rtrgFired && topOf(z) == c) {
                    z.retrigPulse.trigger(1e-3f);
                }
            }
            else if (gateHigh && channelZone[c] >= 0) {
                // No RTRG edge, but this already-classified channel's own
                // live pitch just crossed the split point — only reachable
                // from a raw mono CV/Gate source with no retrigger line (see
                // the comment atop this file); real polyphonic input never
                // changes a still-gated channel's pitch, so this is a no-op
                // there.
                int zone = (pitch >= threshold) ? 1 : 0;
                if (zone != channelZone[c]) {
                    Zone& z = zoneOf(zone);
                    classify(c, zone);
                    if (topOf(z) == c) {
                        z.retrigPulse.trigger(1e-3f);
                    }
                }
            }

            prevGateHigh[c] = gateHigh;
        }

        lights[LEARN_LIGHT].setBrightness(learnArmed ? 1.f : 0.f);

        int zoneAMode = (int)params[ZONE_A_MODE_PARAM].getValue();
        int zoneBMode = (int)params[ZONE_B_MODE_PARAM].getValue();
        // DUO forces both mode lights on: both zones' outputs are then mono.
        lights[ZONE_A_MODE_LIGHT].setBrightness(duo || zoneAMode == MODE_MONO ? 1.f : 0.f);
        lights[ZONE_B_MODE_LIGHT].setBrightness(duo || zoneBMode == MODE_MONO ? 1.f : 0.f);
        lights[DUO_LIGHT].setBrightness(duo ? 1.f : 0.f);

        if (!duo) {
            // Re-entering DUO later must see each voice's gate as rising.
            duoA.channel = -1;
            duoB.channel = -1;
        }

        if (duo) {
            processDuo(pressed, args.sampleTime);
        }
        // While LEARN is armed, hold every output silent — `learnArmed` was
        // already cleared above the moment a note lands and sets the split
        // point, so that same note's classification plays through normally
        // in this same sample rather than being swallowed too.
        else if (learnArmed) {
            muteZoneOutputs(ZONE_A_PITCH_OUTPUT, ZONE_A_GATE_OUTPUT, ZONE_A_VEL_OUTPUT, ZONE_A_AFT_OUTPUT, ZONE_A_RTRIG_OUTPUT);
            muteZoneOutputs(ZONE_B_PITCH_OUTPUT, ZONE_B_GATE_OUTPUT, ZONE_B_VEL_OUTPUT, ZONE_B_AFT_OUTPUT, ZONE_B_RTRIG_OUTPUT);
        }
        else {
            processZone(zoneA, zoneAMode, channels, args.sampleTime,
                ZONE_A_PITCH_OUTPUT, ZONE_A_GATE_OUTPUT, ZONE_A_VEL_OUTPUT, ZONE_A_AFT_OUTPUT, ZONE_A_RTRIG_OUTPUT, 0);
            processZone(zoneB, zoneBMode, channels, args.sampleTime,
                ZONE_B_PITCH_OUTPUT, ZONE_B_GATE_OUTPUT, ZONE_B_VEL_OUTPUT, ZONE_B_AFT_OUTPUT, ZONE_B_RTRIG_OUTPUT, 1);
        }
    }

    // DUO mode: zones are still classified as usual (so switching DUO off
    // mid-note resumes cleanly), but both voices pick from all held notes.
    // Voice B = highest held note. Voice A follows Zone A's priority menu
    // (default Last note, like the Sub37's KB PRIORITY driving OSC 1) among
    // the other held notes, so a third key can take over voice A.
    void processDuo(const bool* pressed, float sampleTime) {
        int highest = -1;
        int count = 0;
        for (const Zone* zone : {&zoneA, &zoneB}) {
            for (int c : zone->held) {
                if (learnSuppressed[c]) {
                    continue;
                }
                count++;
                // >= so two held channels at the same pitch still map to two
                // distinct voices.
                if (highest < 0 || inputs[PITCH_INPUT].getVoltage(c) >= inputs[PITCH_INPUT].getVoltage(highest)) {
                    highest = c;
                }
            }
        }

        int channelA = (count >= 2) ? duoPickA(highest) : highest;
        int channelB = (count >= 2 || duoSingleKeyBoth) ? highest : -1;
        processDuoVoice(duoA, channelA, pressed, sampleTime,
            ZONE_A_PITCH_OUTPUT, ZONE_A_GATE_OUTPUT, ZONE_A_VEL_OUTPUT, ZONE_A_AFT_OUTPUT, ZONE_A_RTRIG_OUTPUT);
        processDuoVoice(duoB, channelB, pressed, sampleTime,
            ZONE_B_PITCH_OUTPUT, ZONE_B_GATE_OUTPUT, ZONE_B_VEL_OUTPUT, ZONE_B_AFT_OUTPUT, ZONE_B_RTRIG_OUTPUT);
    }

    // Picks voice A among all held notes except `exclude` (voice B's).
    int duoPickA(int exclude) {
        int best = -1;
        for (const Zone* zone : {&zoneA, &zoneB}) {
            for (int c : zone->held) {
                if (learnSuppressed[c] || c == exclude) {
                    continue;
                }
                bool better;
                switch (zoneA.priority) {
                    case PRIORITY_HIGH:
                        better = best < 0 || inputs[PITCH_INPUT].getVoltage(c) > inputs[PITCH_INPUT].getVoltage(best);
                        break;
                    case PRIORITY_LOW:
                        better = best < 0 || inputs[PITCH_INPUT].getVoltage(c) < inputs[PITCH_INPUT].getVoltage(best);
                        break;
                    default: // PRIORITY_LAST
                        better = best < 0 || pressOrder[c] > pressOrder[best];
                        break;
                }
                if (better) {
                    best = c;
                }
            }
        }
        return best;
    }

    void processDuoVoice(DuoVoice& voice, int channel, const bool* pressed, float sampleTime,
                         int pitchOut, int gateOut, int velOut, int aftOut, int rtrigOut) {
        if (channel >= 0) {
            // Retrigger on gate rise, on a genuinely new key-press becoming
            // this voice's note (or a same-channel "Reuse" retrigger), and
            // optionally on fallback to an already-held note.
            bool fallback = channel != voice.channel && duoRetrigOnFallback;
            if (voice.channel < 0 || pressed[channel] || fallback) {
                voice.retrigPulse.trigger(1e-3f);
            }
            voice.pitch = inputs[PITCH_INPUT].getVoltage(channel);
            voice.vel = inputs[VEL_INPUT].getVoltage(channel);
            voice.aft = inputs[AFT_INPUT].getVoltage(channel);
        }
        voice.channel = channel;

        outputs[pitchOut].setChannels(1);
        outputs[gateOut].setChannels(1);
        outputs[velOut].setChannels(1);
        outputs[aftOut].setChannels(1);
        outputs[rtrigOut].setChannels(1);
        outputs[pitchOut].setVoltage(voice.pitch);
        outputs[velOut].setVoltage(voice.vel);
        outputs[aftOut].setVoltage(voice.aft);
        outputs[gateOut].setVoltage(channel >= 0 ? 10.f : 0.f);
        outputs[rtrigOut].setVoltage(voice.retrigPulse.process(sampleTime) ? 10.f : 0.f);
    }

    void muteZoneOutputs(int pitchOut, int gateOut, int velOut, int aftOut, int rtrigOut) {
        outputs[pitchOut].setChannels(0);
        outputs[gateOut].setChannels(0);
        outputs[velOut].setChannels(0);
        outputs[aftOut].setChannels(0);
        outputs[rtrigOut].setChannels(0);
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
                bool belongs = (channelZone[c] == zoneIndex) && !learnSuppressed[c];
                if (belongs) {
                    zone.lastPitch[c] = inputs[PITCH_INPUT].getVoltage(c);
                    zone.lastVel[c] = inputs[VEL_INPUT].getVoltage(c);
                    zone.lastAft[c] = inputs[AFT_INPUT].getVoltage(c);
                }
                // PITCH/VEL/AFT: held at their last real value once the
                // channel stops belonging (a still-releasing envelope
                // downstream must keep seeing the correct pitch), not reset
                // to 0 — only GATE/RTRIG actually need to drop instantly.
                outputs[pitchOut].setVoltage(zone.lastPitch[c], c);
                outputs[velOut].setVoltage(zone.lastVel[c], c);
                outputs[aftOut].setVoltage(zone.lastAft[c], c);
                outputs[gateOut].setVoltage(belongs ? inputs[GATE_INPUT].getVoltage(c) : 0.f, c);
                outputs[rtrigOut].setVoltage(belongs ? inputs[RTRG_INPUT].getVoltage(c) : 0.f, c);
            }
        }
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "zoneAPriority", json_integer(zoneA.priority));
        json_object_set_new(rootJ, "zoneBPriority", json_integer(zoneB.priority));
        json_object_set_new(rootJ, "duoSingleKeyBoth", json_boolean(duoSingleKeyBoth));
        json_object_set_new(rootJ, "duoRetrigOnFallback", json_boolean(duoRetrigOnFallback));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        if (json_t* j = json_object_get(rootJ, "zoneAPriority")) {
            zoneA.priority = json_integer_value(j);
        }
        if (json_t* j = json_object_get(rootJ, "zoneBPriority")) {
            zoneB.priority = json_integer_value(j);
        }
        if (json_t* j = json_object_get(rootJ, "duoSingleKeyBoth")) {
            duoSingleKeyBoth = json_boolean_value(j);
        }
        if (json_t* j = json_object_get(rootJ, "duoRetrigOnFallback")) {
            duoRetrigOnFallback = json_boolean_value(j);
        }
    }
};

// Live note-name + frequency readout for the split point, in the spirit of
// BogaudioModules' Reftone display (confirmed pattern: a small custom
// Widget with its own draw(), not part of the cached panel framebuffer,
// since its content changes continuously). Uses Rack's own bundled
// ShareTechMono font (asset::system) rather than shipping our own copy.
struct SplitNoteDisplay : TransparentWidget {
    Zones* module;

    SplitNoteDisplay(Zones* module, Vec size) : module(module) {
        box.size = size;
    }

    void draw(const DrawArgs& args) override {
        nvgBeginPath(args.vg);
        nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 2.f);
        nvgFillColor(args.vg, nvgRGBA(0x10, 0x10, 0x10, 0xff));
        nvgFill(args.vg);
        nvgStrokeColor(args.vg, nvgRGBA(0x50, 0x50, 0x50, 0xff));
        nvgStroke(args.vg);
    }

    // Note name + frequency text drawn on layer 1, like Rack's own
    // LedDisplayChoice/LedDisplayTextField (see app/LedDisplay.cpp) — that
    // layer is painted after RackWidget's ambient-darkness overlay, so this
    // readout stays as bright and legible as PadX's label field even with
    // the room lights turned way down, instead of dimming along with the
    // rest of the panel.
    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer == 1) {
            float voltage = module ? module->params[Zones::SPLIT_PARAM].getValue() : 0.f;
            float freq = dsp::FREQ_C4 * std::pow(2.f, voltage);
            bool duo = module && module->params[Zones::DUO_PARAM].getValue() > 0.5f;

            std::string noteStr = voltageToNoteName(voltage);
            std::string freqStr = string::f(freq >= 1000.f ? "%.0f Hz" : "%.1f Hz", freq);

            std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
            if (font && font->handle >= 0) {
                nvgFontFaceId(args.vg, font->handle);
                nvgFillColor(args.vg, nvgRGBA(0x40, 0xff, 0x80, 0xee));
                nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

                nvgFontSize(args.vg, 22.f);
                // The split point is ignored in DUO mode.
                if (duo) {
                    nvgText(args.vg, box.size.x / 2.f, box.size.y / 2.f, "Duo", NULL);
                    Widget::drawLayer(args, layer);
                    return;
                }
                nvgText(args.vg, box.size.x / 2.f, box.size.y * 0.36f, noteStr.c_str(), NULL);

                nvgFontSize(args.vg, 11.f);
                nvgText(args.vg, box.size.x / 2.f, box.size.y * 0.75f, freqStr.c_str(), NULL);
            }
        }

        Widget::drawLayer(args, layer);
    }
};

struct ZonesWidget : ModuleWidget {
    ZonesWidget(Zones* module) {
        setModule(module);
        setPanel(new AlPanel(mm2px(Vec(55.88f, 128.5f)),
            Svg::load(asset::plugin(pluginInstance, "res/Zones_Silk_Light.svg")),
            Svg::load(asset::plugin(pluginInstance, "res/Zones_Silk_Dark.svg"))));

        addChild(createWidget<AlScrewComponent>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<AlScrewComponent>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<AlScrewComponent>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<AlScrewComponent>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // Placeholder coordinates — panel layout WIP in Inkscape. Grid: 11HP
        // wide, 4 columns, 8 rows (row 1 spans 2 row-heights for the knob +
        // split display). "Text" cells below are silkscreen labels, drawn on
        // the sérigraphie SVG layer, not runtime widgets — listed here only
        // as comments so the code and the artwork stay in sync.
        // col2 (24mm) carries no runtime widget — every row's 2nd column is
        // a silkscreen label ("Split", "Learn", "V/Oct", ...), not listed
        // here since it lives on the sérigraphie SVG layer, not in code.
        // Columns are no longer evenly spaced: col1/col3/col4 only need to
        // clear a knob/port, but col2 has to fit its longest label
        // ("Aftertouch", measured ~19.1mm in the silkscreen artwork) plus a
        // little breathing room on each side. col3/col4 already reflect that
        // (col2-col3 widened to 16.5mm for the label, col3-col4 tightened to
        // 10.5mm since it's just port-to-port) — kept as-is from the 12HP
        // layout. The panel's 1HP reduction comes entirely out of the right
        // margin instead (was 9.96mm at 12HP, now 4.88mm at 11HP).
        const float col1 = 8.f, col3 = 39.f, col4 = 50.f;
        const float row2 = 48.f, row3 = 59.f, row4 = 70.f;
        const float row5 = 81.f, row6 = 92.f, row7 = 103.f, row8 = 114.f;
 
        const float displayCenterX = 55.88f / 2.f;
        // "Split" (silkscreen)
        {
            Vec size = mm2px(Vec(24.f, 15.f));
            auto* display = new SplitNoteDisplay(module, size);
            display->box.pos = mm2px(Vec(displayCenterX, 20.f)).minus(size.div(2));
            addChild(display);
        }
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(displayCenterX, 36.f)), module, Zones::SPLIT_PARAM));

        // Row 2: LEARN_PARAM | "Learn" | "A" | "B"
        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<RedLight>>>(
            mm2px(Vec(col1, row2)), module, Zones::LEARN_PARAM, Zones::LEARN_LIGHT));
        // col2: "Learn", col3: "A", col4: "B" (silkscreen)

        // Row 3: PITCH_INPUT | "V/Oct" | ZONE_A_PITCH_OUTPUT | ZONE_B_PITCH_OUTPUT
        addInput(createInputCentered<AlPortComponentIn>(mm2px(Vec(col1, row3)), module, Zones::PITCH_INPUT));
        addOutput(createOutputCentered<AlPortComponentOut>(mm2px(Vec(col3, row3)), module, Zones::ZONE_A_PITCH_OUTPUT));
        addOutput(createOutputCentered<AlPortComponentOut>(mm2px(Vec(col4, row3)), module, Zones::ZONE_B_PITCH_OUTPUT));

        // Row 4: GATE_INPUT | "Gate" | ZONE_A_GATE_OUTPUT | ZONE_B_GATE_OUTPUT
        addInput(createInputCentered<AlPortComponentIn>(mm2px(Vec(col1, row4)), module, Zones::GATE_INPUT));
        addOutput(createOutputCentered<AlPortComponentOut>(mm2px(Vec(col3, row4)), module, Zones::ZONE_A_GATE_OUTPUT));
        addOutput(createOutputCentered<AlPortComponentOut>(mm2px(Vec(col4, row4)), module, Zones::ZONE_B_GATE_OUTPUT));

        // Row 5: VEL_INPUT | "Velocity" | ZONE_A_VEL_OUTPUT | ZONE_B_VEL_OUTPUT
        addInput(createInputCentered<AlPortComponentIn>(mm2px(Vec(col1, row5)), module, Zones::VEL_INPUT));
        addOutput(createOutputCentered<AlPortComponentOut>(mm2px(Vec(col3, row5)), module, Zones::ZONE_A_VEL_OUTPUT));
        addOutput(createOutputCentered<AlPortComponentOut>(mm2px(Vec(col4, row5)), module, Zones::ZONE_B_VEL_OUTPUT));

        // Row 6: AFT_INPUT | "Aftertouch" | ZONE_A_AFT_OUTPUT | ZONE_B_AFT_OUTPUT
        addInput(createInputCentered<AlPortComponentIn>(mm2px(Vec(col1, row6)), module, Zones::AFT_INPUT));
        addOutput(createOutputCentered<AlPortComponentOut>(mm2px(Vec(col3, row6)), module, Zones::ZONE_A_AFT_OUTPUT));
        addOutput(createOutputCentered<AlPortComponentOut>(mm2px(Vec(col4, row6)), module, Zones::ZONE_B_AFT_OUTPUT));

        // Row 7: RTRG_INPUT | "Retrigger" | ZONE_A_RTRIG_OUTPUT | ZONE_B_RTRIG_OUTPUT
        addInput(createInputCentered<AlPortComponentIn>(mm2px(Vec(col1, row7)), module, Zones::RTRG_INPUT));
        addOutput(createOutputCentered<AlPortComponentOut>(mm2px(Vec(col3, row7)), module, Zones::ZONE_A_RTRIG_OUTPUT));
        addOutput(createOutputCentered<AlPortComponentOut>(mm2px(Vec(col4, row7)), module, Zones::ZONE_B_RTRIG_OUTPUT));

        // Row 8: DUO_PARAM | "Mono" | ZONE_A_MODE_PARAM | ZONE_B_MODE_PARAM
        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<YellowLight>>>(
            mm2px(Vec(col1, row8)), module, Zones::DUO_PARAM, Zones::DUO_LIGHT));
        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(
            mm2px(Vec(col3, row8)), module, Zones::ZONE_A_MODE_PARAM, Zones::ZONE_A_MODE_LIGHT));
        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(
            mm2px(Vec(col4, row8)), module, Zones::ZONE_B_MODE_PARAM, Zones::ZONE_B_MODE_LIGHT));
    }

    void appendContextMenu(Menu* menu) override {
        Zones* module = dynamic_cast<Zones*>(this->module);
        assert(module);

        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexSubmenuItem("Monophonic priority (Zone A, and Duo voice A)",
            {"Last note", "Highest note", "Lowest note"},
            [=]() { return module->zoneA.priority; },
            [=](int p) { module->zoneA.priority = p; }
        ));
        menu->addChild(createIndexSubmenuItem("Monophonic priority (Zone B)",
            {"Last note", "Highest note", "Lowest note"},
            [=]() { return module->zoneB.priority; },
            [=](int p) { module->zoneB.priority = p; }
        ));

        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexSubmenuItem("Duo: single key",
            {"Both voices (Sub37, paraphonic)", "A only (duophonic)"},
            [=]() { return module->duoSingleKeyBoth ? 0 : 1; },
            [=](int i) { module->duoSingleKeyBoth = (i == 0); }
        ));
        menu->addChild(createBoolPtrMenuItem("Duo: retrigger on fallback to a held note", "", &module->duoRetrigOnFallback));

        appendAluminiumThemeMenu(menu);
    }
};

Model* modelZones = createModel<Zones, ZonesWidget>("Zones");
