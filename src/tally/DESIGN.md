# Tally on the Alchemy Lab — design

Tally is a clean-room complex oscillator for the Alchemy Lab V2, in the spirit
of fancyyyyy × grrrwaaa's **K-ACCUMULATOR** (fancysynthesis.net; 32 HP, dual
Daisy, firmware closed). Tally is not affiliated with Fancy Synthesis, contains none of
their code, and is not named after their product. Everything here was written
from one public source: the **K-ACCUMULATOR Quick-Start Guide** (11 pages,
fetched 2026-10-08). The guide says a full technical manual is "in progress".
The Perfect Circuit interview was behind a bot wall. grrrwaaa's public GitHub
(oopsy, libDaisy fork, gen~ material) has no K-ACCUMULATOR code.

So every behaviour below is tagged:
- **[described]**: the guide says what it does, and Tally does that.
- **[invented]**: the guide only names it or says what it sounds like. The
  algorithm is ours and may differ from the original.

## Panel

Front view: `[P1] B1 [P2] / [P3] B2 [P4] / [P5] B3 [P6]`. B2 cycles four pages;
holding B3 shows the fifth.

| Page (B2 colour) | P1 | P2 | P3 | P4 | P5 | P6 |
|---|---|---|---|---|---|---|
| **OSC** (green) | MORPH | FREQ ±3 oct | SHIFT | DEPTH | SHAPE | DAMPED/PULSAR |
| **UFG** (cyan) | TIME | SKEW | U.SHAPE | STRETCH | LOOP / ONE-SHOT | TRIG TYPE (4) |
| **PATTERN** (amber) | CHANCE | LENGTH | SMOOTH | PITCH att (Δ–Σ → OSC) | TIME att (random walk) | TZPM depth (J2) |
| **MODS** (purple) | SHIFT att | DEPTH att | SHAPE att | MORPH att | DAMPED att | SOURCE (UFG / Δ–Σ) |
| **ROOT** (hold B3) | ROOT ±3 oct about C4 | SCALE (9) | HARMONIC | ORDER (5) | DETUNE | ROUTE (OSC / both / UFG / none) |

Buttons:
- **B1** triggers the UFG and, held, is its gate. It ORs with any jack mapped to UFG trigger (J5 by default).
  - On PATTERN: a tap writes a new value at the current step, a double tap
    undoes it, and holding for 1 s writes a whole new pattern. These stand in
    for the Pattern switch.
  - With B3 held: cycles the scale system Off → 12-TET → JI (the Scale
    button). B3's LEDs show it: grey, blue, amber.
- **B2** goes to the next page. **B2+B3 for 2 s** opens Settings.

Settings: **EXT (J1)** Off/Sync/Track and **MOD TRACKS** Root/OSC/UFG
(page 0); the SD picker and **Q.TRIG** (page 1); the **CV map** (page 2,
below).

What the original has that the Lab has no room for, and where it went:
- **Attenuverters and normalisation buttons:** the original has one per CV
  input, with a button choosing UFG or Δ–Σ. Here they are the MODS page, with
  one SOURCE for all five, because nothing on the Lab is patched by default.
  The internal normal is always on, and the attenuverter at noon turns it off.
- **Root CRS/FIN/OCT button:** dropped. ROOT is one continuous ±36 st knob,
  and FREQ moves in whole semitones while a scale is on.
- **Root Send button:** became the ROUTE selector. **OSC/UFG tracking
  button:** became Settings MOD TRACKS.
- **Morph encoder:** became a pot, once round the ring over its travel. The
  encoder's coarse/fine and double-press snap are dropped.
- **UFG TZFM input, separate Δ–Σ clock input, and Rung Divisions header:** no
  jacks or header for them, so dropped.
- **Double-length (32-step) patterns:** dropped (16 max).
- **Skew CV with the Δ–Σ normal:** dropped.

## Jacks

| Jack | Use |
|---|---|
| J1 | EXT: damped-sync leader (Sync) or the pitch tracked (Track). AC-coupled codec input. **[described]** |
| J2 | TZPM: through-zero phase modulation into the OSC, depth on PATTERN P6. AC-coupled. **[described]** |
| J3 | V/Oct for Root (calibrated `Volts()`). |
| J4 | Mappable CV (default MORPH, 1.6 nodes per volt round the ring). |
| J5 | Mappable CV (default UFG trigger / gate: on above 1.5 V, off below 0.5 V). |
| J6 | Mappable CV (default SHAPE, 0.2 per volt). |

### The CV map (`cv_map.{h,cpp}`)

J4, J5 and J6 each drive any of 40 destinations with an amount: every knob
on every page (30), four Settings choices (scale system, EXT mode, MOD
TRACKS, Q.TRIG) and five actions (UFG trigger, write step, restore step,
new pattern, next scale system). J3 stays V/Oct. The defaults are the old
fixed jacks, so nothing changes until a jack is remapped.

Set on **Settings page 2**, one jack per row of the panel: P1/P2 J4, P3/P4
J5, P5/P6 J6. Destination on the left, amount on the right (an
attenuverter: full right +1, noon off, full left inverted). The settings
save with the rest and carry `Ident`s (`cv_j4_dest`, `cv_j4_amount`, …), so
the web control page shows them as menus.

Scaling, at amount +1:
- **Knobs:** ±5 V sweeps the whole knob (0.2 of its range per volt).
  FREQ, ROOT and TIME are 1 V/oct; MORPH is 1.6 nodes per volt and wraps.
- **Selectors and Settings choices:** 0–5 V steps through every option from
  where the knob sits.
- **Actions:** gates (Schmitt, 1.5 V / 0.5 V); the action fires on the
  rising edge. UFG trigger is the level, so a held gate sustains.

The map works on a per-block copy of the panel (every 0.5 ms), so CV moves
the sound but not the knob rings, and a mapped Settings choice never
changes the saved setting. Nothing the map touches is written back.
| J7 | UFG function out, 0–5 V (STM32 DAC, once per block). |
| J8 | Δ–Σ out, 0–5 V (STM32 DAC, once per block). |
| J9 / J10 | SINE / COSINE: the stereo pair. **[described]** |

## The engine (`tally_engine.{h,cpp}`, portable C++, native-tested)

**Centre** [described]: SHIFT, DEPTH, SHAPE, DAMPED and STRETCH at zero, with
every attenuverter at noon, gives a pure sine and cosine wherever MORPH is.
Every node's parameter vector is zero at s = d = h = 0. `test_centre` checks
32 morph positions and quadrature, and the emulator test `centre.emu` checks
it on the panel.

**The morph ring:** eight nodes in the guide's order and LED colours (green
self-PM, yellow PM from the Mod, red cross-PM). Clockwise from the top: FMNT,
FBPM2, 2OP2, XPM2, ASYM, then XPM, 2OP, FBPM back to FMNT.
- [described] What each node's three controls do, from the Morph Reference page.
- [invented] Each node maps (SHIFT, DEPTH, SHAPE) onto one shared 11-term
  parameter vector: harmonic blend, fold, self-PM index and damping, Mod-PM
  index and damping, cross-PM, formant window, Mod AM, asymmetry, chaos.
  MORPH linearly interpolates the vectors of neighbouring nodes per sample.
  That makes it a continuous morph through one voice (the guide: "every mode
  is built from the same four core sine waves") rather than a crossfade of
  separate renders.
- [invented] The numeric ranges (e.g. FBPM's feedback index up to 1.6 rad,
  FBPM2's 2.6 rad fed back through the folder for the "chaotic, subharmonic
  latching" character).

**SHIFT, blended harmonic frequency shifting:** [described] "blends between
the two nearest integer harmonics of the fundamental". Implemented exactly
that way: a crossfade of harmonics n and n+1 (up to 8) of the phase-modulated
carrier. Partials fade out approaching Nyquist.

**DEPTH, phase modulation:** [described] self-feedback PM (sine → saw) on the
green nodes; PM from the Mod on yellow and red; cross-feedback into the Mod's
phase on red. [invented] Damping is a one-pole low-pass on the modulator or
feedback path; the cutoffs are ours.

**SHAPE, harmonic wavefolding:** [invented] `sin(π/2 · g · x)` with g up to 8,
crossfaded in, and g limited so the folded harmonics stay under Nyquist.

**STRETCH:** [described] "stretches waveshaped harmonics while keeping the
fundamental constant; only has an effect when waveshaping is active".
[invented]
- On SHIFT's harmonics: harmonic n's phase is `n·θ + (n−1)·ψ`, where ψ runs at
  0.5·stretch·f₀. Harmonic 1 never moves, and harmonic n gains
  0.5·(n−1)·stretch·f₀. This is phase-continuous.
- On the folder: the fold residual (folded − unfolded) is evaluated on a phase
  advanced by ψ, so the folded harmonics move and the fundamental does not.
- Tested: bit-identical with no waveshaping. With SHIFT, harmonic 2 moves from
  2 f₀ to 2.5 f₀ and f₀ is unchanged.

**DETUNE** [described]: detunes the Mod (up to ±1 st, square-law so it is fine
near noon) and also drifts the folder. [invented] The folder drift is a
0.004·detune·f₀ phase walk on the fold residual.

**DAMPED / PULSAR** [described]: progressive sync from the UFG plus AM from the
UFG, from nothing up to hard sync with full gating. [invented] The algorithm:
- At each leader edge (a UFG wrap, or J1's rising zero-crossing in EXT Sync),
  the OSC phase is pulled toward the nearest zero by the DAMPED fraction.
  Pulling toward the *nearest* zero is what locks a slower OSC to subharmonics.
- The output is multiplied by `1 − D + D·U` (U = the UFG's output).
- In EXT Sync mode the AM is off: the guide doesn't say, and a UFG that runs
  free while J1 leads would chop the sound.

**Mod oscillator:**
- [described] HARMONIC above noon blends harmonics, and below noon is a free
  drop. ORDER's endpoints are free and quantised, the centre is all harmonics,
  clockwise is odd and counter-clockwise is even. The Mod tracks Root, OSC or
  UFG, with quantisation included.
- [invented] ORDER is five zones (Free, Even, All, Odd, Quantised), not a
  continuous spread. "Quantised" with no scale uses the ratios 1, 3/2, 2, 3, 4.

**UFG:**
- [described] Loop or one-shot; TIME tracks 1 V/oct; SKEW tilts the ramp and
  sets the gate width; U.SHAPE runs linear → sine → exponential → raised
  cosine. The four trigger types are as the guide lists them. Its
  wraps clock the Δ–Σ. TIME's unpatched normal is a random walk.
- [invented]
  - "Raised cosine" is the sine curve squared, and "exponential" is x³.
  - Hard sync "lets the current cycle finish" by ignoring triggers until the
    cycle has wrapped.
  - The random walk spans ±3 octaves at full attenuverter.
- **Not done:** the guide's "advanced anti-aliasing". The UFG is a naive
  ramp/triangle, which aliases at high audio rates.

**Δ–Σ pattern generator:**
- [described] Clocked by the UFG. CHANCE is the probability of new values; at
  zero the loop is locked, and new values never overwrite it, so CHANCE back to
  zero restores the pattern. LENGTH runs 2–16 steps with a noon dead zone at 8,
  and is a non-destructive zoom starting at the current step. SMOOTH glides on
  per-pattern fixed steps; at audio rate, a filter.
- [invented] What "delta-sigma encoding of the Mod oscillator" means here: at
  each clock, the Mod's instantaneous value goes through a first-order, 8-level
  error-feedback quantizer whose error carries to the next step. So the pattern's
  character follows the Mod/UFG frequency ratio. A new pattern samples the Mod
  forward one clock period per step.
- [invented] SMOOTH past noon slews every step, not only the flagged ones.

**Root and scales:**
- [described] ROOT can be quantised to 12-TET or JI and routed to OSC, UFG,
  both or neither.
- [described] SCALE's layout: octaves at both ends, the densest at noon,
  standard scales to the left and exotic ones to the right.
- [invented] The scales themselves. The original has 21 per system; Tally has
  9 per system:
  - 12-TET: octaves, fifths, pentatonic, major, **chromatic**, whole tone,
    octatonic, hirajoshi, octaves.
  - JI: 1/1, 1/1+3/2, PM ratios (1, 5/4, 3/2, 7/4), JI major,
    **harmonics 32–63** (the guide's "32-step microtonal"), 7-limit, utonal,
    11-limit, 1/1.
- [changed] The guide puts ROOT's centre at MIDI note 0 (8.2 Hz) for MIDI-CV
  integration. Tally's centre is C4, so the first boot is audible.

**Q.TRIG** [described]: with Root to the OSC and a scale on, each change of
the quantised step triggers the UFG.

**EXT Track** [described]: zero-crossing pitch tracking; pitch only, not
amplitude. [invented] A ±50 mV Schmitt trigger, periods smoothed, and the
track dropped after 100 ms with no crossing. FREQ becomes a ±3 octave ratio.

## Persistence

- **Pages and Settings:** saved by the SDK.
- **TallyExtras** (schema tag `'TLY'`): the CPU readout, the scale system, and
  the stored pattern as 16 three-bit levels plus glide flags. So a pattern
  survives a power cycle.
- **Home slot 2:** 0 is the migration slot, 3 is Stencil, and 4–15 belong to the other family
  firmwares, which leaves 1 for hand-saved presets.
- **First boot:** Centre, Root C4 to the OSC, a ~2.6 Hz looping UFG, and a
  locked 8-step pattern.

## Memory and CPU (build 2026-10-08)

| | |
|---|---|
| Image | 327,080 B of the 480 KB SRAM region (66.5 %) |
| DTCM | 85 KB of 128 KB |
| Engine state | ~0.3 KB, plus a 4 KB sine table |
| Host CPU | 1.65 µs per 24-frame block at -O2 (Apple M-series), against 500 µs |

The M7 is roughly 10–30× slower per operation than the host. That puts the
estimate at 5–10 % load, but it is **unmeasured**: watch B2, which turns red
above 80 %, and the P1 boot readout.

## Tests

- `make test` (from `test/`): 298 native checks under ASan+UBSan, including
  the CV map's scaling (1 V/oct through the engine, zone stepping, gate
  hysteresis, every destination at ±5 V staying finite).
  - Centre at 32 morph positions, plus quadrature
  - V/oct, FREQ and ROUTE
  - Both scale systems' quantisers
  - Every node × control changing the sound
  - A 40,000-block fuzz of every control, staying finite and ≤ 1.0
  - STRETCH keeping f₀
  - UFG rate, one-shot, and damped gating
  - Hard sync
  - Pattern lock, length, chance-restore, write/undo and the preset round trip
  - EXT track and TZPM
- The alchemy-lab emulator (`emu.mk`, `emu-tests/*.emu`): six scripts with 67
  checks, including the generic smoke test, all passing. They cover Centre and
  V/Oct, the waveshapers, page cycling and J7's rate, damped one-shot hits
  from J5 and B1, the Δ–Σ on J8, the new-pattern hold, and the scale system.
- `emu-tests-hostlink/cvmap.mjs` (emu-hostlink emulator): remaps J4/J5/J6
  over HostLink, as the control page would, then listens: J6 → FREQ +1 V is
  C5, J5 → SHIFT moves to harmonic 2, J4 → Off changes nothing (9 checks).

```
EMU_ALCHEMY_DIR=~/tim-os/tally-alchemy/lib/alchemy-sdk \
EMU_LIBDAISY_DIR=~/tim-os/tally-alchemy/lib/libDaisy \
make -C ~/tim-os/alchemy-lab/.claude/worktrees/emu/emu test-custom FW_DIR=<this repo>
```

## Open (nothing here has run on hardware)

- CPU on the M7, and whether `-O3` holds the per-sample node interpolation.
- Output level: 0.8 full scale ≈ ±4 V. It hasn't been compared with anything.
- The STM32 DAC writes on J7/J8 from the audio callback. Break does the same,
  and it is also unverified there.
- The sound itself. Every [invented] range was set by the tests, not by ear.
