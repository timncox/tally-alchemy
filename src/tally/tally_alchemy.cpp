/*
 * Tally on the Hermetic Modular Alchemy Lab V2 -- a clean-room complex
 * oscillator in the spirit of fancyyyyy x grrrwaaa's K-ACCUMULATOR (no
 * affiliation, none of their code: see src/tally/DESIGN.md).
 *
 * The DSP is tally::Engine (tally_engine.cpp). This file is the host shim:
 * the K-ACCUMULATOR's 32 HP panel folded onto six pots, three buttons and
 * ten jacks.
 *
 *   OSC     MORPH . FREQ . SHIFT . DEPTH . SHAPE . DAMPED/PULSAR
 *   UFG     TIME . SKEW . U.SHAPE . STRETCH . LOOP/ONE-SHOT . TRIG TYPE
 *   PATTERN CHANCE . LENGTH . SMOOTH . PITCH att . TIME att . TZPM
 *   MODS    SHIFT att . DEPTH att . SHAPE att . MORPH att . DAMPED att . SOURCE
 *   ROOT    ROOT . SCALE . HARMONIC . ORDER . DETUNE . ROUTE        (hold B3)
 *   B1      UFG trigger (held = gate); on PATTERN: tap = new value at the
 *           step, double tap = undo it, hold 1 s = new pattern;
 *           with B3 held: scale system Off -> 12-TET -> JI
 *   B2      next page (OSC -> UFG -> PATTERN -> MODS)
 *   J1 EXT sync/track . J2 TZPM . J3 V/Oct (Root) . J7 UFG out . J8 D-S out
 *   J4 / J5 / J6 mappable CV (cv_map.h): any knob, Settings choice or action,
 *   with an amount; by default Morph / UFG trigger / Shape
 *   J9 SINE . J10 COSINE
 *   Settings (B2+B3 2 s): EXT mode, MOD tracks; page 1 = the SD picker and
 *   Q.TRIG; page 2 = the CV map (P1/P2 J4, P3/P4 J5, P5/P6 J6)
 */
#include <math.h>
#include <string.h>

#include "daisy_seed.h"
#include "util/CpuLoadMeter.h"

#include "alchemy/hw/alchemy_lab.h"
#include "alchemy/host_link/cdc_transport.h"
#include "alchemy/host_link/host.h"
#include "alchemy/storage/fs_extension.h"
#include "alchemy/storage/sd_card.h"
#include "alchemy/surface/control_loop.h"
#include "alchemy/surface/cv_matrix.h"
#include "alchemy/surface/jack.h"
#include "alchemy/surface/manual.h"
#include "alchemy/surface/page.h"
#include "alchemy/surface/pager.h"
#include "alchemy/surface/presets.h"
#include "alchemy/surface/settings.h"
#include "alchemy/surface/virtual_button.h"
#include "alchemy/surface/virtual_knob.h"

#include "mi_family.h"
#include "picker.h"
#include "cv_map.h"
#include "tally_engine.h"

#ifndef TALLY_VERSION
#define TALLY_VERSION "0.0.0"
#endif
#ifndef TALLY_GIT_HASH
#define TALLY_GIT_HASH "dev"
#endif

using namespace alchemy;
using daisy::System;

/* ---- geometry ----------------------------------------------------------- */

/* 24 frames at 48 kHz = 0.5 ms: the trigger and the J7/J8 outputs move
 * once per block. */
static constexpr uint32_t kBlockSize = 24u;

enum : uint8_t {
    kPageOsc = 0, kPageUfg = 1, kPagePattern = 2, kPageMods = 3, kPageRoot = 4,
    kNumAppPages = 5
};
enum : uint8_t { kSettingsMain = 0, kSettingsFirmware = 1, kSettingsCv = 2 };

/* CV indices: 0 = J3 ... 5 = J8. */
static constexpr uint8_t kCvVoct = 0u;
/* The three mappable jacks, J4-J6. */
static constexpr uint8_t kNumMapped = 3u;
static constexpr uint8_t kCvMapped[kNumMapped] = {1u, 2u, 3u};
static constexpr uint8_t kCvUfgOut = 4u, kCvDsOut = 5u;

/* ---- hardware + engine -------------------------------------------------- */

static AlchemyLab          hw;
static daisy::CpuLoadMeter cpu;
static tally::Engine       engine;

/* ---- colours ------------------------------------------------------------ */

/* The guide's LED code for the morph ring: green = self-feedback, yellow =
 * PM from the Mod, red = cross PM. */
static constexpr LedPanel::Rgb kTopo[3] = {
    {0x00, 0xFF, 0x30}, {0xFF, 0xB0, 0x00}, {0xFF, 0x10, 0x10}};

static constexpr LedPanel::Rgb kColFreqU  = {0xFF, 0xC0, 0x60};
static constexpr LedPanel::Rgb kColFreqD  = {0x60, 0xC0, 0xFF};
static constexpr LedPanel::Rgb kColShift  = {0x60, 0xFF, 0xC0};
static constexpr LedPanel::Rgb kColDepth  = {0xFF, 0x60, 0xA0};
static constexpr LedPanel::Rgb kColShape  = {0xC0, 0x60, 0xFF};
static constexpr LedPanel::Rgb kColDamped = {0xFF, 0xFF, 0x80};
static constexpr LedPanel::Rgb kColUfg    = {0x40, 0xC0, 0xFF};
static constexpr LedPanel::Rgb kColDs     = {0xFF, 0x80, 0x20};
static constexpr LedPanel::Rgb kColAttP   = {0xFF, 0x90, 0x20};
static constexpr LedPanel::Rgb kColAttN   = {0x20, 0x90, 0xFF};
static constexpr LedPanel::Rgb kColRoot   = {0xFF, 0xE0, 0xA0};
static constexpr LedPanel::Rgb kColNotch  = {0xFF, 0xFF, 0xFF};
static constexpr LedPanel::Rgb kColDim    = {0x18, 0x18, 0x18};

static constexpr LedPanel::Rgb kIdle  = {0x00, 0x00, 0x30};
static constexpr LedPanel::Rgb kRed   = {0xFF, 0x00, 0x00};

/* Page tints (B2 shows the page). */
static constexpr LedPanel::Rgb kPageCol[kNumAppPages] = {
    {0x00, 0xFF, 0x40}, {0x40, 0xC0, 0xFF}, {0xFF, 0x80, 0x20}, {0xC0, 0x60, 0xFF},
    {0xFF, 0xE0, 0xA0}};

/* B3 shows the scale system: dim off, blue 12-TET, gold JI. */
static constexpr LedPanel::Rgb kScaleCol[3] = {
    {0x50, 0x50, 0x50}, {0x30, 0x60, 0xFF}, {0xFF, 0xB0, 0x20}};

static const char* const kModeLabels[2]    = {"Loop", "One-shot"};
static const char* const kTrigLabels[4]    = {"Sync", "Reverse", "Sustain + sync", "Sustain + reverse"};
static const char* const kSourceLabels[2]  = {"UFG", "Delta-sigma"};
static const char* const kOrderLabels[5]   = {"Free", "Even", "All", "Odd", "Quantised"};
static const char* const kRouteLabels[4]   = {"OSC", "OSC + UFG", "UFG", "None"};
static const char* const kScaleLabels[9]   = {
    "Octaves", "Fifths / 3:2", "Pentatonic / PM ratios", "Major / JI major",
    "Densest: chromatic / 32 harmonics", "Whole tone / 7-limit",
    "Octatonic / utonal", "Hirajoshi / 11-limit", "Octaves"};
static const char* const kExtLabels[3]     = {"Off", "Sync", "Track"};
static const char* const kTrackLabels[3]   = {"Root", "OSC", "UFG"};
static const char* const kOnOffLabels[2]   = {"Off", "On"};

/* ---- pages --------------------------------------------------------------- */

/* Panel positions, front view:
 *      [P1] B1 [P2]
 *      [P3] B2 [P4]
 *      [P5] B3 [P6]                                                        */

static void MorphRing(LedPanel& panel, uint8_t pot, const ArcGeometry& geo,
                      float norm, uint32_t t_ms, void* ctx);

/* OSC */
static VirtualKnob morph = VirtualKnob(kPotTopLeft, "Morph")
    .Linear(0.f, 1.f).Ident("morph")
    .Help("Position on the waveshaping ring, once round from FMNT through "
          "FBPM2, 2OP2, XPM2 to ASYM (noon) and back through XPM, 2OP, FBPM. "
          "Green nodes: self-feedback PM; yellow: PM from the Mod; red: "
          "cross PM. J4 adds 1.6 nodes per volt.")
    .Ring(Custom(MorphRing));

static VirtualKnob freq = VirtualKnob(kPotTopRight, "Freq")
    .Linear(-1.f, 1.f).Ident("freq")
    .Help("OSC pitch, +-3 octaves: relative to Root when Root is routed to "
          "the OSC (whole semitones while a scale is on), about C4 otherwise. "
          "In EXT Track mode, a ratio to the tracked pitch.")
    .Ring(Bipolar(kColFreqU, kColFreqD, kColNotch));

static VirtualKnob shift = VirtualKnob(kPotMiddleLeft, "Shift")
    .Linear(0.f, 1.f).Ident("shift")
    .Help("Waveshaping. On most nodes, blended harmonic frequency shifting: "
          "a crossfade up the harmonic series of the fundamental.")
    .Ring(Level(kColShift));

static VirtualKnob depth = VirtualKnob(kPotMiddleRight, "Depth")
    .Linear(0.f, 1.f).Ident("depth")
    .Help("Waveshaping. On most nodes, phase modulation with damping: self "
          "feedback on the green nodes, from the Mod on yellow and red.")
    .Ring(Level(kColDepth));

static VirtualKnob shape = VirtualKnob(kPotBottomLeft, "Shape")
    .Linear(0.f, 1.f).Ident("shape")
    .Help("Waveshaping. On most nodes, harmonic wavefolding.")
    .Ring(Level(kColShape));

static VirtualKnob damped = VirtualKnob(kPotBottomRight, "Damped")
    .Linear(0.f, 1.f).Ident("damped")
    .Help("Damped sync and AM from the UFG: from gentle animation through "
          "PLL-like locking to hard sync and full gating at the top. With a "
          "sub-audio UFG, enveloped hits; at audio rate, pulsar synthesis.")
    .Ring(Level(kColDamped));

/* UFG */
static VirtualKnob ufg_time = VirtualKnob(kPotTopLeft, "Time")
    .Linear(0.f, 1.f).Ident("time")
    .Help("UFG rate: 0.02 Hz to ~4 kHz free, or -8 to +2 octaves about Root "
          "when Root is routed to the UFG. J6 can add 1 V/oct.")
    .Ring(Level(kColUfg));

static VirtualKnob ufg_skew = VirtualKnob(kPotTopRight, "Skew")
    .Linear(0.f, 1.f).Ident("skew")
    .Help("Tilts the function from falling ramp to rising ramp; triangle at "
          "noon. Also the gate's pulse width.")
    .Ring(Bipolar(kColFreqU, kColFreqD, kColNotch));

static VirtualKnob ufg_shape = VirtualKnob(kPotMiddleLeft, "U.Shape")
    .Linear(0.f, 1.f).Ident("ufg_shape")
    .Help("The UFG's curve: linear, sine, exponential, raised cosine.")
    .Ring(Level(kColUfg));

static VirtualKnob stretch = VirtualKnob(kPotMiddleRight, "Stretch")
    .Linear(0.f, 1.f).Ident("stretch")
    .Help("Harmonic stretching: moves the waveshaped harmonics (from SHIFT "
          "and SHAPE) up while the fundamental stays. No effect without "
          "waveshaping.")
    .Ring(Level(kColShape));

static VirtualKnob ufg_mode = VirtualKnob(kPotBottomLeft, "Loop")
    .Selector(2).Labels(kModeLabels).Ident("ufg_mode")
    .Ring(SelectorRing(kColUfg, kColDim, 2));

static VirtualKnob trig_type = VirtualKnob(kPotBottomRight, "Trig type")
    .Selector(4).Labels(kTrigLabels).Ident("trig_type")
    .Help("How J5 / B1 act on the UFG: hard sync (lets the cycle finish, so "
          "fast triggers lock it to a subharmonic), reverse direction, and "
          "the same two holding at the peak while the gate is high.")
    .Ring(SelectorRing(kColUfg, kColDim, 4));

/* PATTERN */
static VirtualKnob chance = VirtualKnob(kPotTopLeft, "Chance")
    .Linear(0.f, 1.f).Ident("chance")
    .Help("Probability of a new value at each step. Zero is a locked loop; "
          "the stored pattern is never overwritten, so turning back to zero "
          "always restores it.")
    .Ring(Level(kColDs));

static VirtualKnob length = VirtualKnob(kPotTopRight, "Length")
    .Linear(0.f, 1.f).Ident("length")
    .Help("2 to 16 steps, 8 in the dead zone at noon. A zoom into the "
          "pattern starting where the change landed.")
    .Ring(Level(kColDs));

static VirtualKnob smooth = VirtualKnob(kPotMiddleLeft, "Smooth")
    .Linear(0.f, 1.f).Ident("smooth")
    .Help("Glide between steps. The steps that glide are fixed per pattern; "
          "past noon every step slews. At audio rate, a filter.")
    .Ring(Level(kColDs));

static VirtualKnob pitch_att = VirtualKnob(kPotMiddleRight, "Pitch")
    .Linear(-1.f, 1.f).Ident("pitch_att")
    .Help("Delta-sigma to OSC pitch (the 1V/TZ attenuverter's normal), "
          "+-2 octaves; quantised with Root when a scale is on.")
    .Ring(Bipolar(kColAttP, kColAttN, kColNotch));

static VirtualKnob time_att = VirtualKnob(kPotBottomLeft, "Time mod")
    .Linear(-1.f, 1.f).Ident("time_att")
    .Help("A random walk on the UFG's TIME, up to +-3 octaves.")
    .Ring(Bipolar(kColAttP, kColAttN, kColNotch));

static VirtualKnob tzpm = VirtualKnob(kPotBottomRight, "TZPM")
    .Linear(0.f, 1.f).Ident("tzpm")
    .Help("Depth of the through-zero phase modulation from J2.")
    .Ring(Level(kColAttP));

/* MODS: the attenuverters for the bottom row's internal normals */
static VirtualKnob shift_att = VirtualKnob(kPotTopLeft, "Shift mod")
    .Linear(-1.f, 1.f).Ident("shift_att").Ring(Bipolar(kColAttP, kColAttN, kColNotch));
static VirtualKnob depth_att = VirtualKnob(kPotTopRight, "Depth mod")
    .Linear(-1.f, 1.f).Ident("depth_att").Ring(Bipolar(kColAttP, kColAttN, kColNotch));
static VirtualKnob shape_att = VirtualKnob(kPotMiddleLeft, "Shape mod")
    .Linear(-1.f, 1.f).Ident("shape_att").Ring(Bipolar(kColAttP, kColAttN, kColNotch));
static VirtualKnob morph_att = VirtualKnob(kPotMiddleRight, "Morph mod")
    .Linear(-1.f, 1.f).Ident("morph_att").Ring(Bipolar(kColAttP, kColAttN, kColNotch));
static VirtualKnob damped_att = VirtualKnob(kPotBottomLeft, "Damped mod")
    .Linear(-1.f, 1.f).Ident("damped_att").Ring(Bipolar(kColAttP, kColAttN, kColNotch));
static VirtualKnob mod_source = VirtualKnob(kPotBottomRight, "Source")
    .Selector(2).Labels(kSourceLabels).Ident("mod_source")
    .Help("What the five attenuverters on this page scale: the UFG or the "
          "delta-sigma output.")
    .Ring(SelectorRing(kColDs, kColDim, 2));

/* ROOT (hold B3) */
static VirtualKnob root = VirtualKnob(kPotTopLeft, "Root")
    .Linear(-1.f, 1.f).Ident("root")
    .Help("The tuning reference, +-3 octaves about C4; J3 adds 1 V/oct. "
          "Quantised to the scale when one is on (B1 with B3 held).")
    .Ring(Bipolar(kColRoot, kColFreqD, kColNotch));

static VirtualKnob scale = VirtualKnob(kPotTopRight, "Scale")
    .Selector(9).Labels(kScaleLabels).Ident("scale")
    .Help("Octaves at both ends, the densest scale at noon, standard scales "
          "to the left and exotic ones to the right. Each label names the "
          "12-TET / JI pair.")
    .Ring(SelectorRing(kColRoot, kColDim, 9));

static VirtualKnob harmonic = VirtualKnob(kPotMiddleLeft, "Harmonic")
    .Linear(-1.f, 1.f).Ident("harmonic")
    .Help("The Mod's pitch relative to what it tracks: above noon through "
          "the harmonics (ORDER picks which), below noon a free drop of up "
          "to four octaves.")
    .Ring(Bipolar(kColShift, kColFreqD, kColNotch));

static VirtualKnob order = VirtualKnob(kPotMiddleRight, "Order")
    .Selector(5).Labels(kOrderLabels).Ident("order")
    .Help("HARMONIC's response: a free +-4 octave offset; blended even, all "
          "or odd harmonics; or quantised to the scale (integer ratios with "
          "no scale).")
    .Ring(SelectorRing(kColShift, kColDim, 5));

static VirtualKnob detune = VirtualKnob(kPotBottomLeft, "Detune")
    .Linear(-1.f, 1.f).Ident("detune")
    .Help("Detunes the Mod, up to a semitone, finer near noon; also drifts "
          "the wavefolder's harmonics, which animates SHAPE.")
    .Ring(Bipolar(kColFreqU, kColFreqD, kColNotch));

static VirtualKnob route = VirtualKnob(kPotBottomRight, "Route")
    .Selector(4).Labels(kRouteLabels).Ident("route")
    .Help("Where Root goes: the OSC, both, the UFG, or neither.")
    .Ring(SelectorRing(kColRoot, kColDim, 4));

static Page osc_page = Page(kPageOsc).Name("OSC").Color("#00ff40")
    .Knobs(morph, freq, shift, depth, shape, damped);
static Page ufg_page = Page(kPageUfg).Name("UFG").Color("#40c0ff")
    .Knobs(ufg_time, ufg_skew, ufg_shape, stretch, ufg_mode, trig_type);
static Page pattern_page = Page(kPagePattern).Name("Pattern").Color("#ff8020")
    .Knobs(chance, length, smooth, pitch_att, time_att, tzpm);
static Page mods_page = Page(kPageMods).Name("Mods").Color("#c060ff")
    .Knobs(shift_att, depth_att, shape_att, morph_att, damped_att, mod_source);
static Page root_page = Page(kPageRoot).Name("Root").Color("#ffe0a0")
    .Knobs(root, scale, harmonic, order, detune, route);

/* ---- descriptor metadata -------------------------------------------------- */

static Jack jk_ext   ("J1",  "Ext sync/track", JackSig::AudioIn);
static Jack jk_tz    ("J2",  "TZPM",           JackSig::AudioIn);
static Jack jk_voct  ("J3",  "V/Oct (Root)",   JackSig::Voct);
static Jack jk_cv4   ("J4",  "CV A (mappable; Morph)",       JackSig::CvBi);
static Jack jk_cv5   ("J5",  "CV B (mappable; UFG trigger)", JackSig::CvBi);
static Jack jk_cv6   ("J6",  "CV C (mappable; Shape)",       JackSig::CvBi);
static Jack jk_ufg   ("J7",  "UFG out",        JackSig::CvUni);
static Jack jk_ds    ("J8",  "Delta-sigma out", JackSig::CvUni);
static Jack jk_sin   ("J9",  "Sine",           JackSig::AudioOut);
static Jack jk_cos   ("J10", "Cosine",         JackSig::AudioOut);

static VirtualButton bt_b1 = VirtualButton("b1", "Trigger / Pattern")
    .Action("Press", "Trigger the UFG; held, its gate")
    .Action("Tap (Pattern page)", "A new value at the current step; double tap undoes it")
    .Action("Hold 1 s (Pattern page)", "A whole new pattern")
    .Action("With B3 held", "Scale system: off, 12-TET, JI");

static VirtualButton bt_b2 = VirtualButton("b2", "Page")
    .Action("Tap", "OSC -> UFG -> Pattern -> Mods");

static VirtualButton bt_b3 = VirtualButton("b3", "Root")
    .Action("Hold", "Show the Root / Mod page");

static Manual manual = Manual()
    .Tagline("Complex oscillator: morphing waveshaping ring, Mod, UFG, delta-sigma patterns")
    .Preamble(
        "**Tally** is a sine/cosine oscillator (J9/J10, a stereo pair) "
        "with a waveshaping ring. MORPH moves round eight nodes; SHIFT, "
        "DEPTH and SHAPE are the waveshapers, and with all three down "
        "(and DAMPED down) you are at Centre: a pure sine and cosine "
        "wherever MORPH is. B2 steps through the UFG (a function generator "
        "that is the modulation source, pulsar window and pattern clock), "
        "PATTERN (a delta-sigma sequencer fed by the Mod) and MODS (the "
        "attenuverters). Hold B3 for Root and the Mod oscillator. In the "
        "spirit of fancyyyyy x grrrwaaa's K-ACCUMULATOR; no affiliation, "
        "none of their code.");

/* ---- persisted extras: CPU readout, scale system, the pattern -------------- */

struct TallyExtras : public Serializable
{
    float    cpu_peak = 0.f, cpu_avg = 0.f;
    uint8_t  scale_system = tally::kScaleOff;
    uint8_t  levels[tally::kMaxSteps] = {};
    uint16_t glide = 0;
    bool     have_pattern = false;

    size_t SerializedSize() const override { return 8u + 1u + tally::kMaxSteps + 2u + 1u; }

    void Serialize(uint8_t* out) const override
    {
        memcpy(out, &cpu_peak, 4u);
        memcpy(out + 4u, &cpu_avg, 4u);
        out[8] = scale_system;
        memcpy(out + 9u, levels, tally::kMaxSteps);
        memcpy(out + 9u + tally::kMaxSteps, &glide, 2u);
        out[11u + tally::kMaxSteps] = have_pattern ? 1u : 0u;
    }

    bool Deserialize(const uint8_t* in) override
    {
        float p;
        memcpy(&p, in, 4u);
        cpu_peak = (p >= 0.f && p <= 1.f) ? p : 0.f;
        memcpy(&p, in + 4u, 4u);
        cpu_avg = (p >= 0.f && p <= 1.f) ? p : 0.f;
        scale_system = in[8] <= tally::kScaleJi ? in[8] : tally::kScaleOff;
        memcpy(levels, in + 9u, tally::kMaxSteps);
        memcpy(&glide, in + 9u + tally::kMaxSteps, 2u);
        have_pattern = in[11u + tally::kMaxSteps] == 1u;
        return true;
    }

    /* 'TLY' << 8 | layout version. */
    uint32_t SchemaHash() const override { return (0x544C59u << 8) | 0x01u; }

    bool Track(daisy::CpuLoadMeter& meter, float saved_peak)
    {
        float mx = meter.GetMaxCpuLoad();
        if (mx > 1.f) mx = 1.f;
        if (mx > cpu_peak) cpu_peak = mx;
        float av = meter.GetAvgCpuLoad();
        if (av > 1.f) av = 1.f;
        if (av > cpu_avg) cpu_avg = av;
        return cpu_peak > saved_peak + 0.05f;
    }
};

/* ---- surfaces ------------------------------------------------------------ */

static ControlLoop loop(hw);
static Pager       pager = Pager(kNumAppPages, kNumPots)
                               .Cycle(hw.buttons[kButtonB2], kPageOsc, kPageUfg, kPagePattern, kPageMods)
                               .Shift(hw.buttons[kButtonB3], kPageRoot);

/* Home slot 2: 3 is Stencil, 4-15 the other family firmwares (memory
 * project_break_alchemy has the card's map), 0 is the migration slot. */
static mi::HomeSlot home(2u);
static Presets      presets(hw.seed.qspi);
static Settings     settings(hw, &pager);
static CvMatrix     cv_matrix(kNumCvInputs);
static SdCard       sd;
static hostlink::FsExtension fs_ext(sd);
static hostlink::Host host(presets, "tally_alchemy", "Tally",
                           TALLY_VERSION, TALLY_GIT_HASH);
static TallyExtras  extras;
static SelectorHandle s_ext, s_track, s_qtrig;
static SelectorHandle s_dest[kNumMapped];
static KnobHandle     s_amount[kNumMapped];

#ifdef TALLY_BENCH_USB
static daisy::UsbHandle          bench_usb;
static hostlink::CdcUsbTransport bench_cdc;
#endif

/* ---- control frame -> audio block ---------------------------------------- */

/* The control thread writes the panel here; the callback copies it once
 * per block. Fields are single words: a torn struct is a one-block mix of
 * old and new values, never a torn float. */
static tally::Params    P;

/* The CV map, set by the control thread from Settings page 2. */
static volatile uint8_t g_dest[kNumMapped]   = {tally::kDestMorph, tally::kDestTrigger, tally::kDestShape};
static volatile float   g_amount[kNumMapped] = {1.f, 1.f, 1.f};
static volatile bool    g_b1_gate = false;

/* Pattern requests: made by the control thread, run by the callback (the
 * engine is only ever touched from the audio thread). */
static volatile bool g_req_write = false, g_req_restore = false, g_req_new = false;
static volatile bool g_pattern_dirty = false;

/* Read back by the LEDs. */
static volatile float G_MORPH = 0.f, G_UFG = 0.f, G_DS = 0.f;
static volatile bool  G_GATE = false;

/* ---- buttons (1 ms poll) -------------------------------------------------- */

static mi::Swallow swallow;
static bool        b1_down = false, b1_long_fired = false;
static uint32_t    b1_t0 = 0, b1_last_tap = 0;
static volatile bool g_scale_tap = false;

static void OnPoll(uint32_t now)
{
    if (settings.IsActive())
    {
        g_b1_gate = false;
        b1_down   = false;
        swallow.All();
        return;
    }
    const bool b1 = swallow.Live(hw, kButtonB1);
    const bool b3 = swallow.Live(hw, kButtonB3);
    const uint8_t page = pager.Page();

    if (b1 && !b1_down) { b1_down = true; b1_t0 = now; b1_long_fired = false; }

    if (b3 || page == kPageRoot)
    {
        /* With B3 held B1 is the scale-system button. */
        if (!b1 && b1_down) g_scale_tap = true;
        g_b1_gate = false;
    }
    else if (page == kPagePattern)
    {
        g_b1_gate = false;
        if (b1 && !b1_long_fired && now - b1_t0 >= 1000u)
        {
            b1_long_fired = true;
            g_req_new     = true;
        }
        if (!b1 && b1_down && !b1_long_fired)
        {
            if (b1_last_tap && now - b1_last_tap < 300u) { g_req_restore = true; b1_last_tap = 0; }
            else { g_req_write = true; b1_last_tap = now; }
        }
    }
    else
    {
        g_b1_gate = b1;
    }
    if (!b1) b1_down = false;
}

/* ---- control frame (~60 Hz) ------------------------------------------------ */

static mi::StoredWatch<kNumPots> watch[kNumAppPages];
static float g_saved_peak  = 0.0f;
static bool  prev_settings = false;
static int   prev_sel[3]   = {-1, -1, -1};
static float prev_map[2 * kNumMapped] = {-9.f, -9.f, -9.f, -9.f, -9.f, -9.f};

static int sel(VirtualKnob& k, int n)
{
    int v = (int)k.Value();
    return v < 0 ? 0 : (v >= n ? n - 1 : v);
}

static void OnFrame(void)
{
    const uint32_t now = System::GetNow();

    for (uint8_t pg = 0; pg < kNumAppPages; pg++)
        if (watch[pg].Changed(pager, pg)) home.Touch(now);

    if (g_scale_tap)
    {
        g_scale_tap = false;
        extras.scale_system = (uint8_t)((extras.scale_system + 1u) % 3u);
        home.Touch(now);
    }
    if (g_pattern_dirty)
    {
        g_pattern_dirty = false;
        engine.GetPattern(extras.levels, &extras.glide);   /* a benign race: worst case one step is stale */
        extras.have_pattern = true;
        home.Touch(now);
    }

    P.morph   = mi::clampf(morph.Value(), 0.f, 1.f);
    P.freq    = mi::clampf(freq.Value(), -1.f, 1.f);
    P.shift   = mi::clampf(shift.Value(), 0.f, 1.f);
    P.depth   = mi::clampf(depth.Value(), 0.f, 1.f);
    P.shape   = mi::clampf(shape.Value(), 0.f, 1.f);
    P.damped  = mi::clampf(damped.Value(), 0.f, 1.f);
    P.stretch = mi::clampf(stretch.Value(), 0.f, 1.f);

    P.time      = mi::clampf(ufg_time.Value(), 0.f, 1.f);
    P.skew      = mi::clampf(ufg_skew.Value(), 0.f, 1.f);
    P.ufg_shape = mi::clampf(ufg_shape.Value(), 0.f, 1.f);
    P.ufg_mode  = (uint8_t)sel(ufg_mode, 2);
    P.trig_type = (uint8_t)sel(trig_type, 4);

    P.chance    = mi::clampf(chance.Value(), 0.f, 1.f);
    P.length    = mi::clampf(length.Value(), 0.f, 1.f);
    P.smooth    = mi::clampf(smooth.Value(), 0.f, 1.f);
    P.pitch_att = mi::clampf(pitch_att.Value(), -1.f, 1.f);
    P.time_att  = mi::clampf(time_att.Value(), -1.f, 1.f);
    P.tzpm      = mi::clampf(tzpm.Value(), 0.f, 1.f);

    P.shift_att  = mi::clampf(shift_att.Value(), -1.f, 1.f);
    P.depth_att  = mi::clampf(depth_att.Value(), -1.f, 1.f);
    P.shape_att  = mi::clampf(shape_att.Value(), -1.f, 1.f);
    P.morph_att  = mi::clampf(morph_att.Value(), -1.f, 1.f);
    P.damped_att = mi::clampf(damped_att.Value(), -1.f, 1.f);
    P.mod_source = (uint8_t)sel(mod_source, 2);

    P.root      = mi::clampf(root.Value(), -1.f, 1.f);
    P.scale     = sel(scale, 9);
    P.harmonic  = mi::clampf(harmonic.Value(), -1.f, 1.f);
    P.order     = (uint8_t)sel(order, 5);
    P.detune    = mi::clampf(detune.Value(), -1.f, 1.f);
    P.root_send = (uint8_t)sel(route, 4);
    P.scale_system = extras.scale_system;

    const int v[3] = {(int)s_ext.Value(), (int)s_track.Value(), (int)s_qtrig.Value()};
    for (int i = 0; i < 3; i++)
    {
        if (prev_sel[i] >= 0 && v[i] != prev_sel[i]) home.Touch(now);
        prev_sel[i] = v[i];
    }
    P.ext_mode  = (uint8_t)(v[0] < 0 ? 0 : (v[0] > 2 ? 2 : v[0]));
    P.mod_track = (uint8_t)(v[1] < 0 ? 0 : (v[1] > 2 ? 2 : v[1]));
    P.qtrig     = v[2] == 1;

    /* The CV map: a destination and an amount (-1..+1) per jack. */
    for (uint8_t j = 0; j < kNumMapped; j++)
    {
        int d = (int)s_dest[j].Value();
        if (d < 0 || d >= tally::kNumDests) d = tally::kDestOff;
        const float a = mi::clampf(2.f * s_amount[j].Value() - 1.f, -1.f, 1.f);
        g_dest[j]   = (uint8_t)d;
        g_amount[j] = a;
        const float seen[2] = {(float)d, a};
        for (int k = 0; k < 2; k++)
        {
            float& pm = prev_map[2 * j + k];
            if (pm > -9.f && fabsf(seen[k] - pm) > 0.01f) home.Touch(now);
            pm = seen[k];
        }
    }

    if (extras.Track(cpu, g_saved_peak)) home.Touch(now);

    const bool sact = settings.IsActive();
    if (prev_settings && !sact) home.Touch(now);
    prev_settings = sact;

    if (home.Service(presets, hw, now, sact || picker::Busy()))
        g_saved_peak = extras.cpu_peak;
}

/* ---- LEDs ------------------------------------------------------------------ */

/*
 * The SDK draws the rings from the knobs. Added:
 *   MORPH ring  the eight nodes in their topology colours, the position bright
 *   B1 pair     the UFG's level (white), or on PATTERN the delta-sigma (orange)
 *   B2 pair     the page colour; red when the callback averages over 80 %
 *   B3 pair     the scale system: dim off, blue 12-TET, gold JI
 *   P1 ring     the previous session's worst CPU load, 2.5 s at boot
 */
static uint32_t g_readout_until = 0;
static float    g_boot_peak     = 0.0f;

static LedPanel::Rgb scaled(LedPanel::Rgb c, float k)
{
    k = mi::clampf(k, 0.f, 1.f);
    return {(uint8_t)(c.r * k), (uint8_t)(c.g * k), (uint8_t)(c.b * k)};
}

static void OnRender(uint32_t t_ms)
{
    LedPanel& L = hw.leds;
    if (t_ms < g_readout_until) mi::paint_cpu_readout(hw, g_boot_peak);
    if (settings.IsActive()) return;   /* Settings owns the buttons */

    const uint8_t page = pager.Page();

    LedPanel::Rgb b1 = page == kPagePattern ? scaled(kColDs, 0.08f + 0.92f * G_DS)
                                            : scaled({0xFF, 0xFF, 0xFF}, 0.05f + 0.95f * G_UFG);
    if (b1.r < kIdle.r && b1.g < kIdle.g && b1.b < kIdle.b) b1 = kIdle;
    L.SetButtonPair(kButtonB1, L.ScaleGlobal(b1));

    const LedPanel::Rgb b2 = cpu.GetAvgCpuLoad() > 0.80f
                                 ? kRed
                                 : kPageCol[page == kPageRoot ? kPageOsc : page];
    L.SetButtonPair(kButtonB2, L.ScaleGlobal(b2));

    L.SetButtonPair(kButtonB3, L.ScaleGlobal(kScaleCol[extras.scale_system % 3u]));
}

/* The ring is the morph matrix: node i sits at LED 2i of 16 (scaled to the
 * arc), dim in its topology colour; the playing position (MORPH + CV +
 * modulation, so it moves when the ring is modulated) is lit bright across
 * the two LEDs either side of it. */
static void MorphRing(LedPanel& panel, uint8_t pot, const ArcGeometry& geo,
                      float norm, uint32_t t_ms, void* ctx)
{
    (void)norm; (void)t_ms; (void)ctx;
    panel.ClearRing(pot);
    const int n = geo.arc_leds;
    if (n <= 0) return;
    auto at = [&](int led, LedPanel::Rgb c) {
        led = ((led % n) + n) % n;
        panel.SetRingByHour(pot, fmodf(geo.start_hour + geo.step_hours * (float)led, 12.0f),
                            panel.ScaleGlobal(c));
    };
    for (int i = 0; i < tally::kNumNodes; i++)
    {
        const LedPanel::Rgb c = kTopo[tally::NodeTopology(i)];
        at((int)((float)i * (float)n / 8.f + 0.5f), scaled(c, 0.12f));
    }
    const float pos  = G_MORPH * (float)n / 8.f;
    const int   lo   = (int)floorf(pos);
    const float fr   = pos - (float)lo;
    const int   node = (int)floorf(G_MORPH + 0.5f) & 7;
    const LedPanel::Rgb c = kTopo[tally::NodeTopology(node)];
    at(lo, scaled(c, 1.f - fr));
    at(lo + 1, scaled(c, fr));
}

/* ---- audio ----------------------------------------------------------------- */

static tally::Params P_audio;
static tally::CvJack s_jack[kNumMapped];

static void AudioCallback(daisy::AudioHandle::InputBuffer  in,
                          daisy::AudioHandle::OutputBuffer out,
                          size_t                           size)
{
    cpu.OnBlockStart();

    if (size != kBlockSize)
    {
        for (size_t i = 0; i < size; i++) { out[0][i] = 0.f; out[1][i] = 0.f; }
        cpu.OnBlockEnd();
        return;
    }

    if (g_req_new)     { g_req_new = false;     engine.NewPattern();  g_pattern_dirty = true; }
    if (g_req_write)   { g_req_write = false;   engine.WriteStep();   g_pattern_dirty = true; }
    if (g_req_restore) { g_req_restore = false; engine.RestoreStep(); g_pattern_dirty = true; }

    P_audio      = P;
    P_audio.voct = hw.cv_jacks[kCvVoct].Volts();

    /* J4-J6 through the CV map, onto this block's copy of the panel. */
    tally::CvActions act;
    for (uint8_t j = 0; j < kNumMapped; j++)
        tally::ApplyCv(g_dest[j], hw.cv_jacks[kCvMapped[j]].Volts(), g_amount[j],
                       s_jack[j], P_audio, act);
    P_audio.gate = act.gate || g_b1_gate;
    if (act.new_pattern)  { engine.NewPattern();  g_pattern_dirty = true; }
    if (act.write_step)   { engine.WriteStep();   g_pattern_dirty = true; }
    if (act.restore_step) { engine.RestoreStep(); g_pattern_dirty = true; }
    if (act.next_scale)   g_scale_tap = true;

    engine.Process(P_audio, in[0], in[1], out[0], out[1], size);

    const tally::Status& st = engine.status();
    hw.cv_jacks[kCvUfgOut].SetVolts(5.f * st.ufg);
    hw.cv_jacks[kCvDsOut].SetVolts(5.f * st.ds);

    G_MORPH = st.morph_pos;
    G_UFG   = st.ufg;
    G_DS    = st.ds;
    G_GATE  = st.ufg_gate;

    cpu.OnBlockEnd();
}

/* ---- boot ------------------------------------------------------------------ */

int main(void)
{
    hw.Init(daisy::SaiHandle::Config::SampleRate::SAI_48KHZ, kBlockSize);
    cpu.Init(hw.SampleRate(), (int)hw.BlockSize());
    if (hw.BlockSize() != kBlockSize) mi::fault_forever(hw);

    engine.Init(hw.SampleRate());

    /* J7 and J8 (the STM32 DAC) become outputs, written once per block. */
    hw.cv_jacks[kCvUfgOut].SetVolts(0.f);
    hw.cv_jacks[kCvUfgOut].EnableCvOutput();
    hw.cv_jacks[kCvDsOut].SetVolts(0.f);
    hw.cv_jacks[kCvDsOut].EnableCvOutput();

    sd.Init();
    picker::Install(settings, kSettingsFirmware, sd, hw);
    settings.UseBrightness();
    settings.UsePresets(presets);
    s_ext = settings.Page(kSettingsMain).Pot(1)
        .Selector(kExtLabels).Default(0)
        .Ident("ext_mode").Name("Ext (J1)")
        .Help("**Sync**: J1's rising zero crossings replace the UFG as the "
              "damped-sync leader. **Track**: the OSC follows J1's pitch "
              "(zero-crossing detection; FREQ is then a ratio). **Off**: J1 "
              "is ignored.");
    s_track = settings.Page(kSettingsMain).Pot(4)
        .Selector(kTrackLabels).Default(0)
        .Ident("mod_track").Name("Mod tracks")
        .Help("What the Mod oscillator's HARMONIC is relative to: Root, the "
              "OSC or the UFG, scale quantisation included.");
    s_qtrig = settings.Page(kSettingsFirmware).Pot(3)
        .Selector(kOnOffLabels).Default(0)
        .Ident("qtrig").Name("Q.Trig")
        .Help("**On**: each time the quantised OSC pitch changes step, the "
              "UFG is triggered (needs Root to the OSC and a scale). With the "
              "UFG on One-shot, an LFO into J3 auto-arpeggiates.");

    /* Settings page 2: the CV map. Each row of the panel is one jack --
     * P1/P2 J4, P3/P4 J5, P5/P6 J6 -- destination on the left, amount on
     * the right (an attenuverter: noon off, full right +1, full left -1). */
    {
        static const char* const kDestIds[kNumMapped]    = {"cv_j4_dest", "cv_j5_dest", "cv_j6_dest"};
        static const char* const kAmountIds[kNumMapped]  = {"cv_j4_amount", "cv_j5_amount", "cv_j6_amount"};
        static const char* const kDestNames[kNumMapped]  = {"J4 to", "J5 to", "J6 to"};
        static const char* const kAmountNames[kNumMapped] = {"J4 amount", "J5 amount", "J6 amount"};
        const uint8_t defaults[kNumMapped] = {tally::kDestMorph, tally::kDestTrigger, tally::kDestShape};
        for (uint8_t j = 0; j < kNumMapped; j++)
        {
            s_dest[j] = settings.Page(kSettingsCv).Pot((uint8_t)(2u * j))
                .Selector(tally::kDestLabels).Default(defaults[j])
                .Ident(kDestIds[j]).Name(kDestNames[j])
                .Help("Any knob on any page, a Settings choice, or a button "
                      "action. Knobs: +-5 V sweeps the whole knob (FREQ, ROOT "
                      "and TIME are 1 V/oct; MORPH 1.6 nodes per volt, round "
                      "the ring). Selectors: 0-5 V steps through every option "
                      "from where the knob sits. Actions are gates (on above "
                      "1.5 V); UFG trigger holds while the gate is high.");
            s_amount[j] = settings.Page(kSettingsCv).Pot((uint8_t)(2u * j + 1u))
                .Knob().Default(1.0f)
                .Ident(kAmountIds[j]).Name(kAmountNames[j])
                .Help("How much, and which way: full right +1, noon off, full "
                      "left inverted. Ignored by the actions.");
        }
    }

    /* Every jack is read raw in the callback. */
    for (uint8_t j = 0; j < 6; j++) cv_matrix.Jack(j).Off();

    for (uint8_t pg = 0; pg < kNumAppPages; pg++) pager.SetPageColor(pg, kPageCol[pg]);

    host.Product("Tally")
        .BootSlot(home.slot)
        .Pages(osc_page, ufg_page, pattern_page, mods_page, root_page)
        .Jacks(jk_ext, jk_tz, jk_voct, jk_cv4, jk_cv5, jk_cv6,
               jk_ufg, jk_ds, jk_sin, jk_cos)
        .Buttons(bt_b1, bt_b2, bt_b3)
        .Attach(manual)
        .Extend(fs_ext);
#ifdef TALLY_BENCH_USB
    bench_cdc.Init(bench_usb, daisy::UsbHandle::FS_INTERNAL, "Tally (bench)");
    host.Transport(bench_cdc);
#endif

    presets.Manage(pager);
    presets.Manage(settings);
    presets.Manage(extras);
    presets.Init();
    const bool had_boot = home.Boot(presets);

    if (extras.have_pattern) engine.SetPattern(extras.levels, extras.glide);

    float phys[kNumPots];
    for (int i = 0; i < 8; i++)
    {
        hw.ProcessAllControls();
        System::Delay(1);
    }
    for (uint8_t p = 0; p < kNumPots; p++) phys[p] = hw.pots[p].Value();

    if (!had_boot)
    {
        /* First boot after a flash: Centre. MORPH at FMNT, FREQ at noon
         * (C4), the three waveshapers, DAMPED and STRETCH down, every
         * attenuverter at noon (off); a slow looping UFG; a locked 8-step
         * pattern; Root at C4 to the OSC, Mod on harmonic 1 of Root. */
        const float osc[kNumPots]  = {0.f, 0.5f, 0.f, 0.f, 0.f, 0.f};
        const float ufg[kNumPots]  = {0.4f, 0.5f, 0.f, 0.f, 0.25f, 0.125f};
        const float pat[kNumPots]  = {0.f, 0.5f, 0.f, 0.5f, 0.5f, 0.f};
        const float mods[kNumPots] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.25f};
        const float rt[kNumPots]   = {0.5f, 4.5f / 9.f, 0.5f, 2.5f / 5.f, 0.5f, 0.125f};
        for (uint8_t p = 0; p < kNumPots; p++)
        {
            pager.SetStored(kPageOsc, p, osc[p], phys);
            pager.SetStored(kPageUfg, p, ufg[p], phys);
            pager.SetStored(kPagePattern, p, pat[p], phys);
            pager.SetStored(kPageMods, p, mods[p], phys);
            pager.SetStored(kPageRoot, p, rt[p], phys);
        }
        engine.GetPattern(extras.levels, &extras.glide);
        extras.have_pattern = true;
    }

    g_boot_peak     = extras.cpu_peak;
    extras.cpu_peak = 0.0f;
    extras.cpu_avg  = 0.0f;
    g_readout_until = System::GetNow() + 2500u;

    hw.StartAudio(AudioCallback);
    cpu.Reset();

    loop.Use(pager)
        .Use(settings)
        .Use(cv_matrix)
        .Use(osc_page)
        .Use(ufg_page)
        .Use(pattern_page)
        .Use(mods_page)
        .Use(root_page)
        .Use(host)
        .OnFrame(OnFrame)
        .OnPoll(OnPoll)
        .OnRender(OnRender);

    for (;;) loop.Tick();
}
