# RSP renderer test harness

Runs `src/platform/n64/rsp_ppu.S` on the cxd4 RSP interpreter (linked in
directly) over captured SNES frames, and compares every line with the C
reference renderer in `src/snes/ppu_rsp.c`.

* `build_ucode.sh rsp_ppu.S` assembles the microcode into `.text.bin` / `.data.bin`.
* `ppu_test <prefix> [-dbg] [-max N] frames.bin...` runs it. Frame dumps come
  from the big-endian rig (`tools/be_rig`, env `DUMP_FRAMES=file DUMP_EVERY=n`).
  Prints bad lines, RSP instructions per frame and DMA counts, and writes a PC
  histogram (`pc_hist.txt`, read with `rsp_prof.py`).
* `rsph.c/.h` hosts cxd4 (DMEM/IMEM/RDRAM, DMA). cxd4 needs two small local
  fixes: unaligned `SQV` and a non-blocking `message()` (see
  `../n64emu/mupen64plus-rsp-cxd4.patch`).
* `linestats.c` counts which line types occur (simple, sub screen, windows...).
* `rspcyc.c` is a cycle model fed by the interpreter's instruction trace (dual
  issue, vector / load hazards, DMA time); `ppu_test` prints its result.
* 2.5D pass (`src/platform/n64/rsp_fx.S`):
  * `fxhost [seeds]` runs `rsp_fx` on random masks and pixels (random line
    selections, so the out-of-order row loads are covered too) and compares
    every pixel with a C model of the effect.
  * `MASKCHK=1 ppu_test ...` also gives `rsp_ppu` a mask image and checks each
    row's pixel classes against the C reference renderer's z values.
  * `MASKCHK=1 FXUCODE=rsp_fx [FXIMG=dir] ppu_test ...` then runs `rsp_fx` on
    every frame (all valid lines from row 32 on, i.e. no copied lines) and
    prints its time from the cycle model; FXIMG saves before / after images
    and the mask of each frame.

* Mode 7 rows (`src/platform/n64/rsp_m7.S`): `m7host [seeds]` runs `rsp_m7`
  on random tilemaps, tiles and line lists (runs of lines, jumps, every
  start column) and compares each row with the C model of Mode7Row's
  scale-1 path; it also prints RSP cycles per row from the cycle model.

Build: `gcc -O2 -DN64 -iquote ../../src/snes -iquote ../../src -o ppu_test
ppu_test.c ../../src/snes/ppu_rsp.c rsph.c rspcyc.c <cxd4 objects>` (and
`fxhost.c` / `m7host.c` + `rsph.c rspcyc.c <cxd4 objects>` for the others).

The microcode is loaded once and reused for all frames, exactly like the ROM,
so state left in DMEM from the previous frame can't hide bugs.
