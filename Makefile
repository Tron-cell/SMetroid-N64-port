# Super Metroid -> N64 port (snesrev/sm game code + libdragon platform layer)
# Build: put your own ROM at filesystem/sm.smc, then run `make`.

BUILD_DIR = build
include $(N64_INST)/include/n64.mk

N64_ROM_TITLE    = "Super Metroid 64"
N64_ROM_SAVETYPE = sram256k

GAME_SRCS := $(wildcard src/*.c)
SNES_SRCS := $(wildcard src/snes/*.c)
PLAT_SRCS := src/platform/n64/n64_main.c src/platform/n64/n64_glue.c
RSP_SRCS  := src/platform/n64/rsp_ppu.S src/platform/n64/rsp_dsp.S src/platform/n64/rsp_fx.S src/platform/n64/rsp_m7.S
SRCS      := $(GAME_SRCS) $(SNES_SRCS) $(PLAT_SRCS)

# Everything is compiled as C++ (gnu++20), so the game code can use the
# byte-order-safe LeInt types from src/types.h (enabled by SM_LE_TYPES).
# The SNES hardware code (src/snes) keeps native integer types.
SM_CXXFLAGS = -x c++ -std=gnu++20 -fpermissive -fno-exceptions -fno-rtti -fno-threadsafe-statics \
              -O2 -fno-strict-aliasing -ftrivial-auto-var-init=uninitialized -w -Wno-error -I. -Isrc -DPPU_OUT16

# snesrev's SNES hardware code uses a few names libdragon also exports.
RENAMES = -Dcart_init=snes_cart_init -Ddma_read=snes_dma_read -Ddma_write=snes_dma_write

all: sm64.z64

ifeq ($(wildcard filesystem/sm.smc),)
$(error filesystem/sm.smc not found - copy your own Super Metroid (JU) ROM there, see README.md)
endif

# Replaces libdragon's default C rule: compile .c files with the C++ compiler.
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "    [C++] $<"
	$(N64_CXX) -c $(N64_CXXFLAGS) $(SM_CXXFLAGS) $(SM_EXTRA) $(TEST_FLAGS) -MMD -o $@ $<

$(GAME_SRCS:%.c=$(BUILD_DIR)/%.o): SM_EXTRA = -DSM_LE_TYPES $(RENAMES)
$(PLAT_SRCS:%.c=$(BUILD_DIR)/%.o): SM_EXTRA = -DSM_LE_TYPES
$(SNES_SRCS:%.c=$(BUILD_DIR)/%.o): SM_EXTRA = $(RENAMES)

$(BUILD_DIR)/sm64.dfs: filesystem/sm.smc
$(BUILD_DIR)/sm64.elf: $(SRCS:%.c=$(BUILD_DIR)/%.o) $(RSP_SRCS:%.S=$(BUILD_DIR)/%.o)
sm64.z64: $(BUILD_DIR)/sm64.dfs

clean:
	rm -rf $(BUILD_DIR) sm64.z64

-include $(wildcard $(BUILD_DIR)/src/*.d $(BUILD_DIR)/src/*/*.d $(BUILD_DIR)/src/*/*/*.d)
.PHONY: all clean
