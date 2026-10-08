/*
 * Native tests for the Tally engine (no hardware). Built with ASan+UBSan by
 * test/Makefile. Every check prints on failure; the exit code is the number
 * of failures.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>
#include <vector>

#include "tally_engine.h"

using namespace tally;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond, ...)                                                  \
    do {                                                                  \
        g_checks++;                                                       \
        if (!(cond)) {                                                    \
            g_fail++;                                                     \
            printf("FAIL %s:%d  %s  -- ", __FILE__, __LINE__, #cond);     \
            printf(__VA_ARGS__);                                          \
            printf("\n");                                                 \
        }                                                                 \
    } while (0)

static constexpr float kSr = 48000.f;
static constexpr int   kBlock = 24;

struct Render
{
    std::vector<float> s, c;
};

/* Render `seconds` with fixed params; optional per-sample inputs. */
static Render run(Engine& e, const Params& p, float seconds,
                  const std::vector<float>* ext = nullptr,
                  const std::vector<float>* tz = nullptr)
{
    const int total = (int)(seconds * kSr) / kBlock * kBlock;
    Render r;
    r.s.resize(total);
    r.c.resize(total);
    for (int i = 0; i < total; i += kBlock)
        e.Process(p, ext ? ext->data() + i : nullptr, tz ? tz->data() + i : nullptr,
                  r.s.data() + i, r.c.data() + i, kBlock);
    return r;
}

/* Magnitude of the component at `hz` (Goertzel over the whole buffer). */
static float bin(const std::vector<float>& x, float hz, size_t from = 0)
{
    const size_t n = x.size() - from;
    const double w = 2.0 * M_PI * hz / kSr;
    double re = 0, im = 0;
    for (size_t i = 0; i < n; i++)
    {
        re += x[from + i] * cos(w * (double)i);
        im -= x[from + i] * sin(w * (double)i);
    }
    return (float)(2.0 * sqrt(re * re + im * im) / (double)n);
}

static float rms(const std::vector<float>& x, size_t from = 0)
{
    double a = 0;
    for (size_t i = from; i < x.size(); i++) a += (double)x[i] * x[i];
    return (float)sqrt(a / (double)(x.size() - from));
}

/* Rising zero crossings per second. */
static float crossings_hz(const std::vector<float>& x, size_t from = 0)
{
    int first = -1, last = -1, count = 0;
    for (size_t i = from + 1; i < x.size(); i++)
        if (x[i - 1] < 0.f && x[i] >= 0.f)
        {
            if (first < 0) first = (int)i;
            last = (int)i;
            count++;
        }
    if (count < 2) return 0.f;
    return (float)(count - 1) * kSr / (float)(last - first);
}

/* Fraction of energy NOT in the fundamental: 0 for a pure sine. */
static float distortion(const std::vector<float>& x, float f0, size_t from)
{
    const float a = bin(x, f0, from);
    const float r = rms(x, from);
    const float fund_rms = a / sqrtf(2.f);
    return r > 1e-6f ? 1.f - (fund_rms * fund_rms) / (r * r) : 0.f;
}

static const float kC4 = 261.6256f;

/* ------------------------------------------------------------------------- */

static void test_centre()
{
    /* Centre: waveshaping, stretch, damped and attenuverters at zero gives
     * a sine/cosine pair at every morph position. */
    for (int k = 0; k < 32; k++)
    {
        Engine e;
        e.Init(kSr);
        Params p;
        p.morph = (float)k / 32.f;
        Render r = run(e, p, 0.5f);
        const size_t from = 2400;
        const float f = crossings_hz(r.s, from);
        CHECK(fabsf(f - kC4) < 0.5f, "morph %.3f: %.2f Hz, want C4", p.morph, f);
        CHECK(distortion(r.s, kC4, from) < 0.002f, "morph %.3f sine distortion %.5f", p.morph,
              distortion(r.s, kC4, from));
        float worst = 0.f;
        for (size_t i = from; i < r.s.size(); i++)
            worst = fmaxf(worst, fabsf(r.s[i] * r.s[i] + r.c[i] * r.c[i] - 0.64f));
        CHECK(worst < 0.01f, "morph %.3f: sin^2+cos^2 deviates %.4f (not quadrature)", p.morph, worst);
    }
}

static void test_pitch()
{
    Engine e;
    e.Init(kSr);
    Params p;
    p.voct = 1.f;
    Render r = run(e, p, 0.3f);
    float f = crossings_hz(r.s, 2400);
    CHECK(fabsf(f - 2.f * kC4) < 1.f, "1 V/oct: %.2f Hz", f);

    p.voct = 0.f;
    p.freq = 1.f / 3.f;   /* +12 st */
    e.Init(kSr);
    r = run(e, p, 0.3f);
    f = crossings_hz(r.s, 2400);
    CHECK(fabsf(f - 2.f * kC4) < 1.f, "FREQ +12 st: %.2f Hz", f);

    /* Root not sent to OSC: V/Oct (a Root input) leaves the OSC alone. */
    p.freq = 0.f;
    p.voct = 1.f;
    p.root_send = kSendUfg;
    e.Init(kSr);
    r = run(e, p, 0.3f);
    f = crossings_hz(r.s, 2400);
    CHECK(fabsf(f - kC4) < 0.5f, "root to UFG only: OSC %.2f Hz, want C4", f);
}

static void test_quantizer()
{
    Engine e;
    e.Init(kSr);
    Params p;
    p.scale_system = kScaleTet;
    p.scale = 4;   /* chromatic */
    CHECK(fabsf(e.QuantizeNote(p, 60.4f) - 60.f) < 1e-4f, "chromatic 60.4 -> %.3f", e.QuantizeNote(p, 60.4f));
    CHECK(fabsf(e.QuantizeNote(p, 60.6f) - 61.f) < 1e-4f, "chromatic 60.6");
    p.scale = 0;   /* octaves */
    CHECK(fabsf(e.QuantizeNote(p, 65.f) - 60.f) < 1e-4f, "octaves 65 -> %.3f", e.QuantizeNote(p, 65.f));
    CHECK(fabsf(e.QuantizeNote(p, 67.f) - 72.f) < 1e-4f, "octaves 67 -> %.3f", e.QuantizeNote(p, 67.f));
    p.scale = 3;   /* major */
    CHECK(fabsf(e.QuantizeNote(p, 61.f) - 60.f) < 1e-4f || fabsf(e.QuantizeNote(p, 61.f) - 62.f) < 1e-4f,
          "major 61 -> %.3f", e.QuantizeNote(p, 61.f));
    CHECK(fabsf(e.QuantizeNote(p, 66.f) - 65.f) < 1e-4f || fabsf(e.QuantizeNote(p, 66.f) - 67.f) < 1e-4f,
          "major 66 -> %.3f", e.QuantizeNote(p, 66.f));
    p.scale_system = kScaleJi;
    p.scale = 3;   /* JI major: 5/4 above C is 386.31 cents */
    CHECK(fabsf(e.QuantizeNote(p, 64.f) - (60.f + 3.8631f)) < 1e-3f, "JI 5/4 -> %.4f", e.QuantizeNote(p, 64.f));
    p.scale = 4;   /* 32 harmonics: 33/32 = 53.27 c */
    CHECK(fabsf(e.QuantizeNote(p, 60.5f) - (60.f + 0.5327f)) < 1e-3f, "JI 33/32 -> %.4f", e.QuantizeNote(p, 60.5f));

    /* The quantized root reaches the OSC; FREQ moves in whole semitones. */
    p.scale_system = kScaleTet;
    p.scale = 4;
    p.root = 0.4f / 36.f;   /* +0.4 st -> C4 */
    p.freq = 0.3f / 36.f;   /* rounds to 0 */
    e.Init(kSr);
    Render r = run(e, p, 0.3f);
    const float f = crossings_hz(r.s, 2400);
    CHECK(fabsf(f - kC4) < 0.5f, "quantized root: %.2f Hz", f);
}

static float diff_rms(const Render& a, const Render& b, size_t from)
{
    double d = 0;
    for (size_t i = from; i < a.s.size(); i++) d += (double)(a.s[i] - b.s[i]) * (a.s[i] - b.s[i]);
    return (float)sqrt(d / (double)(a.s.size() - from));
}

static void test_nodes_shape_the_sound()
{
    /* DEPTH at max distorts every node. Then each control, moved from a
     * DEPTH-at-half baseline, changes the sound: on FBPM2 and XPM2 SHIFT
     * and SHAPE only shape the modulation DEPTH brings in (the guide:
     * damping filter cutoff, XPM index), so alone at Centre they are
     * silent by design. Nothing goes out of range or NaN. */
    for (int node = 0; node < kNumNodes; node++)
    {
        Params base;
        base.morph    = (float)node / 8.f;
        base.harmonic = 0.15f;   /* Mod near harmonic 2 */
        {
            Engine e;
            e.Init(kSr);
            Params p = base;
            p.depth = 1.f;
            Render r = run(e, p, 0.3f);
            const float dist = distortion(r.s, kC4, 2400);
            CHECK(dist > 0.05f, "node %s depth 1: still a sine (distortion %.4f)", NodeName(node), dist);
        }
        base.depth = 0.5f;
        Engine e0;
        e0.Init(kSr);
        Render r0 = run(e0, base, 0.3f);
        for (int ctl = 0; ctl < 3; ctl++)
        {
            Engine e;
            e.Init(kSr);
            Params p = base;
            if (ctl == 0) p.shift = 1.f;
            if (ctl == 1) p.depth = 1.f;
            if (ctl == 2) p.shape = 1.f;
            Render r = run(e, p, 0.3f);
            bool finite = true;
            float mx = 0.f;
            for (float x : r.s) { if (!std::isfinite(x)) finite = false; mx = fmaxf(mx, fabsf(x)); }
            CHECK(finite, "node %s ctl %d: non-finite", NodeName(node), ctl);
            CHECK(mx <= 1.0f, "node %s ctl %d: peak %.3f", NodeName(node), ctl, mx);
            CHECK(rms(r.s, 2400) > 0.05f, "node %s ctl %d: silent", NodeName(node), ctl);
            const float d = diff_rms(r, r0, 2400);
            CHECK(d > 0.02f, "node %s ctl %d: no change from the baseline (%.4f)", NodeName(node), ctl, d);
        }
    }
}

static void test_fuzz()
{
    /* Random sweeps of every control, changing every block. */
    Engine e;
    e.Init(kSr, 1234u);
    srand(42);
    auto rnd = [] { return (float)rand() / (float)RAND_MAX; };
    std::vector<float> s(kBlock), c(kBlock), ext(kBlock), tz(kBlock);
    bool finite = true;
    float mx = 0.f;
    for (int b = 0; b < 40000; b++)
    {
        Params p;
        p.morph = rnd(); p.freq = 2.f * rnd() - 1.f; p.shift = rnd(); p.depth = rnd();
        p.shape = rnd(); p.damped = rnd(); p.stretch = rnd();
        p.shift_att = 2.f * rnd() - 1.f; p.depth_att = 2.f * rnd() - 1.f;
        p.shape_att = 2.f * rnd() - 1.f; p.morph_att = 2.f * rnd() - 1.f;
        p.damped_att = 2.f * rnd() - 1.f; p.mod_source = rand() & 1;
        p.time = rnd(); p.skew = rnd(); p.ufg_shape = rnd(); p.ufg_mode = rand() & 1;
        p.trig_type = rand() & 3; p.chance = rnd(); p.length = rnd(); p.smooth = rnd();
        p.pitch_att = 2.f * rnd() - 1.f; p.time_att = 2.f * rnd() - 1.f; p.tzpm = rnd();
        p.root = 2.f * rnd() - 1.f; p.scale = rand() % 9; p.scale_system = rand() % 3;
        p.harmonic = 2.f * rnd() - 1.f; p.order = rand() % kNumOrders;
        p.detune = 2.f * rnd() - 1.f; p.root_send = rand() & 3; p.ext_mode = rand() % 3;
        p.mod_track = rand() % 3; p.qtrig = rand() & 1; p.cv_target = rand() % kNumCvTargets;
        p.voct = 10.f * rnd() - 5.f; p.morph_cv = 10.f * rnd() - 5.f; p.aux_cv = 10.f * rnd() - 5.f;
        p.gate = rand() & 1;
        for (int i = 0; i < kBlock; i++) { ext[i] = 2.f * rnd() - 1.f; tz[i] = 2.f * rnd() - 1.f; }
        e.Process(p, ext.data(), tz.data(), s.data(), c.data(), kBlock);
        for (int i = 0; i < kBlock; i++)
        {
            if (!std::isfinite(s[i]) || !std::isfinite(c[i])) finite = false;
            mx = fmaxf(mx, fmaxf(fabsf(s[i]), fabsf(c[i])));
        }
        if (b % 5000 == 0 && rnd() < 0.5f) e.NewPattern();
        if (b % 777 == 0) e.WriteStep();
        if (b % 999 == 0) e.RestoreStep();
    }
    CHECK(finite, "fuzz: non-finite output");
    CHECK(mx <= 1.0f, "fuzz: peak %.4f", mx);
    const Status& st = e.status();
    CHECK(std::isfinite(st.ds) && st.ds >= 0.f && st.ds <= 1.f, "fuzz: ds %.3f", st.ds);
}

static void test_stretch()
{
    /* Without waveshaping STRETCH does nothing (bit-identical). */
    Engine a, b;
    a.Init(kSr);
    b.Init(kSr);
    Params p;
    Params q = p;
    q.stretch = 1.f;
    Render ra = run(a, p, 0.2f), rb = run(b, q, 0.2f);
    CHECK(memcmp(ra.s.data(), rb.s.data(), ra.s.size() * sizeof(float)) == 0,
          "stretch without waveshaping changed the output");

    /* With SHIFT blending harmonics 1+2: stretch moves harmonic 2 away from
     * 2 f0 while f0 stays. At stretch 1 harmonic n gains 0.5 (n-1) f0. */
    p.morph = (float)kFbpm / 8.f;
    p.shift = 0.5f / 7.f;        /* half fundamental, half 2nd */
    q = p;
    q.stretch = 1.f;
    a.Init(kSr);
    b.Init(kSr);
    ra = run(a, p, 1.f);
    rb = run(b, q, 1.f);
    const float f0a = bin(ra.s, kC4, 4800), f0b = bin(rb.s, kC4, 4800);
    const float h2a = bin(ra.s, 2.f * kC4, 4800), h2b = bin(rb.s, 2.f * kC4, 4800);
    const float h2s = bin(rb.s, 2.5f * kC4, 4800);
    CHECK(fabsf(f0a - f0b) < 0.02f, "stretch moved the fundamental: %.3f -> %.3f", f0a, f0b);
    CHECK(h2a > 0.2f && h2b < 0.05f, "2nd harmonic not moved: %.3f -> %.3f", h2a, h2b);
    CHECK(h2s > 0.2f, "stretched 2nd harmonic not at 2.5 f0: %.3f", h2s);

    /* With folding: the fundamental survives, the folded harmonics move. */
    p = Params();
    p.morph = (float)kFbpm / 8.f;
    p.shape = 0.6f;
    q = p;
    q.stretch = 0.6f;
    a.Init(kSr);
    b.Init(kSr);
    ra = run(a, p, 1.f);
    rb = run(b, q, 1.f);
    const float h3a = bin(ra.s, 3.f * kC4, 4800), h3b = bin(rb.s, 3.f * kC4, 4800);
    CHECK(bin(rb.s, kC4, 4800) > 0.3f, "fold+stretch lost the fundamental: %.3f", bin(rb.s, kC4, 4800));
    CHECK(h3a > 0.03f && h3b < 0.5f * h3a, "fold harmonic 3 did not move: %.3f -> %.3f", h3a, h3b);
}

static void test_ufg()
{
    /* Loop: the UFG rate (free, root not sent) follows 0.02 * 2^(17.6 t). */
    Engine e;
    e.Init(kSr);
    Params p;
    p.time = 0.5f;
    run(e, p, 0.01f);
    const float want = 0.02f * exp2f(8.8f);
    CHECK(fabsf(e.status().ufg_hz - want) < 0.01f * want, "ufg %.3f Hz, want %.3f", e.status().ufg_hz, want);

    /* Count UFG cycles via the gate's rising edges. */
    e.Init(kSr);
    int rises = 0;
    bool prev = false;
    std::vector<float> s(kBlock), c(kBlock);
    for (int b = 0; b < (int)(kSr / kBlock); b++)
    {
        e.Process(p, nullptr, nullptr, s.data(), c.data(), kBlock);
        if (e.status().ufg_gate && !prev) rises++;
        prev = e.status().ufg_gate;
    }
    CHECK(abs(rises - (int)want) <= 1, "ufg %d cycles in 1 s, want ~%.1f", rises, want);

    /* One-shot: silent until triggered, then one cycle. */
    p.ufg_mode = kUfgOneShot;
    p.time = log2f(8.f / 0.02f) / 17.6f;   /* 8 Hz: a 125 ms cycle */
    e.Init(kSr);
    run(e, p, 0.05f);   /* the boot cycle may still run from phase 0... */
    run(e, p, 0.3f);
    CHECK(e.status().ufg == 0.f, "one-shot idle but ufg = %.3f", e.status().ufg);
    p.gate = true;
    run(e, p, 0.002f);
    p.gate = false;
    run(e, p, 0.02f);
    CHECK(e.status().ufg > 0.f, "one-shot did not start on the trigger");
    run(e, p, 0.3f);
    CHECK(e.status().ufg == 0.f, "one-shot did not finish: %.3f", e.status().ufg);

    /* Damped at max with a slow one-shot UFG: silence between hits. */
    p.damped = 1.f;
    e.Init(kSr);
    run(e, p, 0.5f);
    Render r = run(e, p, 0.2f);
    CHECK(rms(r.s) < 1e-4f, "damped max, idle UFG: rms %.5f", rms(r.s));
    p.gate = true;
    run(e, p, 0.002f);
    p.gate = false;
    r = run(e, p, 0.06f);
    CHECK(rms(r.s) > 0.05f, "damped max, struck: rms %.4f", rms(r.s));
}

static void test_damped_sync()
{
    /* Hard sync (damped 1) to an audio-rate UFG puts the fundamental at the
     * UFG's rate; damped 0 leaves the OSC free. */
    Engine e;
    e.Init(kSr);
    Params p;
    p.time = log2f(110.f / 0.02f) / 17.6f;   /* UFG 110 Hz */
    p.damped = 1.f;
    p.skew = 0.98f;
    Render r = run(e, p, 1.f);
    CHECK(bin(r.s, 110.f, 4800) > 0.02f, "hard sync: no 110 Hz component (%.4f)", bin(r.s, 110.f, 4800));
    CHECK(bin(r.s, 117.f, 4800) < 0.5f * bin(r.s, 110.f, 4800), "hard sync: not periodic at 110 Hz");
}

static void test_pattern()
{
    Engine e;
    e.Init(kSr);
    Params p;
    p.time = log2f(200.f / 0.02f) / 17.6f;   /* fast clock: 200 steps/s */
    p.length = 0.5f;                         /* 8 steps */
    p.smooth = 0.f;
    std::vector<float> s(kBlock), c(kBlock);

    /* Collect the step values over 64 clocks. */
    auto collect = [&](int clocks) {
        std::vector<float> v;
        int last = e.status().ds_step;
        while ((int)v.size() < clocks)
        {
            e.Process(p, nullptr, nullptr, s.data(), c.data(), kBlock);
            if (e.status().ds_step != last)
            {
                last = e.status().ds_step;
                v.push_back(e.status().ds);
            }
        }
        return v;
    };
    auto periodic = [](const std::vector<float>& v, int L) {
        for (size_t i = L; i < v.size(); i++)
            if (fabsf(v[i] - v[i - L]) > 1e-6f) return false;
        return true;
    };
    collect(4);
    std::vector<float> locked = collect(48);
    CHECK(periodic(locked, 8), "chance 0, length 8: not an 8-step loop");
    CHECK(e.status().ds_len == 8, "length noon: %d", e.status().ds_len);

    p.length = 0.0f;
    collect(2);
    std::vector<float> two = collect(24);
    CHECK(e.status().ds_len == 2, "length min: %d", e.status().ds_len);
    CHECK(periodic(two, 2), "length 2: not a 2-step loop");

    p.length = 1.0f;
    collect(2);
    CHECK(e.status().ds_len == 16, "length max: %d", e.status().ds_len);
    std::vector<float> sixteen = collect(48);
    CHECK(periodic(sixteen, 16), "length 16: not periodic");

    float before[kMaxSteps];
    for (int i = 0; i < kMaxSteps; i++) before[i] = e.PatternValue(i);
    p.chance = 1.f;
    collect(64);
    for (int i = 0; i < kMaxSteps; i++)
        CHECK(e.PatternValue(i) == before[i], "chance wrote the pattern at step %d", i);
    p.chance = 0.f;
    collect(1);
    std::vector<float> back = collect(32);
    CHECK(periodic(back, 16), "chance back to 0: loop not restored");

    /* WriteStep then RestoreStep returns the step. */
    const int at = e.status().ds_step;
    const float was = e.PatternValue(at);
    e.WriteStep();
    e.RestoreStep();
    CHECK(e.PatternValue(at) == was, "restore step");

    /* The pattern survives a round trip through the preset format. */
    uint8_t lv[kMaxSteps];
    uint16_t gl = 0;
    e.GetPattern(lv, &gl);
    Engine f;
    f.Init(kSr, 99u);
    f.SetPattern(lv, gl);
    for (int i = 0; i < kMaxSteps; i++)
        CHECK(fabsf(f.PatternValue(i) - e.PatternValue(i)) < 1e-6f, "pattern round trip at %d", i);

    /* D-S modulating pitch moves the OSC. */
    bool any_diff = false;
    float v0 = -1.f;
    for (int i = 0; i < kMaxSteps; i++)
    {
        if (v0 < 0.f) v0 = e.PatternValue(i);
        if (fabsf(e.PatternValue(i) - v0) > 1e-3f) any_diff = true;
    }
    CHECK(any_diff, "pattern is flat");
}

static void test_track()
{
    /* A 220 Hz square on J1 with EXT = Track: the OSC follows it. */
    Engine e;
    e.Init(kSr);
    Params p;
    p.ext_mode = kExtTrack;
    const int n = (int)(0.5f * kSr) / kBlock * kBlock;
    std::vector<float> ext(n);
    for (int i = 0; i < n; i++) ext[i] = fmodf((float)i * 220.f / kSr, 1.f) < 0.5f ? 0.5f : -0.5f;
    Render r = run(e, p, 0.5f, &ext);
    CHECK(fabsf(e.status().track_hz - 220.f) < 1.f, "tracked %.2f Hz", e.status().track_hz);
    const float f = crossings_hz(r.s, 12000);
    CHECK(fabsf(f - 220.f) < 1.5f, "OSC follows the track: %.2f Hz", f);

    /* Off: ignored. */
    p.ext_mode = kExtOff;
    e.Init(kSr);
    r = run(e, p, 0.5f, &ext);
    CHECK(fabsf(crossings_hz(r.s, 12000) - kC4) < 0.5f, "ext off still moved the OSC");
}

static void test_tzpm()
{
    /* Audio on J2 with TZPM up adds sidebands; at zero it is ignored. */
    Engine a, b;
    a.Init(kSr);
    b.Init(kSr);
    Params p;
    const int n = (int)(0.5f * kSr) / kBlock * kBlock;
    std::vector<float> tz(n);
    for (int i = 0; i < n; i++) tz[i] = 0.5f * sinf(2.f * (float)M_PI * 100.f * (float)i / kSr);
    Render ra = run(a, p, 0.5f, nullptr, &tz);
    p.tzpm = 1.f;
    Render rb = run(b, p, 0.5f, nullptr, &tz);
    CHECK(distortion(ra.s, kC4, 2400) < 0.002f, "tzpm 0 not clean");
    CHECK(bin(rb.s, kC4 + 100.f, 2400) > 0.05f, "tzpm: no sideband (%.4f)", bin(rb.s, kC4 + 100.f, 2400));
}

static void test_cpu()
{
    /* Host time per 24-frame block with everything on; the budget is 500 us. */
    Engine e;
    e.Init(kSr);
    Params p;
    p.morph = 0.4f; p.shift = 0.5f; p.depth = 0.7f; p.shape = 0.6f; p.stretch = 0.5f;
    p.damped = 0.3f; p.shift_att = 0.3f; p.depth_att = 0.2f; p.morph_att = 0.1f;
    p.time = 0.7f; p.chance = 0.3f; p.smooth = 0.5f; p.tzpm = 0.5f;
    std::vector<float> s(kBlock), c(kBlock), ext(kBlock, 0.1f), tz(kBlock, 0.1f);
    const int blocks = 20000;
    auto t0 = std::chrono::steady_clock::now();
    for (int b = 0; b < blocks; b++) e.Process(p, ext.data(), tz.data(), s.data(), c.data(), kBlock);
    auto t1 = std::chrono::steady_clock::now();
    const double us = std::chrono::duration<double, std::micro>(t1 - t0).count() / blocks;
    printf("host: %.2f us per 24-frame block (budget 500 us)\n", us);
}

int main()
{
    test_centre();
    test_pitch();
    test_quantizer();
    test_nodes_shape_the_sound();
    test_fuzz();
    test_stretch();
    test_ufg();
    test_damped_sync();
    test_pattern();
    test_track();
    test_tzpm();
    test_cpu();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
