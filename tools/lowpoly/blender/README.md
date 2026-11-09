# Blender add-on: your own low-poly hero

`lowpoly_blocks.py` lets you build the hero (the 3D character that replaces the
player's sprite) in Blender out of boxes, and writes it straight into this
project. Then you run `make` as usual.

## Install (once)

1. Install Blender (free, blender.org), version 3.6 or newer.
2. Edit > Preferences > Add-ons > Install from Disk (in older versions: Install...),
   pick `tools/lowpoly/blender/lowpoly_blocks.py`, and tick
   **Super Metroid 64 Low-Poly Blocks**.
3. In the 3D view press **N**. The sidebar has a tab **SM64 Blocks**.
4. Set **Project folder** to this folder (`sm_n64_latest_source`).

## Workflow

1. **Import Model** > `kKestrel`. The current hero appears as 33 boxes in a
   collection, plus markers for the joints. Press numpad 1 (front view): that
   is the in-game side view, character facing right.
2. Edit:
   * Move (G), scale (S) and rotate about Y (R, Y) boxes as usual. Don't
     rotate about X or Z, and don't edit vertices: a block is always a box.
   * In the panel, per block: **Limb** (which body part it moves with),
     **Material** (lit, lit + texture, glow), **Colour**, **Taper** (size of
     the top face), **Back taper** (size of the back end).
   * **Add Block** adds a box at the 3D cursor (copying the selected block's
     settings). **Make Block** turns any mesh, such as a cube you added
     yourself, into a block (the box around it). **Mirror to Far Side** copies
     blocks to the other side and swaps near / far leg and arm.
3. **Check Model** lists anything the game can't show well.
4. **Export as Hero** writes `tools/lowpoly/hero.json` and regenerates
   `src/platform/n64/lowpoly_models.h`. Then run `make` and test the ROM.
5. **Restore Kestrel** deletes `hero.json` and puts the original back.

You can also start from scratch: make a new collection, add blocks, export.

## Rules the game needs

| | |
|---|---|
| Units | 1 Blender unit = 1 model unit. Kestrel is 49 tall; about 1.16 units = 1 SNES pixel |
| Ground | Feet at Z = 0 |
| Facing | +X. Near side (toward the player) is -Y |
| Joints | Legs (limbs 1, 2) swing about Z = 24; arms (3, 4) about Z = 33.5; tail (6) about Z = 14.4; the spin jump rolls everything about Z = 26 |
| Near arm | Limb 3 points where shots go: build it hanging straight down from the shoulder |
| Blocks | At most 48 (Kestrel uses 33). Each block is 12 triangles before hidden faces are removed |
| Size | Up to about 72 units above the feet and 9 below; within about 42 of the spin centre. Bigger parts get cut off |

The morph ball is a separate shape and isn't affected. The Burrowdrake can't be
replaced yet; only the hero.

## Command line

Without Blender: `python3 tools/lowpoly/emit.py src/platform/n64/lowpoly_models.h`
uses `tools/lowpoly/hero.json` when it exists; `--no-hero` ignores it,
`--hero FILE.json` uses another file.
