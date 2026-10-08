/*
 * Tally engine. See tally_engine.h for the shape and src/tally/DESIGN.md
 * for what is described by the K-ACCUMULATOR quick-start and what was
 * invented here.
 */
#include "tally_engine.h"

#include <math.h>
#include <string.h>

#include <initializer_list>

namespace tally {

namespace {

constexpr float kTwoPi   = 6.28318530718f;
constexpr float kInvTwoPi = 0.159154943092f;

/* ---- sine table ---------------------------------------------------------- */

constexpr int kSineSize = 1024;
float g_sine[kSineSize + 1];
bool  g_sine_ready = false;

void InitSine()
{
    if (g_sine_ready) return;
    for (int i = 0; i <= kSineSize; i++)
        g_sine[i] = sinf(kTwoPi * (float)i / (float)kSineSize);
    g_sine_ready = true;
}

inline float Wrap(float x) { return x - floorf(x); }

/* sin(2 pi x), x in cycles, any range. */
inline float Sin(float x)
{
    const float p  = Wrap(x) * (float)kSineSize;
    const int   i  = (int)p;
    const float fr = p - (float)i;
    return g_sine[i] + fr * (g_sine[i + 1] - g_sine[i]);
}
inline float Cos(float x) { return Sin(x + 0.25f); }

inline float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float Lerpf(float a, float b, float t) { return a + (b - a) * t; }

inline float NoteToHz(float note) { return 440.f * exp2f((note - 69.f) / 12.f); }
inline float HzToNote(float hz) { return 69.f + 12.f * log2f(hz / 440.f); }

/* Soft limit: transparent below 0.9, approaches 1.0 above. */
inline float SoftClip(float x)
{
    const float a = fabsf(x);
    if (a < 0.9f) return x;
    const float t = (a - 0.9f) * 10.f;
    const float y = 0.9f + 0.1f * t / (1.f + t);
    return x < 0.f ? -y : y;
}

/* Fade a partial out as it nears Nyquist (0.40 .. 0.45 of the rate). */
inline float NyquistFade(float ratio_of_sr)
{
    return Clamp((0.45f - ratio_of_sr) * 20.f, 0.f, 1.f);
}

/* ---- scales -------------------------------------------------------------- */

constexpr int kMaxDegrees = 32;
struct Scale
{
    const char* name;
    int         count;
    float       cents[kMaxDegrees];
};

Scale g_tet[kNumScales];
Scale g_ji[kNumScales];
bool  g_scales_ready = false;

void SetTet(Scale& s, const char* name, std::initializer_list<int> c)
{
    s.name  = name;
    s.count = 0;
    for (int v : c) s.cents[s.count++] = (float)v;
}

void SetJi(Scale& s, const char* name, std::initializer_list<float> ratios)
{
    s.name  = name;
    s.count = 0;
    for (float r : ratios) s.cents[s.count++] = 1200.f * log2f(r);
}

void InitScales()
{
    if (g_scales_ready) return;
    /* Ends: octaves only. Centre: the densest. Left of centre the standard
     * scales, right of centre the exotic ones (the guide's layout). */
    SetTet(g_tet[0], "Octaves", {0});
    SetTet(g_tet[1], "Fifths", {0, 700});
    SetTet(g_tet[2], "Pentatonic", {0, 200, 400, 700, 900});
    SetTet(g_tet[3], "Major", {0, 200, 400, 500, 700, 900, 1100});
    SetTet(g_tet[4], "Chromatic", {0, 100, 200, 300, 400, 500, 600, 700, 800, 900, 1000, 1100});
    SetTet(g_tet[5], "Whole tone", {0, 200, 400, 600, 800, 1000});
    SetTet(g_tet[6], "Octatonic", {0, 200, 300, 500, 600, 800, 900, 1100});
    SetTet(g_tet[7], "Hirajoshi", {0, 200, 300, 700, 800});
    SetTet(g_tet[8], "Octaves", {0});

    SetJi(g_ji[0], "1/1", {1.f});
    SetJi(g_ji[1], "1/1 3/2", {1.f, 1.5f});
    SetJi(g_ji[2], "PM ratios", {1.f, 5.f / 4, 3.f / 2, 7.f / 4});
    SetJi(g_ji[3], "JI major", {1.f, 9.f / 8, 5.f / 4, 4.f / 3, 3.f / 2, 5.f / 3, 15.f / 8});
    g_ji[4].name  = "32 harmonics";
    g_ji[4].count = 32;
    for (int n = 0; n < 32; n++) g_ji[4].cents[n] = 1200.f * log2f((float)(32 + n) / 32.f);
    SetJi(g_ji[5], "7-limit", {1.f, 8.f / 7, 7.f / 6, 4.f / 3, 3.f / 2, 8.f / 5, 7.f / 4});
    SetJi(g_ji[6], "Utonal", {1.f, 16.f / 15, 16.f / 14, 16.f / 13, 16.f / 12, 16.f / 11, 16.f / 10, 16.f / 9});
    SetJi(g_ji[7], "11-limit", {1.f, 12.f / 11, 11.f / 9, 11.f / 8, 3.f / 2, 18.f / 11, 11.f / 6});
    SetJi(g_ji[8], "1/1", {1.f});
    g_scales_ready = true;
}

const Scale* GetScale(uint8_t system, int scale)
{
    if (scale < 0) scale = 0;
    if (scale >= kNumScales) scale = kNumScales - 1;
    if (system == kScaleTet) return &g_tet[scale];
    if (system == kScaleJi) return &g_ji[scale];
    return nullptr;
}

/* Nearest scale degree to `note` (MIDI semitones, C-based octaves). `step`
 * is a monotonic index of the chosen degree, for Q.Trig. */
float Quantize(const Scale* s, float note, int* step)
{
    if (!s)
    {
        if (step) *step = (int)floorf(note + 0.5f);
        return note;
    }
    const float c    = note * 100.f;
    const float oct  = floorf(c / 1200.f);
    const float r    = c - oct * 1200.f;
    int   best       = 0;
    float best_d     = 1e9f;
    for (int i = 0; i <= s->count; i++)
    {
        const float deg = i < s->count ? s->cents[i] : 1200.f;
        const float d   = fabsf(r - deg);
        if (d < best_d) { best_d = d; best = i; }
    }
    const float deg = best < s->count ? s->cents[best] : 1200.f;
    if (step) *step = (int)oct * s->count + best;
    return (oct * 1200.f + deg) / 100.f;
}

/* Harmonic sequences for the Mod's ORDER zones. */
inline int HarmonicAt(uint8_t order, int idx)
{
    if (order == kOrderOdd) return 2 * idx + 1;
    if (order == kOrderEven) return idx == 0 ? 1 : 2 * idx;
    return idx + 1;
}

}  // namespace

/* ---- node table ---------------------------------------------------------- */

static const char* const kNodeNames[kNumNodes] = {
    "FMNT", "FBPM2", "2OP2", "XPM2", "ASYM", "XPM", "2OP", "FBPM"};

const char* NodeName(int node) { return kNodeNames[((node % kNumNodes) + kNumNodes) % kNumNodes]; }

Topology NodeTopology(int node)
{
    switch (((node % kNumNodes) + kNumNodes) % kNumNodes)
    {
        case kFmnt: case kFbpm: case kFbpm2: return kTopoSelf;
        case kXpm: case kXpm2: return kTopoCross;
        default: return kTopoMod;
    }
}

const char* ScaleName(uint8_t system, int scale)
{
    InitScales();
    const Scale* s = GetScale(system, scale);
    return s ? s->name : "Off";
}

/*
 * Each node maps the three waveshaping controls (s = SHIFT, d = DEPTH,
 * h = SHAPE, all 0..1) onto one parameter vector; every vector is zero
 * (a pure sine/cosine) when s = d = h = 0. Indices are in radians.
 */
Engine::Vec Engine::NodeVec(int node, float s, float d, float h)
{
    Vec v = {};
    v.fb_lp = 1.f;
    v.m_lp  = 1.f;
    switch (node)
    {
        case kFmnt:   /* formant AM; SHAPE = damped self-feedback */
            v.harm = s; v.fmnt = d;
            v.fb = 0.8f * h; v.fb_lp = 1.f - 0.85f * h;
            break;
        case kFbpm:   /* self-feedback PM, sine to saw */
            v.harm = s; v.fb = 1.6f * d; v.fold = h;
            break;
        case k2op:    /* two-operator PM, damping rises with DEPTH */
            v.harm = s; v.mpm = 4.f * d; v.m_lp = 1.f - 0.75f * d; v.fold = h;
            break;
        case kXpm:    /* cross PM: DEPTH also feeds back into the Mod */
            v.harm = s; v.mpm = 4.f * d; v.m_lp = 1.f - 0.75f * d;
            v.xpm = 0.8f * d; v.fold = h;
            break;
        case kAsym:   /* asymmetric PM + pulse-train AM from the Mod */
            v.harm = 0.3f * s; v.asym = 1.f; v.mpm = 3.f * d; v.mam = d;
            v.fold = 0.4f * h;
            break;
        case kFbpm2:  /* chaotic feedback through the folder; SHIFT = damping */
            v.chaos = 1.f; v.fb = 2.6f * d; v.fb_lp = 1.f - 0.95f * s; v.fold = h;
            break;
        case k2op2:   /* Mod PM on DEPTH, self PM with damping on SHIFT */
            v.mpm = 4.f * d; v.fb = 1.4f * s; v.fb_lp = 1.f - 0.8f * s; v.fold = h;
            break;
        case kXpm2:   /* separated: DEPTH = PM, SHIFT = damping, SHAPE = XPM */
            v.mpm = 4.f * d; v.m_lp = (1.f - 0.9f * s) * (1.f - 0.5f * h);
            v.xpm = h;
            break;
    }
    return v;
}

Engine::Vec Engine::Lerp(const Vec& a, const Vec& b, float t)
{
    Vec v;
    v.harm  = Lerpf(a.harm, b.harm, t);
    v.fold  = Lerpf(a.fold, b.fold, t);
    v.fb    = Lerpf(a.fb, b.fb, t);
    v.fb_lp = Lerpf(a.fb_lp, b.fb_lp, t);
    v.mpm   = Lerpf(a.mpm, b.mpm, t);
    v.m_lp  = Lerpf(a.m_lp, b.m_lp, t);
    v.xpm   = Lerpf(a.xpm, b.xpm, t);
    v.fmnt  = Lerpf(a.fmnt, b.fmnt, t);
    v.mam   = Lerpf(a.mam, b.mam, t);
    v.asym  = Lerpf(a.asym, b.asym, t);
    v.chaos = Lerpf(a.chaos, b.chaos, t);
    return v;
}

/* ---- setup --------------------------------------------------------------- */

void Engine::Init(float sample_rate, uint32_t seed)
{
    InitSine();
    InitScales();
    sr_     = sample_rate;
    inv_sr_ = 1.f / sample_rate;
    rng_    = seed ? seed : 1u;

    phase_ = stretch_phase_ = fold_phase_ = 0.f;
    fb_lp_ = fb_src_ = out_prev_ = 0.f;
    mod_phase_ = mod_lp_ = mod_out_ = 0.f;
    ufg_phase_ = 0.f; ufg_dir_ = 1.f; ufg_running_ = true; ufg_completed_ = true;
    gate_prev_ = false; walk_ = 0.f;
    step_ = 0; win_start_ = 0; win_len_ = 8;
    ds_target_ = ds_out_ = ds_err_ = 0.f;
    samples_since_clock_ = 0; step_samples_ = (uint32_t)(sample_rate / 10.f);
    last_q_step_ = 0x7FFFFFFF;
    ext_prev_ = 0.f; ext_high_ = false; ext_count_ = 0; ext_period_ = 0.f; ext_since_ = 0;
    peak_ = 0.f;
    st_ = Status();

    /* A first pattern: the Mod sampled at a fixed irrational stride. */
    last_ratio_ = 0.382f;
    NewPattern();
    memcpy(previous_, pattern_, sizeof pattern_);
}

uint32_t Engine::NextBits()
{
    /* xorshift32 */
    uint32_t x = rng_;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_ = x;
    return x;
}

float Engine::NextRandom() { return (float)(NextBits() >> 8) * (1.f / 16777216.f); }

/* ---- delta-sigma pattern section ------------------------------------------ */

/* One output of a first-order, 8-level delta-sigma quantizer fed with the
 * Mod oscillator's value at the clock (invented: the guide says only
 * "delta-sigma encoding of the Mod oscillator"). The error carries from
 * step to step, so the pattern's density follows the Mod/UFG ratio. */
float Engine::EncodeDs(float m)
{
    const float v = m + ds_err_;
    float q = floorf((v * 7.f + 7.f) * 0.5f + 0.5f);
    q       = Clamp(q, 0.f, 7.f);
    const float level = (2.f * q - 7.f) / 7.f;
    ds_err_ = Clamp(v - level, -1.f, 1.f);
    return 0.5f * (level + 1.f);
}

float Engine::FreshDsValue() { return EncodeDs(mod_out_); }

void Engine::WriteStep()
{
    previous_[step_] = pattern_[step_];
    pattern_[step_]  = FreshDsValue();
    glide_[step_]    = (NextBits() & 1u) != 0u;
    ds_target_       = pattern_[step_];
}

void Engine::RestoreStep()
{
    pattern_[step_] = previous_[step_];
    ds_target_      = pattern_[step_];
}

void Engine::NewPattern()
{
    /* Walk the Mod forward one clock period per step. */
    const float ratio = last_ratio_;
    for (int i = 0; i < kMaxSteps; i++)
    {
        previous_[i] = pattern_[i];
        pattern_[i]  = EncodeDs(Sin(mod_phase_ + ratio * (float)i));
        glide_[i]    = (NextBits() & 1u) != 0u;
    }
}

void Engine::AdvancePattern(const Params& p)
{
    /* LENGTH 2..16 with a dead zone at noon for 8. */
    int L;
    const float l = Clamp(p.length, 0.f, 1.f);
    if (fabsf(l - 0.5f) < 0.06f) L = 8;
    else if (l < 0.5f) L = 2 + (int)((l / 0.44f) * 6.f + 0.5f);
    else L = 8 + (int)(((l - 0.56f) / 0.44f) * 8.f + 0.5f);
    if (L < 2) L = 2;
    if (L > kMaxSteps) L = kMaxSteps;
    if (L != win_len_)
    {
        /* Non-destructive zoom: the window starts where the change landed. */
        win_start_ = step_;
        win_len_   = L;
    }
    const int rel = ((step_ - win_start_) % kMaxSteps + kMaxSteps) % kMaxSteps;
    step_ = (win_start_ + (rel + 1) % win_len_) % kMaxSteps;

    /* CHANCE never writes the pattern: at zero the stored loop returns. */
    ds_target_ = NextRandom() < p.chance ? FreshDsValue() : pattern_[step_];
    step_samples_ = samples_since_clock_ > 0 ? samples_since_clock_ : 1;
    samples_since_clock_ = 0;
}

/* ---- external input ------------------------------------------------------- */

/* Zero-crossing pitch tracking with a Schmitt trigger (+-50 mV), periods
 * averaged; invalid after 100 ms without a crossing. Returns true on a
 * rising crossing (the sync edge). */
bool Engine::TrackInput(float x)
{
    bool rose = false;
    ext_count_++;
    ext_since_++;
    if (!ext_high_ && x > 0.01f)
    {
        ext_high_ = true;
        rose      = true;
        const float period = (float)ext_count_;
        if (period > 4.f && period < sr_ / 15.f)
            ext_period_ = ext_period_ > 0.f && fabsf(period - ext_period_) < 0.25f * ext_period_
                              ? 0.6f * ext_period_ + 0.4f * period
                              : period;
        ext_count_ = 0;
        ext_since_ = 0;
    }
    else if (ext_high_ && x < -0.01f)
    {
        ext_high_ = false;
    }
    if (ext_since_ > (uint32_t)(sr_ * 0.1f)) ext_period_ = 0.f;
    return rose;
}

float Engine::QuantizeNote(const Params& p, float note) const
{
    return Quantize(GetScale(p.scale_system, p.scale), note, nullptr);
}

/* ---- render --------------------------------------------------------------- */

void Engine::Process(const Params& p, const float* ext, const float* tz,
                     float* out_sin, float* out_cos, size_t n)
{
    const Scale* scale = GetScale(p.scale_system, p.scale);

    /* --- pitch, at the block rate ---------------------------------------- */
    const bool send_osc = p.root_send == kSendOsc || p.root_send == kSendBoth;
    const bool send_ufg = p.root_send == kSendUfg || p.root_send == kSendBoth;

    const float root_note = 60.f + 36.f * Clamp(p.root, -1.f, 1.f) + 12.f * p.voct;
    int         root_step = 0;
    const float root_q    = Quantize(scale, root_note, &root_step);
    const float ds_st     = 24.f * Clamp(p.pitch_att, -1.f, 1.f) * (2.f * ds_out_ - 1.f);

    bool  qtrig_fire = false;
    float osc_note;
    if (send_osc)
    {
        int step = 0;
        if (scale)
        {
            osc_note = Quantize(scale, root_note + ds_st, &step)
                     + floorf(36.f * Clamp(p.freq, -1.f, 1.f) + 0.5f);
            if (step != last_q_step_)
            {
                if (last_q_step_ != 0x7FFFFFFF && p.qtrig) qtrig_fire = true;
                last_q_step_ = step;
            }
        }
        else
        {
            osc_note = root_note + ds_st + 36.f * Clamp(p.freq, -1.f, 1.f);
        }
    }
    else
    {
        osc_note = 60.f + ds_st + 36.f * Clamp(p.freq, -1.f, 1.f);
    }

    float osc_hz = NoteToHz(osc_note);
    const float track_hz = ext_period_ > 0.f ? sr_ / ext_period_ : 0.f;
    if (p.ext_mode == kExtTrack && track_hz > 0.f)
        osc_hz = track_hz * exp2f(3.f * Clamp(p.freq, -1.f, 1.f));
    osc_hz = Clamp(osc_hz, 0.01f, 0.45f * sr_);

    /* UFG rate. The random walk is TIME's normal (guide: "when no cable is
     * patched, a random walk is applied via the attenuverter"). */
    walk_ += (NextRandom() - 0.5f) * 0.06f;
    walk_ *= 0.9995f;
    walk_  = Clamp(walk_, -1.f, 1.f);
    float time_oct = 3.f * Clamp(p.time_att, -1.f, 1.f) * walk_;
    if (p.cv_target == kCvTime) time_oct += p.aux_cv;
    const float t = Clamp(p.time, 0.f, 1.f);
    float ufg_hz;
    if (send_ufg) ufg_hz = NoteToHz((scale ? root_q : root_note) - 96.f + 120.f * t) * exp2f(time_oct);
    else ufg_hz = 0.02f * exp2f(17.6f * t + time_oct);
    ufg_hz = Clamp(ufg_hz, 0.005f, 0.25f * sr_);

    /* Mod frequency and harmonic blend. */
    float src_hz;
    if (p.mod_track == kTrackOsc) src_hz = osc_hz;
    else if (p.mod_track == kTrackUfg) src_hz = ufg_hz;
    else src_hz = NoteToHz(scale ? root_q : root_note);

    float h = Clamp(p.harmonic + (p.cv_target == kCvHarmonic ? 0.2f * p.aux_cv : 0.f), -1.f, 1.f);
    float ratio = 1.f, mfr = 0.f;
    int   mn1 = 1, mn2 = 1;
    if (p.order == kOrderFree || h < 0.f)
    {
        ratio = exp2f(4.f * h);
    }
    else if (p.order == kOrderQuant)
    {
        if (scale)
        {
            const float sn = HzToNote(src_hz);
            ratio = exp2f((Quantize(scale, sn + 24.f * h, nullptr) - sn) / 12.f);
        }
        else
        {
            static const float kRatios[5] = {1.f, 1.5f, 2.f, 3.f, 4.f};
            ratio = kRatios[(int)Clamp(h * 4.f + 0.5f, 0.f, 4.f)];
        }
    }
    else
    {
        const float k = h * 7.f;
        const int   i = (int)k;
        mfr = k - (float)i;
        mn1 = HarmonicAt(p.order, i);
        mn2 = HarmonicAt(p.order, i + 1);
    }
    const float det = Clamp(p.detune, -1.f, 1.f);
    const float mod_hz = Clamp(src_hz * ratio * exp2f(det * fabsf(det) / 12.f), 0.01f, 0.45f * sr_);
    const float mod_inc = mod_hz * inv_sr_;
    const float mfade1  = NyquistFade(mod_hz * (float)mn1 * inv_sr_);
    const float mfade2  = NyquistFade(mod_hz * (float)mn2 * inv_sr_);
    last_ratio_ = Wrap(mod_hz / ufg_hz);

    /* --- trigger at the block rate --------------------------------------- */
    const bool rising = p.gate && !gate_prev_;
    gate_prev_ = p.gate;
    const bool sustain = p.trig_type == kTrigSustainSync || p.trig_type == kTrigSustainReverse;
    const bool reverse = p.trig_type == kTrigReverse || p.trig_type == kTrigSustainReverse;
    if (rising || qtrig_fire)
    {
        if (p.ufg_mode == kUfgOneShot && !ufg_running_)
        {
            ufg_phase_ = 0.f; ufg_dir_ = 1.f; ufg_running_ = true; ufg_completed_ = false;
        }
        else if (reverse)
        {
            ufg_dir_ = -ufg_dir_;
        }
        else if (ufg_completed_)
        {
            /* Hard sync that lets the current cycle finish first: a trigger
             * faster than the UFG locks it to a subharmonic. */
            ufg_phase_ = 0.f; ufg_dir_ = 1.f; ufg_running_ = true; ufg_completed_ = false;
        }
    }
    if (p.ufg_mode == kUfgLoop) ufg_running_ = true;

    const float skew  = Clamp(p.skew, 0.02f, 0.98f);
    const float ushp  = Clamp(p.ufg_shape, 0.f, 1.f) * 3.f;
    const int   ushp_i = ushp >= 3.f ? 2 : (int)ushp;
    const float ushp_t = ushp - (float)ushp_i;
    const float ufg_inc = ufg_hz * inv_sr_;

    /* --- modulation inputs at the block rate ------------------------------ */
    float aux_shift = 0.f, aux_depth = 0.f, aux_shape = 0.f, aux_damped = 0.f;
    switch (p.cv_target)
    {
        case kCvShift:  aux_shift  = 0.2f * p.aux_cv; break;
        case kCvDepth:  aux_depth  = 0.2f * p.aux_cv; break;
        case kCvShape:  aux_shape  = 0.2f * p.aux_cv; break;
        case kCvDamped: aux_damped = 0.2f * p.aux_cv; break;
        default: break;
    }
    const float base_morph = Clamp(p.morph, 0.f, 1.f) * 8.f + 1.6f * p.morph_cv;

    const float stretch_inc = 0.5f * Clamp(p.stretch, 0.f, 1.f) * osc_hz * inv_sr_;
    const float fold_inc    = stretch_inc + 0.004f * det * osc_hz * inv_sr_;
    const float osc_inc     = osc_hz * inv_sr_;
    const float fold_limit  = 0.45f * sr_ / (osc_hz * 3.f);
    const float tz_depth    = 2.f * Clamp(p.tzpm, 0.f, 1.f);

    float pk = 0.f;
    for (size_t i = 0; i < n; i++)
    {
        /* ---- UFG ---- */
        bool wrapped = false;
        if (ufg_running_)
        {
            const bool hold = sustain && p.gate && ufg_dir_ > 0.f && ufg_phase_ >= skew;
            if (hold) ufg_phase_ = skew;
            else ufg_phase_ += ufg_dir_ * ufg_inc;
            if (ufg_phase_ >= 1.f || ufg_phase_ < 0.f)
            {
                wrapped = true;
                ufg_completed_ = true;
                if (p.ufg_mode == kUfgLoop) ufg_phase_ = Wrap(ufg_phase_);
                else { ufg_running_ = false; ufg_phase_ = 0.f; }
            }
        }
        float U = 0.f;
        bool  ugate = false;
        if (ufg_running_)
        {
            const float x = ufg_phase_ < skew ? ufg_phase_ / skew : (1.f - ufg_phase_) / (1.f - skew);
            float s[4];
            s[0] = x;
            s[1] = 0.5f - 0.5f * Cos(0.5f * x);
            s[2] = x * x * x;
            s[3] = s[1] * s[1];
            U     = Lerpf(s[ushp_i], s[ushp_i + 1], ushp_t);
            ugate = ufg_phase_ < skew;
        }

        /* ---- external input ---- */
        bool ext_edge = false;
        if (ext) ext_edge = TrackInput(ext[i]);

        /* ---- D-S ---- */
        samples_since_clock_++;
        if (wrapped) AdvancePattern(p);
        {
            const float sm = Clamp(p.smooth, 0.f, 1.f);
            float tau = glide_[step_] ? sm : Clamp(2.f * sm - 1.f, 0.f, 1.f) * 0.5f;
            tau *= (float)step_samples_;
            ds_out_ += (ds_target_ - ds_out_) / (1.f + tau);
        }

        /* ---- modulation ---- */
        const float src = p.mod_source == kSrcDs ? ds_out_ : U;
        const float s_  = Clamp(p.shift + aux_shift + p.shift_att * src, 0.f, 1.f);
        const float d_  = Clamp(p.depth + aux_depth + p.depth_att * src, 0.f, 1.f);
        const float h_  = Clamp(p.shape + aux_shape + p.shape_att * src, 0.f, 1.f);
        const float D   = Clamp(p.damped + aux_damped + p.damped_att * src, 0.f, 1.f);

        float mp = base_morph + 8.f * p.morph_att * src;
        mp = mp - 8.f * floorf(mp * 0.125f);
        int   ni = (int)mp;
        if (ni > 7) ni = 7;
        const Vec v = Lerp(NodeVec(ni, s_, d_, h_), NodeVec((ni + 1) & 7, s_, d_, h_), mp - (float)ni);

        /* ---- damped sync: pull the OSC toward phase 0 at the leader's edge ---- */
        const bool sync_edge = p.ext_mode == kExtSync ? ext_edge : wrapped;
        if (sync_edge && D > 0.f)
        {
            const float w = phase_ >= 0.5f ? phase_ - 1.f : phase_;
            phase_ = Wrap(phase_ - D * w);
        }

        /* ---- Mod ---- */
        mod_phase_ = Wrap(mod_phase_ + mod_inc + v.xpm * out_prev_ * 0.02f);
        const float m = (1.f - mfr) * mfade1 * Sin((float)mn1 * mod_phase_)
                      + mfr * mfade2 * Sin((float)mn2 * mod_phase_);
        mod_out_ = m;
        mod_lp_ += v.m_lp * (m - mod_lp_);
        const float asym_in = (mod_lp_ + 1.f) * (mod_lp_ + 1.f) * 0.5f - 1.f;
        const float pm_mod  = v.mpm * Lerpf(mod_lp_, asym_in, v.asym);

        /* ---- self-feedback ---- */
        fb_lp_ += v.fb_lp * (fb_src_ - fb_lp_);
        const float pm_fb = v.fb * fb_lp_;

        const float pm_tz = tz ? tz_depth * tz[i] : 0.f;
        const float theta = phase_ + (pm_mod + pm_fb + pm_tz) * kInvTwoPi;

        /* ---- carrier: blended harmonics, stretched ---- */
        const float k   = v.harm * 7.f;
        const int   hi  = (int)k;
        const float hfr = k - (float)hi;
        const float n1  = (float)(hi + 1), n2 = (float)(hi + 2);
        const float a1  = (1.f - hfr) * NyquistFade(n1 * osc_inc);
        const float a2  = hfr * NyquistFade(n2 * osc_inc);
        const float t1  = n1 * theta + (n1 - 1.f) * stretch_phase_;
        const float t2  = n2 * theta + (n2 - 1.f) * stretch_phase_;
        float xs = a1 * Sin(t1) + a2 * Sin(t2);
        float xc = a1 * Cos(t1) + a2 * Cos(t2);
        const float carrier = xs;

        /* ---- harmonic wavefolding; STRETCH and DETUNE move the folded
         *      harmonics, the fundamental stays put ---- */
        if (v.fold > 0.f)
        {
            float g = 1.f + 7.f * v.fold;
            if (g > fold_limit) g = fold_limit > 1.f ? fold_limit : 1.f;
            const float mix = Clamp(4.f * v.fold, 0.f, 1.f);
            const float u1  = t1 + fold_phase_, u2 = t2 + fold_phase_;
            const float ys  = a1 * Sin(u1) + a2 * Sin(u2);
            const float yc  = a1 * Cos(u1) + a2 * Cos(u2);
            const float rs  = Sin(0.25f * g * ys) - ys;
            const float rc  = Sin(0.25f * g * yc) - yc;
            xs += mix * rs;
            xc += mix * rc;
        }

        /* ---- formant window (FOF-like AM at the fundamental) ---- */
        float env = 1.f;
        if (v.fmnt > 0.f)
        {
            const float rc  = 0.5f + 0.5f * Cos(theta);
            const float w   = powf(rc + 1e-9f, 1.f + 24.f * v.fmnt);
            const float mix = Clamp(3.f * v.fmnt, 0.f, 1.f);
            env = (1.f - mix + mix * w) * (1.f + 1.5f * v.fmnt);
        }
        /* ---- pulse-train AM from the Mod (ASYM) ---- */
        if (v.mam > 0.f)
        {
            const float pa = 0.5f * (mod_lp_ + 1.f);
            env *= 1.f - v.mam + v.mam * 1.6f * pa * pa * pa;
        }

        fb_src_   = Lerpf(carrier, xs, v.chaos);
        out_prev_ = xs;

        /* ---- pulsar AM from the UFG (not in external sync mode) ---- */
        const float g_am = p.ext_mode == kExtSync ? 1.f : 1.f - D + D * U;
        const float os = SoftClip(0.8f * xs * env * g_am);
        const float oc = SoftClip(0.8f * xc * env * g_am);
        out_sin[i] = os;
        out_cos[i] = oc;
        const float a = fabsf(os);
        if (a > pk) pk = a;

        phase_         = Wrap(phase_ + osc_inc);
        stretch_phase_ = Wrap(stretch_phase_ + stretch_inc);
        fold_phase_    = Wrap(fold_phase_ + fold_inc);

        st_.ufg      = U;
        st_.ufg_gate = ugate;
        if (i == 0) st_.morph_pos = mp;
    }

    peak_ = pk > peak_ ? pk : peak_ * 0.99f;
    st_.ds       = ds_out_;
    st_.ds_step  = step_;
    st_.ds_len   = win_len_;
    st_.osc_hz   = osc_hz;
    st_.mod_hz   = mod_hz;
    st_.ufg_hz   = ufg_hz;
    st_.track_hz = track_hz;
    st_.peak     = peak_;
}

}  // namespace tally
