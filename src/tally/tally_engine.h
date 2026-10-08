/*
 * Tally engine -- a clean-room complex oscillator in the spirit of
 * fancyyyyy x grrrwaaa's K-ACCUMULATOR, for the Alchemy Lab.
 *
 * Portable C++ with no Daisy or SDK dependency, so test/ can render it on
 * the host. The firmware (tally_alchemy.cpp) fills Params from the panel
 * once per block and calls Process().
 *
 * Four sections, as on the original:
 *   OSC   a sine/cosine pair (J9/J10) through a morphing waveshaping
 *         matrix: eight nodes on a ring, each mapping SHIFT / DEPTH / SHAPE
 *         to a vector of synthesis parameters; MORPH interpolates between
 *         neighbouring nodes. Plus STRETCH and DAMPED/PULSAR.
 *   MOD   the second oscillator, tracking Root, OSC or UFG.
 *   UFG   a function generator: modulation source, pulsar window and
 *         the clock of the delta-sigma section.
 *   D-S   a looping pattern generator fed by delta-sigma encoding of the
 *         Mod oscillator, clocked by the UFG.
 *
 * src/tally/DESIGN.md says which behaviours the K-ACCUMULATOR quick-start
 * describes and which were invented here.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace tally {

constexpr int kNumNodes  = 8;
constexpr int kNumScales = 9;   /* per tuning system */
constexpr int kMaxSteps  = 16;

/* Ring order, clockwise from the top (north pole). */
enum Node : uint8_t {
    kFmnt = 0, kFbpm2, k2op2, kXpm2, kAsym, kXpm, k2op, kFbpm
};

enum Topology : uint8_t { kTopoSelf = 0, kTopoMod, kTopoCross };   /* green / yellow / red */
Topology NodeTopology(int node);
const char* NodeName(int node);

enum ScaleSystem : uint8_t { kScaleOff = 0, kScaleTet, kScaleJi };
enum RootSend : uint8_t { kSendOsc = 0, kSendBoth, kSendUfg, kSendNone };
enum ModTrack : uint8_t { kTrackRoot = 0, kTrackOsc, kTrackUfg };
enum ExtMode : uint8_t { kExtOff = 0, kExtSync, kExtTrack };
enum UfgMode : uint8_t { kUfgLoop = 0, kUfgOneShot };
enum TrigType : uint8_t { kTrigSync = 0, kTrigReverse, kTrigSustainSync, kTrigSustainReverse };
enum OrderZone : uint8_t { kOrderFree = 0, kOrderEven, kOrderAll, kOrderOdd, kOrderQuant, kNumOrders };
enum ModSource : uint8_t { kSrcUfg = 0, kSrcDs };
enum CvTarget : uint8_t { kCvShift = 0, kCvDepth, kCvShape, kCvDamped, kCvTime, kCvHarmonic, kNumCvTargets };

const char* ScaleName(uint8_t system, int scale);

struct Params
{
    /* OSC */
    float morph   = 0.f;    /* 0..1, the full ring once round */
    float freq    = 0.f;    /* -1..1 = +-36 st */
    float shift   = 0.f;    /* 0..1 */
    float depth   = 0.f;
    float shape   = 0.f;
    float damped  = 0.f;
    float stretch = 0.f;

    /* MODS: attenuverters for the internal normal (UFG or D-S) */
    float shift_att = 0.f, depth_att = 0.f, shape_att = 0.f;
    float morph_att = 0.f, damped_att = 0.f;
    uint8_t mod_source = kSrcUfg;

    /* UFG */
    float   time     = 0.5f;   /* 0..1 */
    float   skew     = 0.5f;
    float   ufg_shape = 0.f;   /* 0 linear, 1/3 sine, 2/3 exp, 1 raised cosine */
    uint8_t ufg_mode = kUfgLoop;
    uint8_t trig_type = kTrigSync;

    /* D-S */
    float chance = 0.f, length = 0.5f, smooth = 0.f;
    float pitch_att = 0.f;     /* D-S -> OSC pitch, +-24 st */
    float time_att  = 0.f;     /* random walk -> UFG time, +-3 oct */
    float tzpm      = 0.f;     /* J2 through-zero PM depth */

    /* ROOT / MOD */
    float   root      = 0.f;   /* -1..1 = +-36 st about C4 */
    int     scale     = 4;     /* 0..8 */
    uint8_t scale_system = kScaleOff;
    float   harmonic  = 0.f;   /* -1..1 */
    uint8_t order     = kOrderAll;
    float   detune    = 0.f;   /* -1..1 */
    uint8_t root_send = kSendOsc;

    /* Settings */
    uint8_t ext_mode  = kExtOff;
    uint8_t mod_track = kTrackRoot;
    bool    qtrig     = false;
    uint8_t cv_target = kCvShape;

    /* Jacks (volts) and the gate */
    float voct     = 0.f;      /* J3 */
    float morph_cv = 0.f;      /* J4 */
    float aux_cv   = 0.f;      /* J6, routed by cv_target */
    bool  gate     = false;    /* J5 or B1 */
};

/* What the panel shows, refreshed by every Process(). */
struct Status
{
    float morph_pos  = 0.f;    /* 0..8 on the ring */
    float ufg        = 0.f;    /* 0..1 */
    bool  ufg_gate   = false;
    float ds         = 0.f;    /* 0..1, smoothed */
    int   ds_step    = 0;
    int   ds_len     = 8;
    float osc_hz     = 0.f;
    float mod_hz     = 0.f;
    float ufg_hz     = 0.f;
    float track_hz   = 0.f;    /* 0 when nothing is tracked */
    float peak       = 0.f;
};

class Engine
{
  public:
    void Init(float sample_rate, uint32_t seed = 0x2545F491u);

    /* ext / tz: J1 / J2 in codec units (+-1 = +-5 V); either may be null. */
    void Process(const Params& p, const float* ext, const float* tz,
                 float* out_sin, float* out_cos, size_t n);

    /* Pattern editing (the Pattern switch). */
    void WriteStep();       /* a new value at the current step */
    void RestoreStep();     /* the value it replaced */
    void NewPattern();      /* overwrite all sixteen */

    const Status& status() const { return st_; }

    /* The stored pattern as 3-bit levels (0..7) plus glide flags, for the
     * preset slot. */
    void GetPattern(uint8_t* levels, uint16_t* glide) const;
    void SetPattern(const uint8_t* levels, uint16_t glide);

    /* For the tests. */
    float PatternValue(int i) const { return pattern_[i & (kMaxSteps - 1)]; }
    float QuantizeNote(const Params& p, float note) const;

  private:
    struct Vec
    {
        float harm, fold, fb, fb_lp, mpm, m_lp, xpm, fmnt, mam, asym, chaos;
    };
    static Vec NodeVec(int node, float s, float d, float h);
    static Vec Lerp(const Vec& a, const Vec& b, float t);

    uint32_t NextBits();
    float NextRandom();      /* 0..1 */
    float EncodeDs(float m);
    float FreshDsValue();
    void  AdvancePattern(const Params& p);
    bool  TrackInput(float x);

    float sr_ = 48000.f, inv_sr_ = 1.f / 48000.f;
    uint32_t rng_ = 1u;

    /* OSC */
    float phase_ = 0.f, stretch_phase_ = 0.f, fold_phase_ = 0.f;
    float fb_lp_ = 0.f, fb_src_ = 0.f, out_prev_ = 0.f;

    /* MOD */
    float mod_phase_ = 0.f, mod_lp_ = 0.f, mod_out_ = 0.f;

    /* UFG */
    float ufg_phase_ = 0.f;
    float ufg_dir_   = 1.f;
    bool  ufg_running_ = true;
    bool  ufg_completed_ = true;
    bool  gate_prev_ = false;
    float walk_ = 0.f;

    /* D-S */
    float pattern_[kMaxSteps] = {};
    float previous_[kMaxSteps] = {};
    bool  glide_[kMaxSteps] = {};
    int   step_ = 0, win_start_ = 0, win_len_ = 8;
    float ds_target_ = 0.f, ds_out_ = 0.f, ds_err_ = 0.f;
    uint32_t samples_since_clock_ = 0, step_samples_ = 4800;
    float last_ratio_ = 0.382f;   /* Mod/UFG frequency ratio, for NewPattern */

    /* Q.Trig */
    int last_q_step_ = 0x7FFFFFFF;

    /* Ext track / sync */
    float ext_prev_ = 0.f;
    bool  ext_high_ = false;
    uint32_t ext_count_ = 0;
    float ext_period_ = 0.f;
    uint32_t ext_since_ = 0;

    float peak_ = 0.f;
    Status st_;
};

}  // namespace tally
