# Aluminium — User Manual

**Aluminium** is a small collection of MIDI-CV utility modules for VCV Rack / Cardinal, all built around one goal: getting more out of real MIDI keyboards and controllers in a live performance patch, beyond what Rack's core **MIDI-CV** module alone gives you. That covers a wide range of setups — from the simplest one, owning a single MIDI master keyboard on one channel and wanting to split it into two or more independently-playable zones, all the way up to a rig built around an advanced controller with velocity- and polyphonic-aftertouch-sensitive pads, where every pad becomes its own fully expressive CV voice.

![All Aluminium panels, Dark theme](docs/images/AllModules_Dark.png)

- **Al Splitter** turns that one keyboard into as many independent note-range zones as you want. Chain several Al Splitters together (feed one zone's output into the next Al Splitter's input) and you can cut the keyboard into 3, 4, or more zones — each usable on its own as a polyphonic or true-legato monophonic voice.
- **Al Gate** and its expander (**Al Gate Expander**) exist for live performance with MIDI pad controllers: patch in a controller whose pads are velocity- and/or polyphonic-aftertouch-sensitive, learn each pad once, and every pad becomes its own independent, fully expressive CV voice — gate, velocity, aftertouch, whatever lane you need — with no MIDI-mapping software and no hand-built V/OCT comparison logic. The same trick works just as well on a handful of keys reserved off the end of an ordinary keyboard (after a split, if the rest is still being played melodically), and nothing ties either module to a single input — feed one Al Gate from one controller's pads while an Al Splitter zone handles another controller's keys, and build a full live rig from several MIDI controllers at once, all inside the same patch.

| Module                            | What it does                                                                    |
|------------------------------------|----------------------------------------------------------------------------------|
| [Al Splitter](#al-splitter)       | Splits one polyphonic MIDI-CV stream into two note-range zones, each poly or mono |
| [Al Gate](#al-gate)               | 16 gate outputs, one per learned note, from a poly V/OCT + GATE cable            |
| [Al Gate Expander](#al-gate-expander) | Al Gate expander: mirrors its note mapping onto a freely-labeled poly lane |

## Table of Contents

- [Panel themes](#panel-themes)
- [Right-click menu](#right-click-menu)
- [Shared conventions](#shared-conventions)
- [Al Splitter](#al-splitter)
- [Al Gate](#al-gate)
- [Al Gate Expander](#al-gate-expander)

## Panel themes

Every Aluminium panel follows one shared **Follow Rack / Light / Dark** setting (see [Right-click menu](#right-click-menu) below), independent of Rack's own "Dark panels" preference. Unlike a per-module theme, this one is shared pack-wide — there's only ever one Aluminium theme active at a time, and it applies to every Aluminium module.

### Light

![All Aluminium panels, Light theme](docs/images/AllModules_Light.png)

### Dark

![All Aluminium panels, Dark theme](docs/images/AllModules_Dark.png)

## Right-click menu

Right-click any Aluminium module to open its context menu. One entry is identical across all 3 modules:

- **Aluminium theme** — a submenu to switch between *Follow Rack*, *Light*, and *Dark*. Because this setting is shared pack-wide (see [Panel themes](#panel-themes) above), changing it from **any** Aluminium module's menu re-themes **every** Aluminium module currently in the patch, in one step — there's no separate "apply to all" action to trigger, it always applies to the whole pack, immediately.

![Aluminium theme submenu](docs/images/Menu_Theme.png)

Al Splitter adds two more entries of its own (documented under [Al Splitter](#al-splitter)), and Al Gate adds one more (documented under [Al Gate](#al-gate)). Al Gate Expander has no other entries — its label is edited directly on the panel, not via the menu.

## Shared conventions

- **V/OCT.** Pitch follows the standard 1 volt per octave convention (0V = C4), like any other Rack oscillator or sequencer.
- **Where inputs come from.** Every Aluminium input in this pack expects the polyphonic outputs of Rack/Cardinal's own core **MIDI-CV** module (V/OCT, GATE, VELOCITY, AFTERTOUCH, RETRIGGER) — either patched directly, or via Al Splitter's own zone outputs, which carry the same 5 signals split by note range.
- **16-channel cap.** Every module in this pack follows Rack's own 16-channel polyphony limit — inputs/zones/cells beyond the 16th incoming channel are simply not read.

## Al Splitter

![Al Splitter panel](docs/images/AlSplitter.png)

Splits one polyphonic MIDI-CV stream into two note-range zones — Zone A (below the split point) and Zone B (at/above it) — each independently switchable between a straight polyphonic passthrough and a single monophonic voice.

**Mono mode is the real reason this module exists.** A single MIDI keyboard is polyphonic hardware, but a huge amount of classic synth voicing — basslines, leads, anything meant to be played legato — wants a genuinely monophonic CV/gate pair instead. Naively reducing a polyphonic stream to one voice usually retriggers the envelope on *every* new key-press, including the moment you release an overlapping note and the synth voice falls back to whichever other note was still being held — which is exactly the kind of glitch that makes rolled chords or legato phrasing sound broken. Zone A/B's Mono switch avoids that entirely: gate stays high continuously across overlapping held notes (true legato, only dropping once the zone is completely empty), and a retrigger pulse fires only for a genuinely new key-press — never when the audible note merely falls back to one already held. That's what makes a single ordinary keyboard usable as a proper monophonic performance controller, split into as many independently-played zones as you like.

**Knobs & switches**
- **Split point** — the note at/above which incoming notes fall into Zone B (below it, Zone A). Its live note-name + frequency readout above the knob doubles as a right-click-editable text field: type a note name (`C4`, `F#3`, `Bb2`, …) directly instead of dialing it in.
- **Learn** (lit button) — arms the module to set the split point from the very next note played on the Pitch input, instead of dialing the knob by hand. That learning note itself is excluded from the outputs (it never plays through); only notes played after Learn turns back off do. Click the lit button again to cancel learning without playing a note.
- **Zone A mode** / **Zone B mode** (lit buttons) — *Polyphonic* (passthrough of that zone's own channels, unchanged channel indices, so downstream polyphonic modules never see a spurious retrigger) or *Monophonic* (a single, true-legato voice — see above — reduced per that zone's priority rule below).

**Inputs**
- **V/OCT**, **Gate**, **Velocity**, **Aftertouch**, **Retrigger** — the 5 polyphonic lanes from Core MIDI-CV (or an upstream Al Splitter's zone outputs).

**Outputs** (×2, one set per zone)
- **V/OCT**, **Gate**, **Velocity**, **Aftertouch**, **Retrigger** — Zone A's and Zone B's own copies of the 5 input lanes, filtered/reduced per that zone's mode.

**Lights**
- **Learn** (red) — lit while Learn is armed and waiting for a note.
- **Zone A / Zone B mode** (white, built into the mode buttons) — lit while that zone is in Monophonic mode.

**Context menu**
- **Zone A priority (when monophonic)** / **Zone B priority (when monophonic)** — which held note plays when that zone is in Monophonic mode and more than one note overlaps: *Last note*, *Highest note*, or *Lowest note*.

![Al Splitter's mono priority submenus](docs/images/AlSplitter_Menu_Priority.png)

**Patch ideas**
- The one-keyboard, one-MIDI-channel setup this module exists for: patch Core MIDI-CV straight into one Al Splitter to get a simple upper/lower keyboard split, or chain two or three together to build several independent zones — bass on the lowest, chords in the middle, lead on top — all from a single physical keyboard.
- Set a zone to Monophonic with *Highest note* priority for a simple top-note lead line while still holding chords underneath in the other (Polyphonic) zone.
- Set the bass zone to Monophonic with *Last note* priority and play it legato (overlapping key presses) for true monosynth-style basslines, gate and envelope behaving exactly like a real analog mono synth would expect.

## Al Gate

![Al Gate panel](docs/images/AlGate.png)

Its headline use case is live performance with a MIDI pad controller — an Akai MPD/MPC-style pad grid, a Novation Launchpad Pro, an Ableton Push, or any other velocity- and/or polyphonic-aftertouch-sensitive controller (see Patch ideas below): learn each pad once and it becomes its own independently expressive CV voice.

Technically, it's like Cardinal/Rack core's **MIDI-Gate** module, but driven by a polyphonic V/OCT + GATE cable instead of MIDI directly (typically from Core MIDI-CV, or from one of Al Splitter's zones) — 16 gate outputs, one per learned note. Each output goes high whenever any incoming polyphonic channel currently carries that exact note with its gate high.

**The note display**

The 2×8 grid in the middle is the whole module: each of its 16 cells shows the note currently learned for that gate output ("--" if unassigned), and is how you (re)assign it — two ways:

- **Learn by playing.** Click a cell — it highlights to show it's armed and waiting — then play the note you want on the incoming V/OCT + GATE cable; the cell captures it and stops waiting. Click the same (still-armed) cell again instead of playing a note to cancel the learn and leave it unchanged.
- **Type it in.** Click a cell to select it, then type the note directly: a letter key `A`–`G` sets the note name, `#` sharpens it, and a digit key sets the octave; press **Enter** to commit, or **Backspace**/**Delete** at any point to clear the cell back to "--" (that output then stays permanently low). Selecting a different cell or clicking elsewhere without pressing Enter discards whatever was typed so far.

Assigning a note to one cell automatically clears it from any other cell that held it — the same note is never mapped to two outputs at once. Fresh from the module browser (or after a reset), the 16 cells default to one chromatic run starting at C2 (cell 1 = C2, cell 2 = C#2, … up through cell 16 = D#3) — a reasonable starting point to re-learn from, not something to rely on.

The left column of ports (outputs 1–8, top to bottom) feeds from the display's left column of cells; the right column of ports (outputs 9–16) feeds from the display's right column, in the same top-to-bottom order.

**Inputs**
- **V/OCT**, **Gate** — the polyphonic pitch/gate pair to learn notes from and gate against (from Core MIDI-CV, or an Al Splitter zone).

**Outputs**
- **Gate 1–16** — one gate per learned cell, high (10V) whenever some incoming channel currently holds that exact note with its gate high, low (0V) otherwise (including for any cell left at "--").

**Context menu**

Adds an expander directly, already cabled and positioned, instead of pulling it from the module browser and connecting it by hand:

![Al Gate's add-expander menu items](docs/images/AlGate_Menu_AddExpander.png)

- **Add an expander** — creates an Al Gate Expander immediately to the right of Al Gate, or to the right of the last expander already chained there if one or more are already attached. Add as many as you need, one per poly lane, in any order — give each one its own name via its on-panel label field, see [Al Gate Expander](#al-gate-expander) below for how the chain works.

**Patch ideas**
- Patch a MIDI pad controller's Core MIDI-CV output into Al Gate, learn each pad once, then add an [Al Gate Expander](#al-gate-expander) per lane you want — one labeled "Velocity", one labeled "Aftertouch" — so gate triggers a drum module, velocity sets its level, and aftertouch modulates a filter, all live and independently per pad.
- No dedicated pad controller? Reserve a few keys at one end of an ordinary keyboard instead (after splitting them off with [Al Splitter](#al-splitter), if the rest is still being played melodically) and learn each one into Al Gate — every key becomes its own ready-to-patch gate output, without building your own per-note V/OCT comparison logic the way Core's own MIDI-Gate/MIDI-CV combination would otherwise take.
- Running more than one MIDI controller? Feed each into its own Al Gate (or Al Splitter zone) for a full live rig — drum pads on one controller, melodic zones on another — in the same patch at once.

## Al Gate Expander

![Al Gate Expander panel](docs/images/AlGateExpander.png)

An Al Gate expander: extracts whatever polyphonic cable is patched into its single input (from Core MIDI-CV, or an Al Splitter zone) into 16 mono outputs, one per note Al Gate has learned — output *N* always carries the value of whichever note is currently learned into Al Gate's cell *N*. The module itself doesn't care which lane that is (Velocity, Aftertouch, Retrigger, or anything else poly) — that's purely a matter of which cable you patch in.

Place it directly to the right of Al Gate — or to the right of another Al Gate Expander already chained there (see [Al Gate](#al-gate)'s context menu above); add as many instances as you need, one per lane, in any order — they all relay the same mapping onward to whichever others follow them.

**Label**
- The single-line field on the panel is a freely-editable text label (click it and type) — it doesn't affect the signal path at all, it's just how you keep track of what you patched into this instance (e.g. "Velocity", "Aftertouch", "Retrigger", or any name you like). It's empty by default, and saved with the patch.
- The first time you patch a cable into this instance's input while the label is still empty, it's auto-filled from that cable's source (its first 8 characters, e.g. patching Core MIDI-CV's Velocity output fills in "Velocity"). Unpatching that cable clears the label back to empty automatically — but only for a label that was auto-filled this way; once you've typed your own text over it, unpatching leaves it alone.

**Inputs**
- **Lane** — the polyphonic lane to read from, named after the label above.

**Outputs**
- **Lane 1–16** — that note's current value on the patched-in lane (0V while its cell is unassigned, its note isn't currently held, or the input's channel count doesn't reach that cell's channel — see Lights below).

**Lights** — one status LED, showing one of three colors at a time (hover it for a tooltip covering all three):
- **Yellow** — chained to Al Gate (directly or through other expanders), but nothing patched into this instance's own input yet.
- **Red** — chained and patched in, but this instance's own cable has a different channel count than Al Gate's V/OCT + Gate cable; outputs for cells beyond its channel count read 0V instead of whatever note Al Gate has learned there.
- **Green** — chained, patched in, and channel counts match: fully working.

**Patch ideas** — see [Al Gate](#al-gate) above.
