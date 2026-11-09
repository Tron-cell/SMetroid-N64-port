# Super Metroid 64: N64 port

This folder holds the snesrev/sm game code (Super Metroid rewritten in C, upstream
commit in `SNESREV\\\\\\\_COMMIT`) plus a new libdragon platform layer that replaces the
PC version's SDL2 code. It is milestone 3 from `../NOTES.md`: running the real game
logic on the N64.

## Status

|Part|State|
|-|-|
|Compiles and links with libdragon (preview branch, GCC 16)|Done|
|**Big-endian / alignment fix**|**Done. Verified frame-for-frame against the PC version on a big-endian MIPS CPU (see below)**|
|Video: **RSP renderer** draws the picture (mode 1) into a 320x240 16-bit framebuffer; mode 7 lines stay on the CPU|Done. Matches the C reference on 2,037 frames in an N64 emulator and 391,000 frames on the test rigs|
|Audio: SPC sound driver + DSP at 32 kHz into libdragon audio|Done, verified|
|Controller 1 input mapping|Done|
|Saves: cartridge SRAM (EverDrive save type `sram256k`)|Written, untested on hardware|
|PAL: 60 Hz game logic stepped on a fixed clock (1-2 steps per 50 Hz frame)|Written, untested on hardware|
|**Speed**|**RSP renderer done; real-hardware FPS not measured yet (see below)**|

The picture is now drawn by the N64's RSP (vector coprocessor) while the CPU
runs the next frame. Please send the numbers from the top border overlay.

## Build

You need your own Super Metroid (Japan/USA) ROM, SHA-1
`da957f0d63d14cb441d215462904c4fa8519c613`, no copier header.

1. Copy it to `filesystem/sm.smc`. It is already there if Claude copied it in.
2. In the shell you use for libdragon, with `N64\\\\\\\_INST` set:

make



3. The output is `sm64.z64`. It contains the ROM, so never share it.

It needs the **Expansion Pak**. The 3 MB ROM is mirrored up to 4 MB and kept in
RAM; with the RSP renderer's two capture buffers (550 KB) and three 16-bit
framebuffers, total use is about 7.7 MB of the 8 MB. Without the pak, a blue
error screen appears.

Debug output (timing per logic step, TV type, SRAM detect) goes through `debugf`:
use UNFLoader `-d` on the EverDrive, or the ares console.

### If your SDK is older

`sram\\\\\\\_init`, `sram\\\\\\\_detect`, `sram\\\\\\\_read` and `sram\\\\\\\_write` are libdragon *preview* APIs
from 2025-2026. If they're missing, remove the four `sram\\\\\\\_` calls in
`src/platform/n64/n64\\\\\\\_main.c`. The game still runs, but saves won't persist.

## Controls (controller 1)

|N64|SNES / Super Metroid|
|-|-|
|A|A: jump|
|B|X: shoot|
|C-down|B: dash|
|C-left|Y: item cancel|
|Z or C-right|Select: item select|
|L / R|L / R: aim|
|Start|Start|
|D-pad or stick|D-pad|
|C-up|performance overlay on / off (not sent to the game)|
|L + C-up|2.5D look on / off (see below; on by default)|
|R + C-up|50 / 60 Hz picture on a PAL console (see below); press A within 10 s to keep 60 Hz|
|L + R + C-up|low-poly 2.5D characters on / off (see below; on by default)|

## What I changed from upstream

* `src/platform/n64/n64\\\\\\\_main.c` is new and replaces `main.c`: video, input,
audio, SRAM, PAL timing and the error screen.
* `src/platform/n64/n64\\\\\\\_glue.c` is new: accessors into the emulated SNES state.
* `snes/ppu.c`: pixels are packed as N64 RGBA (`PIX` macro).
* `snes/cart.c` and `snes/snes\\\\\\\_other.c`: the ROM is loaded in place, with no extra
4 MB copies.
* `sm\\\\\\\_cpu\\\\\\\_infra.c`: runs only the C game code (`RM\\\\\\\_MINE`). The PC build also runs
the original 65816 code side by side to compare, which is far too slow here.
* `sm\\\\\\\_rtl.c`: SRAM goes to the platform layer.
* `config.h`: SDL is compiled out.
* `Makefile`: compiles everything as C++ (game code with `SM\\\\\\\_LE\\\\\\\_TYPES`), and
renames `cart\\\\\\\_init`, `dma\\\\\\\_read` and `dma\\\\\\\_write` because libdragon uses the same names.
* `types.h`: the `LeInt` byte-order-safe integer types, and `memcpy` -> `memmove` (N64 only).
* `sm\\\\\\\_cpu\\\\\\\_infra.c`: the PC-only comparison snapshots (600 KB) are allocated on
demand, so they cost nothing on N64.
* Small C++-compatibility edits across `sm\\\\\\\_\\\\\\\*.c` (a `+` before some values in
`?:` expressions, split declarations before `goto` labels). See "How the
byte-order fix works".

Every change is behind `#ifdef N64`, so the files still build for PC.
`n64\\\\\\\_changes.patch` lists every change to the upstream files.

## How the byte-order fix works

snesrev/sm was written for little-endian PCs. The N64's CPU is big-endian and
also faults on unaligned 16- and 32-bit loads. The game code reads SNES RAM, ROM
and its own data tables through thousands of `uint16`/`uint32` pointers and
struct overlays.

Instead of rewriting each of those, the game code is now compiled as **C++**,
and its integer types `uint16`, `int16`, `uint32` and `int32` are small wrapper
types (`LeInt` in `src/types.h`). They always store bytes in SNES
(little-endian) order with 1-byte alignment, and convert to normal integers in
expressions. So every access in the game code means exactly what it meant on a PC,
on any CPU. The SNES hardware code (`src/snes/`) keeps native types. All of this
is only active in the N64 build (`SM\\\\\\\_LE\\\\\\\_TYPES`); the PC build is unchanged C.

Other fixes found by testing:

* `snes/ppu.c` `ClearBackdrop` did 64-bit stores into a 2-byte-aligned buffer,
which crashes on MIPS.
* The game `memcpy`s between overlapping RAM regions when unpacking room data.
That only worked on PC because glibc's `memcpy` happens to behave like
`memmove`. On N64 it is now `memmove`, which is also what the SNES does.
* About 70 small source edits so the C code is also valid C++, all still valid C.

## How it was verified

`tools/be\\\\\\\_rig/` runs the game code on an emulated big-endian MIPS VR4300 (the
Unicorn engine, with the same compiler and flags as the ROM). It traps unaligned
accesses exactly like the N64 does and compares every frame against the original
PC build. Each frame it fingerprints all of game RAM, save RAM, VRAM, palette,
sprite table, the rendered picture and the audio output.

|Test run|Frames|Result|
|-|-|-|
|Title, file select, intro (scripted buttons)|3,000|identical|
|Attract-mode demos: Crateria, Brinstar, Norfair, Maridia, and more|20,000|identical|
|New game: intro, all of Ceres Station, doors, enemies, pause, Ridley fight|30,000|identical|
|Demos again, built with the ROM's exact compiler flags|6,000|identical|

Not yet covered: Zebes beyond the demos, most bosses, saving and loading. The
wrapper approach fixes the game code generally, not path by path, so remaining
risk is mainly in native-typed code. The rig can test any new route: add
button events to `tools/be\\\\\\\_rig/play\\\\\\\_script.h`.

## Speed and the RSP renderer

The rig counts executed MIPS instructions. The N64's CPU manages roughly
1-1.5 million per 1/60 s (cache and memory stalls make the real figure lower).

|Version|CPU instructions per game frame (gameplay)|
|-|-|
|First byte-order build|\~9.9M + 0.9M audio|
|Per-scanline clock loop + aligned line clears|\~7.8M + 0.9M audio|
|**RSP renderer** (CPU only records what each line needs)|**\~1.4M + 0.9M audio** (max 1.6M + audio)|

### How it works

1. While the game runs a frame, `ppu\\\\\\\_runLine` no longer draws. `src/snes/ppu\\\\\\\_rsp.c`
records each line's PPU state (64 bytes: scroll, tilemap rows, windows,
colour math, brightness), the visible sprite rows (8 bytes each, already
flipped, with the 32-sprite / 34-tile limits of the real SNES), palette
snapshots, and the 33 tiles each background layer needs for its current tile
row (one block per 8 lines). At the end of the frame it copies VRAM.
2. The RSP microcode `src/platform/n64/rsp\\\\\\\_ppu.S` draws mode 1 (BG1/BG2 4bpp,
BG3 2bpp, sprites, both windows, colour math, half/add/subtract, brightness)
eight pixels per vector instruction and DMAs each finished line into the
framebuffer as RGBA5551.
3. The CPU and RSP overlap: the RSP draws frame N while the CPU runs frame N+1
(two capture buffers, three framebuffers).
4. Lines the RSP can't draw yet are drawn by the CPU as before: mode 7 (title
screen, intro, Ceres), mosaic, and a few rare cases. The game decides per line.

RSP time per shown frame (cycle model fed by the accurate RSP interpreter;
the RSP runs at 62.5 MHz, about 1.04M cycles per 1/60 s):

|Job|Average|Worst sampled|
|-|-|-|
|Picture (`rsp\\\\\\\_ppu.S`), 336 sampled frames|8.4 ms|17.3 ms|
|Sound (`rsp\\\\\\\_dsp.S`), per logic step|4.1 ms|5.4 ms|

Sound moved to the RSP too: the CPU runs the SPC driver and logs DSP register
writes; the RSP replays them and renders the samples (bit-exact with dsp.c).
The CPU now needs about 0.63M instructions per logic step.

### Verification

* C reference renderer vs the original PPU code, on the big-endian rig: 0
different lines over 50,000 frames (6.5M lines), and game state unchanged.
* RSP microcode vs C reference (`tools/rsp\\\\\\\_test`): 0 different lines over
336 sampled frames (55,000 lines).
* Whole ROM in a headless N64 emulator (`tools/n64emu`, with a test build that
compares each shown frame against the C reference): 0 different lines over
2,037 frames, through the boot logo, title, intro and demos. This found one
bug the other tests couldn't: the second capture buffer wasn't 8-byte
aligned for RSP DMA. Fixed.

### Overlay

Top border, C-up toggles it, updated every second. It also goes to `debugf`
(UNFLoader `-d`, ares console):

`FPS<shown/s> GAME<logic steps/s> L<logic ms> C<capture ms> W<wait ms> S<sound ms> P<picture ms>`

* GAME should read 60 (the game's own speed). FPS is pictures per second (at most 50 on PAL, 60 in 60 Hz mode).
* L: game logic per step. C: extra CPU time to capture the picture (once per shown frame).
* W: CPU time spent waiting for the RSP. If W is above 0, the RSP is the limit.
* S / P: RSP time for the sound and the picture of each shown frame.
* The debug log (`debugf`) also prints A, the audio buffer time, and G / LN:
microseconds per step in the game code (including its NMI handler) and in
the scanline loop (HDMA, IRQs, capture), MK (us per frame spent finding
repeated lines) and CP (share of lines copied from the previous frame).

Measured in the accurate ares emulator (PAL, Expansion Pak): title and most
gameplay 40-50 FPS, the intro 37-50, Ceres 34-50, the busiest attract-demo
rooms 33-43. Real hardware may differ; the overlay shows where the time goes.

Logic step (L) in ares, before / after the logic pass: busy demo rooms
4.1 -> 2.8 ms, quieter rooms 1.8 -> 1.3 ms, intro 1.3 -> 0.8 ms. What changed:
VRAM / OAM / CGRAM DMA copy whole words (two per 32-bit load on big-endian),
PPU register writes skip the generic bus decoder, the scanline loop runs each
line in one call and HDMA only visits live channels, and the functions called
hundreds of times per frame (register writes, DMA, HDMA) are placed together
at the start of the code (`SNES\\\\\\\_HOT`) so they stop evicting each other from
the 16 KB instruction cache.

RSP waiting time (W), before / after the RSP pass, busy attract-demo rooms:
W 4.5-12 ms -> 0-5 ms, FPS 42-45 -> 47-51. Two changes:

* 16-colour BG tiles are stored by the CPU as one nibble per pixel (converted
once when a tile block column is filled), so the RSP's BG loop no longer
decodes bit planes: about 21 instead of 30 cycles per two tiles.
* Repeated lines are copied, not drawn. A line whose record, palette, sprite
rows and BG data (tilemap entry and tile-page write generation of each of
its 34 columns, or the mode 7 row's key) all equal the previous frame's gets
kLR\_Copy, and the RSP copies its 512 bytes from the previous framebuffer.
The status bar and anything that doesn't move qualify; in normal gameplay
about half of all lines. A fingerprint taken at capture time keeps the
check cheap when lines change. The rig verifies every copied line against
the reference renderer (0 wrong copies over 50,000 frames).

### 2.5D look (bevels and shadows)

(Not the same thing as the low-poly 2.5D characters below. This older
background effect is currently left out of the build, `FX25D 0` in
`n64_main.c`, because it glitched when the screen changes.)

On by default during gameplay; L + C-up switches it off and on. The original
art is kept. After the picture, a third RSP microcode, `rsp\\\\\\\_fx.S`, changes:

* Level blocks (BG1): a 1-pixel bevel. Edge pixels get half their colour; on
the top and left edges also +12 per channel, so blocks look lit from the
top left.
* Everything behind the blocks, Samus, enemies and items (BG2, the BG3 layer,
the backdrop): a soft drop shadow, 22% darker for each of the three pixels
up-left of it, (x-1, y-1), (x-2, y-2), (x-3, y-3), that belong to a block
or a sprite (so up to 66% darker, fading out at the shadow's edge).

How: while drawing, `rsp\\\\\\\_ppu.S` also writes each line's 256 z values (which
layer won each pixel) to a mask image in RDRAM. In rooms where the main screen
only shows the BG3 fog / water layer and the level is on the sub screen, it
takes the sub screen's z values there. `rsp\\\\\\\_fx.S` keeps the last five mask
rows (y-3 .. y+1) in DMEM, converted to one word per pixel so the pixel loop
needs no compares (31 vector instructions per 8 pixels, two groups
interleaved and software-pipelined so the vector unit doesn't stall), and
rewrites each framebuffer line. It runs after the picture and before the
frame is shown, as a third stage chained from the SP interrupt.

Lines it leaves alone: the HUD (top 32 rows), black and mode 7 lines,
anything outside gameplay (title, menus, map, intro). A line copied from the
previous frame (kLR\_Copy, see above) already has the effect, so the CPU only
lets it be copied when rows y-3 .. y+1 are unchanged too, and switching the
effect re-draws everything once.

Cost: about 2,100 RSP cycles per processed line (6.4 ms if all 192 lines
need it; copied lines are skipped). The overlay's P includes it; the debug
log's FX field is its share (microseconds per frame). Measured in ares (PAL,
Expansion Pak), same 30,000-frame autoplay run with and without the effect:

|Part of the run|FPS with effect (min)|FPS without (min)|FX time|
|-|-|-|-|
|Ceres|49.2 (44)|50.1 (49)|3.4 ms|
|Zebes, first rooms|49.3 (45)|50.1 (47)|3.1 ms|
|Busy rooms at the end of the script|45.6 (35)|50.0 (38)|4.7 ms|

Verification: the microcode against a C model (`rsptest/fx/fxhost.c`,
random masks and pixels, 16 seeds, every case incl. gaps between lines):
0 different pixels. The mask rows against the C reference renderer's z
values on 1,000 captured frames (224,000 lines): 0 different. The picture
itself is unchanged (RSP vs C reference: 0 different lines; the in-emulator
self-check build switches the effect off and still reports 0 bad lines).

### 60 Hz mode

A PAL console sends 50 pictures a second to the TV. R + C-up switches it to
60 Hz (libdragon's PAL60 timing: the PAL colour signal at the NTSC frame
rate), which most CRTs and many TVs and upscalers accept. Because some don't,
the switch is on trial: the top border shows "60 Hz: press A to keep it" with
a countdown, and without A it goes back to 50 Hz after 10 seconds. R + C-up
again returns to 50 Hz. NTSC consoles are 60 Hz already. To start in 60 Hz
every time, build with `make TEST\\\\\\\_FLAGS=-DFORCE\\\\\\\_PAL60` (no confirmation).

The game always runs at its own 60 steps per second; at 60 Hz that is one
step per picture, so motion is smoother. A picture now has 16.7 ms instead
of 20 ms, so this pass also made the CPU and RSP work per picture smaller.
Measured in ares (PAL console in 60 Hz mode, Expansion Pak), the same
36,000-step autoplay run, average FPS:

|Part of the run|Before, effect off|After, effect off|Before, effect on|After, effect on|
|-|-|-|-|-|
|Boot, title, intro|56.7|58.2|57.5|58.2|
|Ceres (mode 7 rooms)|47.2|59.4|48.9|58.7|
|Zebes|57.4|58.9|49.5|51.7|

With the 2.5D look on, busy Zebes rooms are limited by the RSP (the effect
costs 3-5 ms there), so for a steady 60 turn it off with L + C-up. The
Ceres escape cutscene is drawn by the CPU (a mode the RSP renderer doesn't
do) and runs slowly in both modes, as before.

What changed:

* Mode 7 rows on the RSP (`rsp\\\\\\\_m7.S`, new). Ceres' rooms are mode 7 without
rotation or scaling; the CPU spent \~3,600 cycles per line building their
BG1 rows (7 ms a frame). It now only lists those lines (plane row and first
column); a new microcode, run before the picture, fetches the 33 tiles of
each tile row once by DMA and builds the rows (\~800 RSP cycles per line).
Rotated or scaled mode 7 lines are still built by the CPU.
* Capture: a line whose PPU registers are the same as the previous line's
(no write to them in between, apart from scroll registers) starts from a
copy of that line's record and only works out what depends on the line:
tilemap rows, tile blocks, sprites. That is most lines. Per-sprite data
is worked out once per OAM change instead of on every line, a layer's tile
block lookup is skipped while its inputs don't change, and the per-line
capture code sits with the other per-scanline code in the instruction
cache (the CPU drawing path is a separate function).
* The whole framebuffer is no longer flushed from the CPU's cache every
frame, only when the CPU drew a line into it.
* libdragon's RSP interrupt handler (it reads RSP memory after every job to
look for RSP asserts, which these microcodes don't use) is not installed.
* 2.5D pass: the framebuffer line is processed in halves, so the next line's
pixels load while the second half is worked on, and consecutive lines
reuse the previous line's mask rows without searching (about 8% faster).

Verified like the earlier passes: the capture vs the original renderer on
50,000 frames (0 different lines, emulation unchanged, 0 wrong copies), the
big-endian rig (3,300 frames match), the in-emulator self-check (0 bad
lines), `rsp\\\\\\\_m7` against its C model (16 random cases, 0 different rows)
and, in the emulator, every row `rsp\\\\\\\_m7` built in the run above against the
same window of the plane worked out by the CPU (490,000 rows, 0 different).

### Low-poly 3D characters

**This is a low-polygon 2.5D effect, not full 3D.** The game still plays on
its original flat 2D plane with the original camera, rooms and backgrounds.
Only the characters gain depth: each one is built from its own 2D sprite as a
stack of textured layers with thickness, turned slightly and seen a little
from above, so it looks like a chunky low-poly model standing in the 2D world.

On by default during gameplay; L + R + C-up switches it off and on. The
player, the enemies and the bosses are drawn with depth over the normal
picture, by the RDP, while the backgrounds, the HUD, items, shots and effects
stay 2D.

* **The player is the game's own sprite in 3D.** Each frame, the sprites the
game writes for the player (`Samus_Draw`, `Samus_ArmCannon_Draw` in `sm_90.c`)
are taken back out of OAM and handed to `platform/n64/lowpoly.c`, with the
tiles the NMI is about to copy for that animation frame (straight from your
ROM) and the palette. There they are decoded into pixels, and each pixel gets
a thickness: 1 at the outline, up to 4 further in, so the figure is rounded.
The figure is then drawn as a stack of six layers, back to front, each one
the sprite's own pixels as a single RDP texture rectangle, shifted a little
along the view (turned towards the side the player faces, seen slightly from
above). Outline pixels are in every layer; inner pixels also reach the front
ones. The back layers show as the figure's sides (62-72% shade), the front
ones as its face (86-100%, brighter towards the middle). Every pose, suit,
palette effect and the arm cannon therefore keep their original look. No art
is stored in the code: it all comes from your ROM. Cost: 6 texture rectangles
and about 62 RDP command words per figure (12 / 123 for one taller than a
texture band, such as the player, 64 x 72 at most), no triangles. The texture
goes through the colour filter.
* **Kestrel**, the original block-model hero (purple full-face helmet, green
armor over a purple suit, wrist blaster, 33 blocks, about 140 triangles), is
still in: build with `make TEST_FLAGS=-DLP_HERO_KESTREL=1`.
* **The opponents are their own sprites in 3D too**, built the same way.
`WriteEnemyOams` (`sm_a0.c`) lets each opponent's sprites be written to OAM as
usual, then hands them over (`LowPoly_BeginEnemy` / `NoteEnemyOam` /
`EndEnemy`) and takes them out. Their tiles come from VRAM, and hit flashes
and freezing come through their palette, as in 2D. Each is turned by where it
is on the screen: the side towards the middle shows. Opponents are drawn
before the player, the front-most last.
* **Which ones:** enemies and bosses drawn only from sprites, up to 128 x 112
pixels and 4 sprite palettes. Harmless creatures, solid objects (doors,
platforms), intangible ones, and bosses or parts drawn on a background layer
stay 2D. So does anything the frame has no room left for (see the budget).
* **Budget per frame:** 16,384 texels of texture (the player's 64 x 72 kept
free). Textures are 8-bit colour indices (CI8): per pixel its colour (one of
the object's up to 4 palettes) and, in the top two bits, its thickness class.
The TLUT (upper half of TMEM) holds the object's 64 colours four times, once
per class; before each nearer layer, a quarter of it is overwritten with
transparent entries, so that layer only shows the pixels thick enough to
reach it (alpha compare drops the rest). The two frames' pools take 32 KB.
* **Memory:** for the first 12 seconds the bottom border shows how much RAM is
left after the 3D buffers ("Low-poly 3D on, N KB RAM free"). If they don't fit,
the game doesn't stop: the look stays off (everything 2D) and the border says
"Low-poly 3D off: not enough memory". (An earlier build with 16-bit textures
needed 32 KB more and stopped at start-up with "lowpoly: out of memory".) An opponent whose texture doesn't fit any
more is refused while the game is still drawing, so it keeps its 2D sprites:
nothing disappears. A crowded test (the player and 12 Metroids) takes 7 of
the 12 (the texture budget): 48 texture rectangles, about 500 command words.
* **Bosses.** Kraid, Phantoon, Crocomire and Mother Brain's body are drawn
on BG2, not with sprites (the game turns X-ray off for these boss IDs, 3 / 7
/ 6 / 10, for that reason). Their BG2 pixels go into a canvas kept between
frames (BG2 tile coordinates, the box of tiles that have pixels). It is built
once and then only where a tile changes (its tilemap entry, or a write to its
VRAM page, from the renderer's page write counts), with the thickness worked
out again just around the change. Each frame the canvas is drawn as layers
like the sprites, at BG2's scroll (read from the frame's line records), with
its outline's front exactly over the 2D boss, so nothing of the 2D picture
shows through. What the 2D picture has in front of the boss (BG1 tiles of at
least its priority, sprites of priority 3, or 2 over its priority-0 tiles,
such as shots) is drawn again on top, 8 x 8 at a time. While BG2's scroll
differs between lines (Phantoon's wave, Crocomire melting), the boss stays
2D. A frame captured just before the canvas changed draws the 2D boss.
Bosses drawn from sprites that are too big for the opponents' budget (Ridley,
Draygon...) get a big pool (2 x 32 KB, up to 256 x 224). Sprites a boss
writes to OAM itself (Ridley's tail, `sub_A6DC13` in `sm_a6.c`; Mother Brain's
neck and other draw hooks) become 3D objects too. The canvas and the big pool
share 64 KB, taken at start-up only if at least 48 KB of RAM stays free after
it; otherwise the bottom border adds ", bosses 2D". The PC test builds a
stand-in boss on BG2 from ROM tiles (`boss.ppm`, fourth argument), with a BG1
floor and a sprite in front, and checks that a canvas updated tile by tile
equals one built from scratch and that a wave leaves it 2D.
* **In front of the player.** The 3D player is drawn over the finished
picture, so whatever the 2D picture had in front of her is drawn again over
her, 8 x 8 at a time (`LpHeroPieces`): BG1 / BG2 tiles whose priority wins
over her sprites' (priority-1 tiles over priority-2 sprites; every BG1 / BG2
tile over priority 0-1), and the sprites written to OAM before hers (lower
entries are in front), such as the gunship when she gets out of it on landing.
`sm_90.c` reports her first OAM entry (`LowPoly_NoteHeroOamFirst`).
* **Colour window per pixel on the RSP (Ceres explosion, power bombs).** Lines
where colour math happens only inside (or only outside) a colour window that
changes along the line used to be drawn by the CPU's generic renderer: the
Ceres explosion (a power-bomb window over mode 7) ran at about 7 FPS. The RSP
now does them: `ppu_rsp.c` marks them `kCB_WinMath` (+ `kCB_WinMathOut`), and
Compose in `rsp_ppu.S` first clears the math bit of the main pixels where the
window rules math out, with the existing window code, then composes as usual.
Lines whose clipping to black uses such a window still go to the CPU. To make
room in IMEM (it was full), the z-mask copy for the 2.5D pass now does one
group of 8 pixels per pass instead of four. Not run on the RSP test bench (its
emulator isn't set up on this machine).
* **Security eye beams (fix in `rsp_ppu.S`).** The loop that draws a layer
through a window one tile at a time (BlkWin) fell through into the mode 7
code after its last tile, which drew the row's tile bytes again as mode 7
pixels. The security eyes (and Mother Brain's room) draw a dark BG3 on the
sub screen with the beam cut out by a window, so that garbage showed as
yellow crosshatch patches and bands away from the beam. It now jumps to the
end (one instruction: the last free one in IMEM).
* **HIWORD of native locals on big-endian (`sm_88.c`).** HIWORD / LOWORD /
HIBYTE / LOBYTE (`types.h`) address a variable's bytes in memory as
little-endian. That's right for anything overlaying SNES memory (LeInt types,
and ROM / RAM structs even where a field is declared `unsigned int`), but
wrong for a native local computed on the N64, whose high half comes first.
The security eye's beam built its HDMA table with `HIWORD` of such a local,
so two bytes came out wrong (a block count and the end marker), and the beam
broke off partway down the screen. A scan of all game code for these macros
on native locals found 8 uses, all in `sm_88.c` (the eye beam, another HDMA
table set up the same way, and an intro cinematic). They're now plain shifts.
(Making the macros pick by type broke the ROM structs: a crash firing beams.)
* **Window mask on the RSP (`WinMask`, `rsp_ppu.S`).** The per-pixel window
test ends with VGE against 0 on a value that is negative inside the window.
VGE settles "equal" with the carry flags of the last VSUBC, and left of a
window that value is often exactly 0, so those pixels came out masked:
diagonal holes in windowed layers left of a window (the security eye's beam
showed yellow stripes through BG3). It now compares against -1. Found with
the test switch: the beam's table and the line records were right, only the
RSP's picture wasn't.
* **Black sub screen skipped (`SubBlack`, `ppu_rsp.c`).** Most rooms keep BG3
on the sub screen, added to everything, as the cleared FX tilemap whose
palettes are black: the RSP drew that layer on every line and ran the colour
math loop to add zero (the security eye's room took about 17 ms of RSP time
per picture before its beam even started). A line whose sub screen holds
only BG3, whose BG3 row (both 32-column screens) only uses black palettes,
whose fixed colour is black and whose math only adds, is now captured without
the sub screen and without colour math: the same pixels, the simple compose
path. Lines with a visible FX layer (water, lava, fog), or a coloured fixed
colour (the eye's beam, power bombs), keep the full path. Not measured yet.
* **Test switch (removed).** While hunting the beam bugs, L + R + C-down
cycled between the CPU's original renderer and the RSP, with a check of the
beam's HDMA table and line records in the borders. It's out of the build now.
Comparing the CPU's original renderer with the RSP on the same frame is the
quickest way to tell a renderer bug from a game-logic one.
* **Speed pass (layers instead of boxes).** The first version split each figure
into rectangles of equal thickness and drew each as a textured box: 300-500
triangles for the player, about 1,700 in a crowded frame, each set up by the
CPU in floating point and written through two 2 KB command buffers, so the CPU
also waited for the RDP. That ran at 30-40 FPS. The layers need no triangle
setup and about 1/40 of the commands: 54-60 FPS in your test, depending on
the room and the number of enemies. The texture decoding now works a tile
row (8 pixels) at a time and the thickness pass runs on a grid with a border
(no bounds checks per pixel). The old box renderer is in
`_backup_before_speed/lowpoly.c`.
* **Shots and blinking at under 60 FPS.** The game shows beam shots every
other frame (half the shot slots on even frames, half on odd), and Samus
blinks every frame after a hit. When the N64 runs two game steps per picture,
only every second step is shown, so a shot could be invisible for its whole
flight. On the N64, shots are drawn every frame (`DrawPlayerExplosions2`,
`sm_93.c`) and the blink changes every two steps (`sm_90.c`).
* **The old enemy models** (below) are still in: `make TEST_FLAGS=-DLP_ENEMY_SHAPES=1`.
* **Checking on a PC:** `tools/lowpoly/sprite3d_test.c` runs the same code on
graphics taken from your ROM: seven player poses (`hero.ppm`), nine enemies
from different areas (`enemies.ppm`; each column is the 2D sprite, then 3D on
the screen's left and right), and the crowded screen (rectangle and command
counts printed). It draws the texture rectangles and TLUT loads the way the
RDP would.
The build line is at the top of the file. The images contain parts of your
ROM, so don't share them.

With `LP_ENEMY_SHAPES=1`, the opponents are the earlier original designs from
the game's design doc instead:
* **Burrowdrake** replaces the enemies that rise up and spit fireballs (the
lava creatures): a bipedal dragon with short wings and a scale texture,
green in the first area and crimson elsewhere, always facing the player.
* Every other enemy and boss drawn from sprites becomes a solid-colour
faceted shape (octahedron, icosahedron, bipyramid, cube, crystal; a spiked
ball for big ones) coloured from its palette. Harmless, solid (doors,
platforms) and intangible objects keep their sprites, and bosses drawn on a
background layer stay as they are.

How: the game's draw routines report the player and each enemy instead of
writing their sprites (`sm\\\\\\\_90.c`, `sm\\\\\\\_a0.c`). After the RSP has finished a
frame's picture, `platform/n64/lowpoly.c` transforms the models (one matrix
per limb, block data prepared once), sorts their blocks back to front and
writes an RDP command list in two small buffers used in turn, while the RSP
already works on the next frame. Rows a model covered are never copied into
the next frame. The models themselves are generated tables
(`lowpoly\\\\\\\_models.h`). To build without the feature: `make TEST\\\\\\\_FLAGS=-DLOWPOLY=0`.

To make room for the models in memory, a frame now holds up to 3,072 sprite
slivers instead of 4,096 (about 2,400 was the most seen; a frame with more
falls back to the CPU drawing the rest).

Cost, measured in ares like the table above (60 Hz, 2.5D look on, same
36,000-step run). "Models" is the CPU and RDP time per picture for the
models; the CPU builds them while the RSP draws the next picture.

|Part of the run|Average FPS before|Average FPS with models|Models, ms per picture|
|-|-|-|-|
|Boot, title, intro|58.2|58.0|0 (no gameplay)|
|Ceres (mode 7 rooms)|58.7|57.4|1.0|
|Zebes|51.7|49.9|3.9|

Getting there: the first version took 5.9 ms for Kestrel alone. Each limb's
transform is now one matrix and the blocks are prepared once, triangle setup
for flat triangles skips the texture and depth maths, faces seen almost
edge-on (under 1.5 square pixels) are skipped, and the models were trimmed to
33 blocks each (about 140 triangles for Kestrel on screen).

Checked in ares with the angrylion reference RDP linked in (the headless
runner draws with it when `ANGRYLION=1`): Kestrel in all poses through the
autoplay run, and, with `TEST\\\\\\\_FLAGS=-DLP\\\\\\\_TEST\\\\\\\_FAKE` (adds two Burrowdrakes and
two shapes beside the player, since the autoplay route meets no regular
enemies), both Burrowdrake colours and the shapes.

### Speed pass: rain and fog rooms

Two places you reported as slow at 60 Hz: the landing site in the rain
(Crateria, right after leaving the ship) and the Morph Ball room in Brinstar
(the security eye's searchlight). Both have a BG3 layer (rain, fog) added to
the whole picture with colour math, and it changes every frame, so no line can
be copied from the previous frame and the RSP draws all 224 lines.

Measured in ares (NTSC, Expansion Pak), about a minute in each room, loading
your save (slot A) and standing / walking (`TEST\\\\\\\_FLAGS="-DAUTOPLAY -DAUTOPLAY\\\\\\\_FILE"`,
see Testing below):

|Room|FPS before|FPS after|RSP sound + picture per frame, before / after|
|-|-|-|-|
|Landing site, rain|34.3|50.7|5.7 + 23.2 ms / 2.9 + 15.8 ms|
|Morph Ball room|44.4|58.4 (59-61 when the picture doesn't scroll)|5.4 + 15.3 ms / 3.0 + 10.5 ms|

What changed:

* **Automatic 2.5D look.** The effect costs 3-6 ms of RSP time per frame in
rooms like these. If the RSP is busy almost all the time with it (93% or
more over half a second) and pictures come slower than the TV rate, it is
switched off for that room; the room is remembered for the rest of the
session. In lighter rooms it stays on. L + C-up still switches it by hand
(pressing it in a room where it went off automatically turns it back on there).
* **Sound microcode (`rsp\\\\\\\_dsp.S`), about 26% less RSP time.** The per-sample
loop is rescheduled so the scalar unit (echo buffer, FIR ring, sample
gathers) and the vector unit (interpolation, envelopes, FIR, mixing) work in
parallel, and BRR decoding picks its filter code once per block and clamps
with one test instead of two branches. Same output: 0 different samples
against `dsp.c` over 59,000 steps (attract demos, new game, both rooms).
* **Picture microcode (`rsp\\\\\\\_ppu.S`), 2-4% less.** The next line's first BG
layer is fetched while the current line is composed, a line doesn't wait for
the previous line's output to leave DMEM unless it needs that buffer, and
lines without sprites skip clearing the sprite line.
* **CPU capture.** Tile blocks are found through a small hash table, and when
VRAM pages they use were written only the columns built from those pages are
rechecked (the rain animates its tiles every frame). Sprites are worked out
for all lines of a frame at once after the frame, while their data stays in
the CPU's cache, instead of between the scanlines.
* The 2.5D mask image (112 KB) is allocated before the other buffers, so it
always gets one piece of memory.

The landing site is still limited by the RSP (about 16 ms for the picture
alone): every line does colour math with the BG3 rain on the sub screen. A
steady 60 there would need a different way of drawing that layer.

Verification: the capture against the original renderer on the rigs (attract
demos 20,000 frames, new game 30,000, landing site 7,000, Morph Ball room
7,000): 0 different lines, emulation unchanged. Both microcodes on the cxd4
RSP against the C reference: 0 bad lines over 479 captured frames, mask image
included; sound as above.

### Testing in the two rooms

* `make TEST\\\\\\\_FLAGS="-DAUTOPLAY -DAUTOPLAY\\\\\\\_FILE"`: loads save slot A at
power-on (`src/platform/n64/autoplay\\\\\\\_file.h`) and walks left and right.
* The headless ares runner takes a save file: `SRAM=<file.ram>`. Slot A's
load station decides the room (area 0 station 0: landing site; area 1
station 9: Morph Ball room, the elevator from Crateria).
* The rig (`tools/be\\\\\\\_rig`) has the same as script 3 (`RIG\\\\\\\_SRAM=<file>`).
* `TEST\\\\\\\_FLAGS=-DCAPSTAT` prints where the capture time goes (tile blocks,
sprites, HDMA) once a second.

### What's left

* Real-hardware numbers. The figures above come from the ares emulator and
the RSP cycle model; a real console may differ. The overlay shows where the
time goes.
* A steady 60 Hz in rooms where every line does colour math with a changing
BG3 layer (the rainy landing site): the picture alone takes about 16 ms of
RSP time there. The 2.5D look switches itself off in such rooms.
* Rotated / scaled mode 7 (title zoom, intro) is still built by the CPU.
* Half-colour math and brightness fades with subtraction use a slower general
path on the RSP (not seen in the sampled frames).

