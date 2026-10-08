# The alchemy-lab emulator (branch emu-custom):
#
#   make -C <alchemy-lab>/emu FW=custom FW_DIR=<this repo>
#   make -C <alchemy-lab>/emu test-custom FW_DIR=<this repo>   (emu-tests/*.emu)
#
# Described here rather than read from the Makefile because picker.cpp must
# stay out: the emulator stubs the SD picker itself (duplicate symbols).
# A worktree with empty lib/ submodules: export EMU_ALCHEMY_DIR and
# EMU_LIBDAISY_DIR as absolute paths to a checkout that has them.
EMU_TARGET       := tally_alchemy
EMU_CPP_SOURCES  := src/tally/tally_alchemy.cpp src/tally/tally_engine.cpp
EMU_C_SOURCES    :=
EMU_C_INCLUDES   := -Isrc/tally -Isrc/common
EMU_C_DEFS       := -DTALLY_VERSION=\"emu\" -DTALLY_GIT_HASH=\"emu\"
EMU_ALCHEMY_DIR  ?= lib/alchemy-sdk
EMU_LIBDAISY_DIR ?= lib/libDaisy
EMU_LABELS       := MORPH|FREQ|SHIFT|DEPTH|SHAPE|DAMPED
EMU_LABELS_ALT   := ROOT|SCALE|HARMONIC|ORDER|DETUNE|ROUTE
# (On the emu-hostlink emulator the labels come from the firmware's own
# VirtualKnob names on whatever page is showing; these are the fallback.)
