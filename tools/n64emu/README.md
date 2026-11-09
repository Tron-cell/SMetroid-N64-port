# Headless N64 emulator for testing

Used to boot `sm64.z64` without a screen and check the RSP renderer against
the C reference (`make TEST_FLAGS=-DRSP_SELFCHECK BUILD_DIR=build_check`, then
read the `SELFCHECK` lines in the log).

Parts (Linux):

| Part | Upstream commit | Patch here |
|---|---|---|
| mupen64plus-core | `ba95bab92a76744753bfe61470823a4937850ab0` | `mupen64plus-core.patch` |
| mupen64plus-rsp-cxd4 (accurate RSP interpreter) | `00906a92641c540a64e4e7505012157315314810` | `mupen64plus-rsp-cxd4.patch` |
| mupen64plus-ui-console | `c8ac4862a019d7885b24927d9b4db5dd3e42a528` | none |

Stock mupen64plus can't run current libdragon ROMs. The core patch adds what
libdragon needs and real hardware has:

* 64-bit XKPHYS addresses (`0x9000_0000_xxxx_xxxx`), used by libdragon for PI/cartridge access.
* `LLD` / `SCD` instructions.
* MI repeat mode (libdragon's hardware memset).
* Interrupt fixes: an exception no longer loses its cause code when an interrupt
  arrives at the same moment, and a stale interrupt check no longer fires while
  interrupts are masked. Without these, libdragon's lazy FPU save in interrupt
  handlers crashes.
* Debug aids: PC histogram/trace env vars (`M64STOP`, `M64SKIP`, `M64PEEK`) in the pure interpreter.

Build the core with `make all OSD=0 NO_ASM=1 DEBUGGER=0 VULKAN=0` (needs SDL2),
then run:

    mupen64plus --corelib libmupen64plus.so.2 --gfx dummy --audio dummy --input dummy \
      --rsp mupen64plus-rsp-cxd4-sse2.so --noosd --nospeedlimit --verbose sm64.z64

`debugf` output appears as `IS64:` lines. Timing in this emulator is not
representative of real hardware; use ares or a console for FPS numbers.
