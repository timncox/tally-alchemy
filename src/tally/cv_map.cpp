/*
 * Tally's CV mapper. See cv_map.h for the scaling rules.
 */
#include "cv_map.h"

#include <math.h>

namespace tally {

const char* const kDestLabels[kNumDests] = {
    "Off",
    "Morph", "Freq", "Shift", "Depth", "Shape", "Damped",
    "Time", "Skew", "U.Shape", "Stretch", "Loop / one-shot", "Trig type",
    "Chance", "Length", "Smooth", "Pitch mod", "Time mod", "TZPM",
    "Shift mod", "Depth mod", "Shape mod", "Morph mod", "Damped mod", "Mod source",
    "Root", "Scale", "Harmonic", "Order", "Detune", "Route",
    "Scale system", "Ext mode", "Mod tracks", "Q.Trig",
    "UFG trigger (gate)", "Write step (gate)", "Restore step (gate)",
    "New pattern (gate)", "Next scale system (gate)"};

namespace {

inline float Clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* A 0..1 knob: 0.2 of its range per volt. */
inline void Unipolar(float& f, float v, float a) { f = Clampf(f + 0.2f * v * a, 0.f, 1.f); }

/* A -1..1 knob: the same share of a range twice as wide. */
inline void Bipolar(float& f, float v, float a) { f = Clampf(f + 0.4f * v * a, -1.f, 1.f); }

/* A selector of n zones: 0-5 V at amount 1 steps through all of them. */
inline int Zone(int base, float v, float a, int n)
{
    int z = base + (int)floorf(0.2f * v * a * (float)n + 0.5f);
    return z < 0 ? 0 : (z >= n ? n - 1 : z);
}

}  // namespace

void ApplyCv(uint8_t dest, float v, float a, CvJack& jack, Params& p, CvActions& act)
{
    a = Clampf(a, -1.f, 1.f);
    switch (dest)
    {
        case kDestMorph:      p.morph_nodes += 1.6f * v * a; break;
        case kDestFreq:       p.freq = Clampf(p.freq + v * a / 3.f, -1.f, 1.f); break;   /* +-1 = +-36 st */
        case kDestShift:      Unipolar(p.shift, v, a); break;
        case kDestDepth:      Unipolar(p.depth, v, a); break;
        case kDestShape:      Unipolar(p.shape, v, a); break;
        case kDestDamped:     Unipolar(p.damped, v, a); break;

        case kDestTime:       p.time_oct += v * a; break;
        case kDestSkew:       Unipolar(p.skew, v, a); break;
        case kDestUfgShape:   Unipolar(p.ufg_shape, v, a); break;
        case kDestStretch:    Unipolar(p.stretch, v, a); break;
        case kDestLoop:       p.ufg_mode = (uint8_t)Zone(p.ufg_mode, v, a, 2); break;
        case kDestTrigType:   p.trig_type = (uint8_t)Zone(p.trig_type, v, a, 4); break;

        case kDestChance:     Unipolar(p.chance, v, a); break;
        case kDestLength:     Unipolar(p.length, v, a); break;
        case kDestSmooth:     Unipolar(p.smooth, v, a); break;
        case kDestPitchAtt:   Bipolar(p.pitch_att, v, a); break;
        case kDestTimeAtt:    Bipolar(p.time_att, v, a); break;
        case kDestTzpm:       Unipolar(p.tzpm, v, a); break;

        case kDestShiftAtt:   Bipolar(p.shift_att, v, a); break;
        case kDestDepthAtt:   Bipolar(p.depth_att, v, a); break;
        case kDestShapeAtt:   Bipolar(p.shape_att, v, a); break;
        case kDestMorphAtt:   Bipolar(p.morph_att, v, a); break;
        case kDestDampedAtt:  Bipolar(p.damped_att, v, a); break;
        case kDestSource:     p.mod_source = (uint8_t)Zone(p.mod_source, v, a, 2); break;

        case kDestRoot:       p.root = Clampf(p.root + v * a / 3.f, -1.f, 1.f); break;
        case kDestScale:      p.scale = Zone(p.scale, v, a, kNumScales); break;
        case kDestHarmonic:   Bipolar(p.harmonic, v, a); break;
        case kDestOrder:      p.order = (uint8_t)Zone(p.order, v, a, kNumOrders); break;
        case kDestDetune:     Bipolar(p.detune, v, a); break;
        case kDestRoute:      p.root_send = (uint8_t)Zone(p.root_send, v, a, 4); break;

        case kDestScaleSystem: p.scale_system = (uint8_t)Zone(p.scale_system, v, a, 3); break;
        case kDestExtMode:     p.ext_mode = (uint8_t)Zone(p.ext_mode, v, a, 3); break;
        case kDestModTrack:    p.mod_track = (uint8_t)Zone(p.mod_track, v, a, 3); break;
        case kDestQTrig:       p.qtrig = Zone(p.qtrig ? 1 : 0, v, a, 2) == 1; break;

        case kDestTrigger: case kDestWriteStep: case kDestRestoreStep:
        case kDestNewPattern: case kDestNextScale:
        {
            const bool was = jack.high;
            if (!jack.high && v > 1.5f) jack.high = true;
            else if (jack.high && v < 0.5f) jack.high = false;
            const bool rose = jack.high && !was;
            if (dest == kDestTrigger) act.gate = act.gate || jack.high;
            else if (rose)
            {
                if (dest == kDestWriteStep) act.write_step = true;
                if (dest == kDestRestoreStep) act.restore_step = true;
                if (dest == kDestNewPattern) act.new_pattern = true;
                if (dest == kDestNextScale) act.next_scale = true;
            }
            break;
        }
        default: break;
    }
}

}  // namespace tally
