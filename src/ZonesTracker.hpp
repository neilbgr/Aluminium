#pragma once
#include <cstdint>

// Zones' note-tracking core — which input channel belongs to which zone,
// held-note order, LEARN, and the mono/DUO note-priority picks — kept free
// of any Rack dependency so it can be unit-tested headless (see
// tests/ZonesTrackerTest.cpp, `make test`). Zones.cpp owns everything
// Rack-facing around it: reading the ports, the RTRG Schmitt triggers, the
// retrigger pulses and the outputs. See the comment atop Zones.cpp for the
// classification rules themselves.
struct ZonesTracker {
    enum Priority {
        PRIORITY_LAST,
        PRIORITY_HIGH,
        PRIORITY_LOW,
        NUM_PRIORITIES
    };

    // Channel indices, oldest first. Fixed capacity (one slot per input
    // channel, and a channel is never held twice) so process() never
    // allocates.
    struct HeldList {
        int channels[16];
        int count = 0;

        bool empty() const {
            return count == 0;
        }

        void clear() {
            count = 0;
        }

        void push(int c) {
            if (count < 16) {
                channels[count++] = c;
            }
        }

        void remove(int c) {
            int kept = 0;
            for (int i = 0; i < count; i++) {
                if (channels[i] != c) {
                    channels[kept++] = channels[i];
                }
            }
            count = kept;
        }

        const int* begin() const {
            return channels;
        }

        const int* end() const {
            return channels + count;
        }
    };

    struct Zone {
        int priority = PRIORITY_LAST;
        // Channels currently held and classified into this zone.
        HeldList held;
    };

    Zone zones[2]; // 0 = A, 1 = B

    // Per input-channel state.
    int channelZone[16];         // -1 = unclassified, 0 = A, 1 = B
    bool prevGateHigh[16];
    // True for the specific channel whose note-on was consumed to set the
    // split point via LEARN — that note itself never plays through (only
    // notes played after LEARN turns off do), so it stays excluded from
    // topOf()/passthrough until its own note-off releases the channel.
    bool learnSuppressed[16];
    // Global note-on order across both zones (each zone's `held` is only
    // ordered within itself), for DUO voice A's "Last note" priority.
    uint64_t pressOrder[16] = {};
    uint64_t pressCounter = 0;

    ZonesTracker() {
        reset();
    }

    void reset() {
        for (int c = 0; c < 16; c++) {
            channelZone[c] = -1;
            prevGateHigh[c] = false;
            learnSuppressed[c] = false;
        }
        zones[0].held.clear();
        zones[1].held.clear();
    }

    bool anyHeld() const {
        return !zones[0].held.empty() || !zones[1].held.empty();
    }

    // Selects which held channel is currently "on top" (audible) for a zone,
    // per its priority rule. Returns -1 if the zone has no (non-suppressed)
    // held notes.
    int topOf(int z, const float* pitch) const {
        const Zone& zone = zones[z];
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
                            ? pitch[c] > pitch[best]
                            : pitch[c] < pitch[best]);
                    if (better) {
                        best = c;
                    }
                }
                return best;
            }
            default: // PRIORITY_LAST
                for (int i = zone.held.count - 1; i >= 0; i--) {
                    if (!learnSuppressed[zone.held.channels[i]]) {
                        return zone.held.channels[i];
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
        zones[0].held.remove(c);
        zones[1].held.remove(c);
        channelZone[c] = zone;
        zones[zone].held.push(c);
        pressOrder[c] = ++pressCounter;
    }

    void release(int c) {
        int z = channelZone[c];
        if (z >= 0) {
            zones[z].held.remove(c);
        }
        channelZone[c] = -1;
    }

    // One sample of channel classification. `pitch`/`gateHigh`/`rtrgFired`
    // hold the first `channels` input channels. While `learnArmed`, the
    // first note-on sets `threshold` (clamped to [splitMin, splitMax], the
    // SPLIT knob's own range, so what's shown, what's played and what a
    // reload restores all agree) and disarms; returns true in that case so
    // the caller can write it back to the knob. retrig[z] is set when zone
    // z's audible (mono) note just became a fresh note-on.
    bool processChannels(int channels, const float* pitch, const bool* gateHigh, const bool* rtrgFired,
                         float& threshold, bool& learnArmed, float splitMin, float splitMax, bool* retrig) {
        bool learned = false;
        retrig[0] = retrig[1] = false;

        for (int c = 0; c < channels; c++) {
            // Note-off: drop the channel from whichever zone holds it.
            if (!gateHigh[c] && prevGateHigh[c]) {
                release(c);
            }

            // Defensive fallback: a channel can be gate-high with no
            // classification yet (e.g. patch loaded mid-note) without ever
            // seeing its RTRG edge.
            bool needsClassification = gateHigh[c] && channelZone[c] < 0;

            if (rtrgFired[c] || needsClassification) {
                bool isLearnNote = false;
                if (learnArmed) {
                    threshold = pitch[c] < splitMin ? splitMin : pitch[c] > splitMax ? splitMax : pitch[c];
                    learnArmed = false;
                    learned = true;
                    isLearnNote = true;
                }
                int zone = (pitch[c] >= threshold) ? 1 : 0;
                classify(c, zone);
                // The note that just set the split point stays excluded from
                // topOf()/passthrough (see learnSuppressed) until its own
                // note-off — only notes played after LEARN turns off actually
                // reach the outputs.
                learnSuppressed[c] = isLearnNote;
                if (rtrgFired[c] && topOf(zone, pitch) == c) {
                    retrig[zone] = true;
                }
            }
            else if (gateHigh[c] && channelZone[c] >= 0) {
                // No RTRG edge, but this already-classified channel's own
                // live pitch just crossed the split point — only reachable
                // from a raw mono CV/Gate source with no retrigger line (see
                // the comment atop Zones.cpp); real polyphonic input never
                // changes a still-gated channel's pitch, so this is a no-op
                // there.
                int zone = (pitch[c] >= threshold) ? 1 : 0;
                if (zone != channelZone[c]) {
                    classify(c, zone);
                    if (topOf(zone, pitch) == c) {
                        retrig[zone] = true;
                    }
                }
            }

            prevGateHigh[c] = gateHigh[c];
        }

        // Channels beyond the current poly width just disappeared (or were
        // never there): release any note they still held — otherwise a
        // channel-count drop mid-note leaves it stuck in its zone forever,
        // since its GATE can no longer be seen falling — and clear their
        // tracked state so a later channel-count increase isn't seen as a
        // spurious note-off.
        for (int c = channels; c < 16; c++) {
            release(c);
            prevGateHigh[c] = false;
            learnSuppressed[c] = false;
        }

        return learned;
    }

    // DUO mode: highest held (non-suppressed) note across both zones, and
    // how many such notes are held in total.
    int duoHighest(const float* pitch, int* count) const {
        int highest = -1;
        *count = 0;
        for (const Zone& zone : zones) {
            for (int c : zone.held) {
                if (learnSuppressed[c]) {
                    continue;
                }
                (*count)++;
                // >= so two held channels at the same pitch still map to two
                // distinct voices.
                if (highest < 0 || pitch[c] >= pitch[highest]) {
                    highest = c;
                }
            }
        }
        return highest;
    }

    // DUO mode: picks voice A among all held notes except `exclude` (voice
    // B's), per Zone A's priority.
    int duoPickA(int exclude, const float* pitch) const {
        int best = -1;
        for (const Zone& zone : zones) {
            for (int c : zone.held) {
                if (learnSuppressed[c] || c == exclude) {
                    continue;
                }
                bool better;
                switch (zones[0].priority) {
                    case PRIORITY_HIGH:
                        better = best < 0 || pitch[c] > pitch[best];
                        break;
                    case PRIORITY_LOW:
                        better = best < 0 || pitch[c] < pitch[best];
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
};
