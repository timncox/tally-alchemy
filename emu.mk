# Tally on the Alchemy Lab emulator (github.com/timncox/alchemy-lab, branch
# claude/emu-hostlink, which has Phone mode):
#   make FW=custom FW_DIR=<this folder>
# Phone mode in the emulator: add --usb phone --hostlink <port>.
# The emulator stubs the SD picker and the USB device (src/stubs_phone.cpp
# there stands in for usb_audio.c's MIDI hooks, so uac_none.c is left out).
EMU_DIR          := $(CURDIR)
EMU_TARGET       := tally_alchemy
EMU_CPP_SOURCES  := src/tally/tally_alchemy.cpp src/tally/tally_engine.cpp src/tally/cv_map.cpp \
                    src/common/usb_shared.cpp src/common/midi_usb_transport.cpp src/common/phone_ext.cpp
EMU_EXTRA_SRC    := $(EMU_DIR)/src/stubs_phone.cpp
EMU_C_INCLUDES   := -Isrc/common -Isrc/tally -I$(EMU_DIR)/include/fw/usbd
EMU_C_DEFS       := -DALCHEMY_BOARD_V2 -DTALLY_VERSION=\"emu\" -DTALLY_GIT_HASH=\"emu\"
EMU_LABELS       := MORPH|FREQ|SHAPE|P4|P5|P6
