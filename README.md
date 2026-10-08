# Tally — a complex oscillator for the Alchemy Lab

Tally is firmware for the Hermetic Modular Alchemy Lab V2. It is a clean-room
complex oscillator in the spirit of fancyyyyy × grrrwaaa's **K-ACCUMULATOR**
(fancysynthesis.net): a sine/cosine carrier with a morphing waveshaping matrix,
a Mod oscillator, a function generator (UFG) and a delta-sigma pattern
generator.

Tally is not affiliated with Fancy Synthesis. It contains none of their code;
the K-ACCUMULATOR firmware is closed. Everything here was written from the
public quick-start guide's descriptions. `src/tally/DESIGN.md` marks which
parts the guide describes and which were invented to fill the gaps.

    make libdaisy     build lib/libDaisy once after cloning
    make              build/tally_alchemy.bin (BOOT_SRAM)
    make test         native engine tests (no hardware)

MIT licence.
