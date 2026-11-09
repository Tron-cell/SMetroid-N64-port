// Accessors into the emulated SNES state for n64_main.c, which can't include
// the game headers directly (their type names clash with libdragon's).
#include "../../snes/snes.h"
#include "../../snes/ppu.h"

extern Snes *g_snes;

void Snes_N64_GetPpus(Ppu **a, Ppu **b) {
  *a = g_snes->snes_ppu;
  *b = g_snes->my_ppu;
}

void Snes_N64_SetDisableRender(bool v) {
  g_snes->disableRender = v;
}

// The PPU that ppu_runLine() currently draws with.
Ppu *Snes_N64_ActivePpu(void) {
  return g_snes->ppu;
}

const uint16_t *Snes_N64_Vram(void) {   // (lowpoly.c reads the sprite tiles)
  return g_snes->ppu->vram;
}

// A BG layer's layout as the PPU has it now (lowpoly.c: bosses drawn on BG2).
// Word addresses; size in 8x8 tiles. Returns the BG mode.
int Snes_N64_BgLayout(int layer, uint16_t *tilemap, uint16_t *tiles, int *wtiles, int *htiles, bool *bigTiles) {
  const BgLayer *L = &g_snes->ppu->bgLayer[layer];
  *tilemap = L->tilemapAdr, *tiles = L->tileAdr;
  *wtiles = L->tilemapWider ? 64 : 32, *htiles = L->tilemapHigher ? 64 : 32;
  *bigTiles = L->bigTiles;
  return g_snes->ppu->mode;
}
