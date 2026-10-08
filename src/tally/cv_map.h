/*
 * Tally's CV mapper: J4, J5 and J6 can each drive any of 40 destinations
 * (every knob on every page, four Settings choices, five button actions),
 * with an amount. Portable, so test/ checks the scaling on the host; the
 * firmware runs it once per audio block (0.5 ms) on a copy of the panel's
 * Params.
 *
 * Scaling, at amount +1 (the amount is an attenuverter, -1..+1):
 *   knobs        +-5 V sweeps the knob's whole range (0.2 of it per volt)
 *   FREQ, ROOT   1 V/oct (12 st per volt)
 *   TIME         1 V/oct on the UFG rate
 *   MORPH        1.6 nodes per volt, wrapping round the ring
 *   selectors    0-5 V steps through every zone from where the knob sits
 *   actions      a gate: on above 1.5 V, off below 0.5 V; the action fires
 *                on the rising edge (UFG TRIGGER is the level, so a held
 *                gate sustains)
 */
#pragma once

#include <stdint.h>

#include "tally_engine.h"

namespace tally {

enum Dest : uint8_t {
    kDestOff = 0,
    /* OSC */
    kDestMorph, kDestFreq, kDestShift, kDestDepth, kDestShape, kDestDamped,
    /* UFG */
    kDestTime, kDestSkew, kDestUfgShape, kDestStretch, kDestLoop, kDestTrigType,
    /* PATTERN */
    kDestChance, kDestLength, kDestSmooth, kDestPitchAtt, kDestTimeAtt, kDestTzpm,
    /* MODS */
    kDestShiftAtt, kDestDepthAtt, kDestShapeAtt, kDestMorphAtt, kDestDampedAtt, kDestSource,
    /* ROOT */
    kDestRoot, kDestScale, kDestHarmonic, kDestOrder, kDestDetune, kDestRoute,
    /* Settings choices */
    kDestScaleSystem, kDestExtMode, kDestModTrack, kDestQTrig,
    /* Actions (gates) */
    kDestTrigger, kDestWriteStep, kDestRestoreStep, kDestNewPattern, kDestNextScale,
    kNumDests
};

extern const char* const kDestLabels[kNumDests];

/* On the module a destination is picked as a SECTION and a KNOB: the page,
 * then the pot on it (P1-P6, front view), so "UFG, P4" is STRETCH. Two small
 * selectors instead of one of 40 (the SDK's selector ring draws 16 at most).
 * Settings: P1 scale system, P2 Ext mode, P3 Mod tracks, P4 Q.Trig.
 * Actions: P1 UFG trigger, P2 write step, P3 restore step, P4 new pattern,
 * P5 next scale system. A slot with nothing on it is Off. */
enum Section : uint8_t {
    kSecOff = 0, kSecOsc, kSecUfg, kSecPattern, kSecMods, kSecRoot, kSecSettings, kSecActions,
    kNumSections
};
constexpr uint8_t kNumSlots = 6;
extern const char* const kSectionLabels[kNumSections];
extern const char* const kSlotLabels[kNumSlots];
uint8_t DestFor(uint8_t section, uint8_t slot);

/* One-shot actions a mapped gate asked for this block. */
struct CvActions
{
    bool gate         = false;   /* UFG trigger, as a level */
    bool write_step   = false;
    bool restore_step = false;
    bool new_pattern  = false;
    bool next_scale   = false;
};

/* One jack's gate state (Schmitt trigger), kept across blocks. */
struct CvJack
{
    bool high = false;
};

/* Apply `volts` on a jack mapped to `dest` with `amount` (-1..1) to `p`;
 * gate destinations update `jack` and set flags in `act`. */
void ApplyCv(uint8_t dest, float volts, float amount, CvJack& jack, Params& p, CvActions& act);

}  // namespace tally
