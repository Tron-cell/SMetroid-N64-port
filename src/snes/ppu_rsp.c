// Frame capture for the RSP renderer, and the C reference renderer that draws
// a captured frame. The reference renderer is a line-by-line port of the mode-1
// path of PpuDrawWholeLine() (ppu.c) and must produce identical pixels; it is
// also the specification the RSP microcode is checked against.
#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include "ppu.h"
#include "ppu_rsp.h"

#ifdef __cplusplus
#define PPUR_STATIC_ASSERT static_assert
#else
#define PPUR_STATIC_ASSERT _Static_assert
#endif

#ifndef countof
#define countof(a) (sizeof(a) / sizeof((a)[0]))
#endif

PPUR_STATIC_ASSERT(sizeof(PpuLineRec) == 64, "PpuLineRec must be 64 bytes");
PPUR_STATIC_ASSERT(sizeof(PpuSliver) == 8, "PpuSliver must be 8 bytes");
PPUR_STATIC_ASSERT(sizeof(PpuTileBlock) == 2248, "PpuTileBlock must be 2248 bytes");
PPUR_STATIC_ASSERT(sizeof(PpuFrame) % 16 == 0, "PpuFrame arrays must keep 16-byte alignment");

#ifdef CAPSTAT
long g_cs[8]; unsigned long g_ct[10];   // calls, search steps, blocks invalidated, columns rechecked, fills, pages written per frame, lines, sprites/line
#define CS(i, n) (g_cs[i] += (n))
#else
#define CS(i, n) ((void)0)
#endif
static const int kSpriteSizes[8][2] = {
  {8, 16}, {8, 32}, {8, 64}, {16, 32}, {16, 64}, {32, 64}, {16, 32}, {16, 32}
};

#define SPRITE_PRIO_TO_PRIO(prio, level6) (((prio) * 4 + 2) * 16 + 4 + (level6 ? 2 : 0))

// ---------------------------------------------------------------------------
// Capture state
// ---------------------------------------------------------------------------
static struct {
  Ppu *ppu;               // PPU being captured (NULL = capture off)
  PpuFrame *frame;
  bool cgramDirty;        // CGRAM written since the last snapshot
  bool objDirty;          // OAM / OBSEL / interlace changed: rebuild buckets
  bool vramWritten;       // VRAM written after the first captured line
  bool linesStarted;
  bool vramSnapped;       // frame->vram already holds the pre-write VRAM
  uint32_t captured[8];   // line records written this frame (the others are invalidated at the end)
  uint32_t blockKey[3];   // tile set of the last block built per layer
  uint16_t blockIdx[3];
  uint32_t lineMask[256][4];  // per evaluated line: which of the 128 sprites cover it
  uint8_t lineTiles[256];     // per evaluated line: on-screen sprite tile columns (at most 34 are used)
  // Lines captured whose sprites are not evaluated yet: that is done for all of
  // them at once (PendFlush, at the end of the frame or before OAM / VRAM
  // change), when the sprite data stays in the CPU's data cache.
  int npend;
  uint8_t pendIdx[PPUR_MAX_LINES], pendS0[PPUR_MAX_LINES];
  // Per sprite, worked out once per OAM / OBSEL change (BuildSpriteBuckets):
  struct {
    int16_t x;            // screen x (-256..255)
    uint8_t y, size;
    uint16_t z;           // palette base + priority, as the renderer wants it
    uint16_t objAdr;      // tile base (name select applied)
    uint8_t c0, c1;       // first / end column (multiples of 8) that are on screen
    uint8_t tileRow, tileCol, hflip, vflip;
  } spr[128];
  // Last tile block per layer this frame, with the inputs it was looked up for
  // (CaptureLine reuses it while they don't change: GetTileBlock would return it).
  uint32_t lastKey[3];    // tm0 / tm1 / tile base / start column packed; ~0 = none
  uint32_t lastKey2[3];
  uint16_t lastBlock[3];
  // The last mode 1 record built in full, and g_ppu_regGen at the time: while
  // no record register is written, the next lines start from a copy of it.
  PpuLineRec tmpl __attribute__((aligned(8)));
  uint32_t tmplGen;
  bool tmplValid;
  uint8_t tmplDrawMask, tmplComposeBits;   // the template's decisions before SubBlack
  // SubBlack: palettes used by the last BG3 tilemap row looked at (both screens)
  uint32_t sbKey, sbGen;
  uint8_t sbPals;
} g_cap;
extern uint32_t g_ppu_regGen;
static void PendFlush(void);

static uint8_t kReverseByte[256];
// kSpread[b]: the 8 bits of b as 8 nibbles, bit 7 (leftmost pixel) in the top
// nibble; kSpreadR: the same for the h-flipped byte.
static uint32_t kSpread[256], kSpreadR[256];

// Count trailing zeros of x != 0 (the VR4300 has no instruction for it, and
// __builtin_ctz becomes a slow library call there).
static inline int Ctz32(uint32_t x) {
  static const uint8_t kDeBruijn[32] = {
    0, 1, 28, 2, 29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4, 8,
    31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6, 11, 5, 10, 9 };
  return kDeBruijn[((x & -x) * 0x077CB531u) >> 27];
}

// VRAM pages (128 words) written since the last end-of-frame snapshot, and per
// capture buffer the pages its snapshot lacks, so EndFrame copies only those.
uint32_t g_ppur_vdirty[8];
uint32_t g_ppur_m7dirty[8] = { ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u };
uint32_t g_ppur_pagegen[256];
uint32_t g_ppur_pagestamp[256];
uint32_t g_ppur_stamp = 1;
static struct { PpuFrame *frame; uint32_t dirty[8]; } g_snapbuf[2];
static Ppu *g_snap_ppu;

void PpuRsp_MarkAllVramDirty(void) {
  for (int i = 0; i < 8; i++) g_ppur_vdirty[i] = g_ppur_m7dirty[i] = ~0u;
  for (int i = 0; i < 256; i++) g_ppur_pagegen[i]++, g_ppur_pagestamp[i] = g_ppur_stamp;
}

#if PPUR_REFERENCE
static void SnapshotVram(Ppu *ppu, PpuFrame *frame) {
  if (ppu != g_snap_ppu) {          // another PPU: nothing is known
    g_snap_ppu = ppu;
    PpuRsp_MarkAllVramDirty();
  }
  for (int j = 0; j < 2; j++)
    for (int i = 0; i < 8; i++) g_snapbuf[j].dirty[i] |= g_ppur_vdirty[i];
  memset(g_ppur_vdirty, 0, sizeof(g_ppur_vdirty));
  int k = g_snapbuf[0].frame == frame ? 0 : g_snapbuf[1].frame == frame ? 1 : -1;
  if (k < 0) {                      // new buffer: replace the older slot, copy everything
    k = g_snapbuf[0].frame == NULL ? 0 : 1;
    if (g_snapbuf[0].frame && g_snapbuf[1].frame) {
      g_snapbuf[0] = g_snapbuf[1];
      k = 1;
    }
    g_snapbuf[k].frame = frame;
    for (int i = 0; i < 8; i++) g_snapbuf[k].dirty[i] = ~0u;
  }
  for (int i = 0; i < 8; i++) {
    uint32_t m = g_snapbuf[k].dirty[i];
    while (m) {
      int b = Ctz32(m);
      m &= m - 1;
      int page = i * 32 + b;
      memcpy(&frame->vram[page * 128], &ppu->vram[page * 128], 256);
    }
    g_snapbuf[k].dirty[i] = 0;
  }
}
#endif

// Tile block lookup by key: a small open-addressing table (rebuilt each frame,
// at most PPUR_MAX_BLOCKS of its 256 slots used), so a lookup doesn't scan all blocks.
static inline unsigned BlockHash(uint32_t key) { return (key * 0x9E3779B1u) >> 24; }
static void BlockHashInsert(PpuFrame *f, int bi) {
  unsigned h = BlockHash(f->bkey[bi]);
  while (f->bhash[h]) h = (h + 1) & 255;
  f->bhash[h] = bi + 1;
}

void PpuRsp_BeginFrame(Ppu *ppu, PpuFrame *frame) {
  if (!kReverseByte[1]) {
    for (int i = 0; i < 256; i++) {
      int r = 0;
      for (int b = 0; b < 8; b++) if (i & (1 << b)) r |= 0x80 >> b;
      kReverseByte[i] = r;
    }
    for (int i = 0; i < 256; i++) {
      uint32_t v = 0, vr = 0;
      for (int b = 0; b < 8; b++) {
        if (i & (0x80 >> b)) v |= 1u << (28 - 4 * b);
        if (kReverseByte[i] & (0x80 >> b)) vr |= 1u << (28 - 4 * b);
      }
      kSpread[i] = v, kSpreadR[i] = vr;
    }
  }
  g_cap.ppu = ppu;
  g_cap.frame = frame;
  g_cap.cgramDirty = true;
  g_cap.objDirty = true;
  g_cap.vramWritten = false;
  g_cap.linesStarted = false;
  g_cap.vramSnapped = false;
  frame->numLines = 0;
  frame->numCgrams = 0;
  frame->numSlivers = 0;
  // Tile blocks persist in this buffer: keep the ones its previous frame used,
  // free the rest (start over for another PPU).
  if (frame->bppu != ppu) {
    frame->bppu = ppu;
    frame->numBlocks = 0;
  }
  for (int i = 0; i < frame->numBlocks; i++)
    if (frame->bstamp[i] != frame->blastStamp) frame->bkey[i] = ~0u;
  while (frame->numBlocks && frame->bkey[frame->numBlocks - 1] == ~0u) frame->numBlocks--;
  memset(frame->bhash, 0, sizeof(frame->bhash));
  for (int i = 0; i < frame->numBlocks; i++)
    if (frame->bkey[i] != ~0u) BlockHashInsert(frame, i);
  g_ppur_stamp++;
  frame->blastStamp = g_ppur_stamp;
  frame->numM7 = 0;
  frame->numM7Jobs = 0;
  frame->overflow = false;
  for (int i = 0; i < 3; i++) { g_cap.blockKey[i] = ~0u; g_cap.blockIdx[i] = PPUR_NO_BLOCK; g_cap.lastKey[i] = ~0u; }
  memset(g_cap.captured, 0, sizeof(g_cap.captured));
  g_cap.tmplValid = false;
  g_cap.npend = 0;
}

// Zero a 64-byte line record. On the N64 the cache lines are claimed without
// reading them from RDRAM first (Create Dirty Exclusive): they are all overwritten.
static inline void ZeroRec(PpuLineRec *rec) {
  uint64_t *p = (uint64_t *)rec;
#if defined(__mips__) && !defined(RIG)
  for (int i = 0; i < 4; i++) __asm__ volatile("cache 0x0d, %0" :: "m"(p[i * 2]));
#endif
  for (int i = 0; i < 8; i++) p[i] = 0;
}

void PpuRsp_EndFrame(Ppu *ppu) {
  if (g_cap.ppu != ppu || !g_cap.frame)
    return;
  if (g_cap.npend) PendFlush();
#ifdef CAPSTAT
  for (int i = 0; i < 256; i++) g_cs[5] += g_ppur_pagestamp[i] >= g_cap.frame->blastStamp;
  g_cs[6] += g_cap.frame->numLines;
  g_cs[7] += g_cap.frame->numSlivers;
#endif
  for (int i = 0; i < g_cap.frame->numLines; i++)
    if (!(g_cap.captured[i >> 5] & (1u << (i & 31))))
      g_cap.frame->lines[i].flags = 0, g_cap.frame->lsig[i] = 0;
#if PPUR_REFERENCE
  if (!g_cap.vramSnapped) {
    SnapshotVram(ppu, g_cap.frame);
  } else {
    // frame->vram holds VRAM from before a mid-frame write: recopy it fully next time
    for (int j = 0; j < 2; j++)
      if (g_snapbuf[j].frame == g_cap.frame)
        for (int i = 0; i < 8; i++) g_snapbuf[j].dirty[i] = ~0u;
  }
#endif
  g_cap.ppu = NULL;
}

SNES_HOT void PpuRsp_NoteCgramWrite(Ppu *ppu) {
  if (g_cap.ppu == ppu) g_cap.cgramDirty = true;
}

SNES_HOT void PpuRsp_NoteObjChange(Ppu *ppu) {
  if (g_cap.ppu == ppu) {
    if (g_cap.npend) PendFlush();     // (called before the change)
    g_cap.objDirty = true;
  }
}

SNES_HOT void PpuRsp_NoteVramWrite(Ppu *ppu) {
  if (g_cap.ppu == ppu && g_cap.npend) PendFlush();   // sprite tiles as they are before the write
  if (g_cap.ppu == ppu && g_cap.linesStarted && !g_cap.vramSnapped) {
    // Lines captured so far must see VRAM as it was before this write.
    // ppu_write calls this before modifying VRAM.
#if PPUR_REFERENCE
    memcpy(g_cap.frame->vram, ppu->vram, sizeof(ppu->vram));
#endif
    g_cap.vramSnapped = true;
  }
}

// Which sprites cover each (evaluated) line. Same tests as ppu_evaluateSprites:
// row = (uint8)(line - y) < height, and x > -size.
static __attribute__((noinline)) void BuildSpriteBuckets(Ppu *ppu) {
  memset(g_cap.lineMask, 0, sizeof(g_cap.lineMask));
  memset(g_cap.lineTiles, 0, sizeof(g_cap.lineTiles));
  for (int i = 0; i < 128; i++) {
    int index = i * 2;
    uint8_t y = ppu->oam[index] >> 8;
    int spriteSize = kSpriteSizes[ppu->objSize][(ppu->highOam[index >> 3] >> ((index & 7) + 1)) & 1];
    int spriteHeight = ppu->objInterlace ? spriteSize / 2 : spriteSize;
    int x = ppu->oam[index] & 0xff;
    x |= ((ppu->highOam[index >> 3] >> (index & 7)) & 1) << 8;
    if (x > 255) x -= 512;
    if (!(x > -spriteSize))
      continue;
    // what EvaluateSpritesToSlivers needs of this sprite
    int oam1 = ppu->oam[index + 1];
    int c0 = 0, c1 = 0;
    while (c0 < spriteSize && !(c0 + x > -8)) c0 += 8;
    for (c1 = c0; c1 < spriteSize && c1 + x < 256; c1 += 8) {}
    int tiles = (c1 - c0) >> 3;
    for (int r = 0; r < spriteHeight; r++) {
      uint8_t l = y + r;
      g_cap.lineMask[l][i >> 5] |= 1u << (i & 31);
      int t = g_cap.lineTiles[l] + tiles;
      g_cap.lineTiles[l] = t > 34 ? 34 : t;
    }
    g_cap.spr[i].x = x;
    g_cap.spr[i].y = y;
    g_cap.spr[i].size = spriteSize;
    g_cap.spr[i].z = 0x80 + 16 * ((oam1 & 0xe00) >> 9) + (SPRITE_PRIO_TO_PRIO((oam1 & 0x3000) >> 12, (oam1 & 0x800) == 0) << 8);
    g_cap.spr[i].objAdr = (oam1 & 0x100) ? ppu->objTileAdr2 : ppu->objTileAdr1;
    g_cap.spr[i].c0 = c0;
    g_cap.spr[i].c1 = c1;
    g_cap.spr[i].tileRow = (oam1 & 0xff) >> 4;
    g_cap.spr[i].tileCol = oam1 & 0xf;
    g_cap.spr[i].hflip = (oam1 & 0x4000) != 0;
    g_cap.spr[i].vflip = (oam1 & 0x8000) != 0;
  }
}

// Port of ppu_evaluateSprites() that emits slivers instead of drawing pixels.
// Returns the number of slivers (tilesFound, max 34).
static int EvaluateSpritesToSlivers(Ppu *ppu, int line, PpuSliver *out, int max_out, int s0) {
  const uint32_t *mask = g_cap.lineMask[(uint8_t)line];
  if (!(mask[0] | mask[1] | mask[2] | mask[3]))
    return 0;
  int spritesFound = 0, tilesFound = 0, n = 0;
  // Visit the sprites of this line in OAM order starting at s0 (wrapping),
  // like the original 128-step loop but skipping sprites not on the line.
  for (int k = 0; k < 128; k++) {
    int s = (s0 + k) & 127;
    uint32_t w = mask[s >> 5] >> (s & 31);
    if (!w) { k += 31 - (s & 31); continue; }
    k += Ctz32(w);
    if (k >= 128) break;
    s = (s0 + k) & 127;
    const __typeof__(g_cap.spr[0]) *sp = &g_cap.spr[s];
    uint8_t row = line - sp->y;
    int spriteSize = sp->size;
    spritesFound++;
    if (spritesFound > 32) {
      ppu->rangeOver = true;
      break;
    }
    if (ppu->objInterlace) row = row * 2 + (ppu->evenFrame ? 0 : 1);
    if (sp->vflip) row = spriteSize - 1 - row;
    uint16_t z = sp->z;
    int x = sp->x;
    // tile row of this line (the column part is added per 8 pixels)
    int rowBase = sp->objAdr + (((sp->tileRow + (row >> 3)) << 4) << 4) + (row & 7);
    const uint16_t *vram = ppu->vram;
    for (int col = sp->c0; col < sp->c1; col += 8) {
      tilesFound++;
      if (tilesFound > 34) {
        ppu->timeOver = true;
        break;
      }
      int usedCol = sp->hflip ? spriteSize - 1 - col : col;
      const uint16_t *addr = &vram[(rowBase + (((sp->tileCol + (usedCol >> 3)) & 0xf) << 4)) & 0x7fff];
      uint32_t p01 = addr[0], p23 = addr[8];
      uint32_t planes;
      if (sp->hflip)
        planes = kReverseByte[p01 & 0xff] | kReverseByte[p01 >> 8] << 8 | kReverseByte[p23 & 0xff] << 16 | kReverseByte[p23 >> 8] << 24;
      else
        planes = (p01 & 0xffff) | p23 << 16;
      if (n < max_out) {
        out[n].x = col + x;
        out[n].z = z;
        out[n].planes = planes;
        n++;
      }
    }
    if (tilesFound > 34) break;
  }
  return n;
}

long g_ppur_why[6];
long g_ppur_bstat[4];   // block cache: hits, builds, builds where only the column changed  // fallback reasons: mode, mosaic, extra, vram-mid-frame, overflow, cgram
static const uint16_t kZhi[3] = { 0xc000, 0xb100, 0xf200 }, kZlo[3] = { 0x8000, 0x7100, 0x1200 };

// Tile address (VRAM words) of a tilemap entry.
static inline int TileAdr(uint16_t entry, int tileBase, bool bpp4) {
  int tile = entry & 0x3ff;
  return bpp4 ? (tileBase + tile * 16) & 0x7fff : (tileBase + tile * 8) & 0x7fff;
}

// Column c of block bi uses VRAM page adr >> 7: note it (the page bitmap for
// the quick "anything changed?" test, the page list for which columns to recheck).
static inline void AddDep(PpuFrame *f, int bi, int c, int adr) {
  unsigned p = (adr >> 7) & 255;
  f->bdeps[bi][p >> 5] |= 1u << (p & 31);
  int n = f->bdpn[bi];
  if (n == PPUR_DEP_MANY) return;
  for (int i = 0; i < n; i++)
    if (f->bdpage[bi][i] == p) { f->bdmask[bi][i] |= 1ull << c; return; }
  if (n == PPUR_DEP_PAGES) { f->bdpn[bi] = PPUR_DEP_MANY; return; }
  f->bdpage[bi][n] = p;
  f->bdmask[bi][n] = 1ull << c;
  f->bdpn[bi] = n + 1;
}

// Fills column c of block bi (layer `layer`, tile row L) from tilemap entry `entry`.
static __attribute__((noinline)) void FillColumn(Ppu *ppu, PpuFrame *f, int bi, int layer, const PpuLayerRec *L, int c, uint16_t entry) {
  PpuTileBlock *b = &f->blocks[bi];
  const uint16_t *vram = ppu->vram;
  bool bpp4 = layer != 2;
  int adr = TileAdr(entry, L->ta0 & ~7, bpp4);
  f->bentry[bi][c] = entry;
  f->bgen[bi][c] = g_ppur_pagegen[adr >> 7];
  AddDep(f, bi, c, adr);
  f->bwritten[bi] = 1;
  uint16_t z = (entry & 0x2000) ? kZhi[layer] : kZlo[layer];
  z += (entry & 0x1c00) >> (bpp4 ? 6 : 8);
  b->z[c] = z ^ 0x8000;
  if (c < 34) b->z[64 + c] = z ^ 0x8000;
  // adr is tile-aligned, so adr + 16 never wraps. Flips are applied here.
  bool hflip = entry & 0x4000, vflip = entry & 0x8000;
  uint8_t *dst = b->tiles[c];
  if (bpp4) {
    // 4bpp: one nibble per pixel (pixel 0 in the top nibble), so the RSP
    // doesn't decode bit planes. Bytes 0-15: pixels 0-3 of rows 0-7, bytes
    // 16-31: pixels 4-7 of rows 0-7, big-endian halfwords (RDRAM order).
    const uint32_t *sp = hflip ? kSpreadR : kSpread;
    for (int r = 0; r < 8; r++) {
      int sr = vflip ? 7 - r : r;
      uint16_t w01 = vram[adr + sr], w23 = vram[adr + 8 + sr];
      uint32_t px = sp[w01 & 0xff] | sp[w01 >> 8] << 1 | sp[w23 & 0xff] << 2 | sp[w23 >> 8] << 3;
      dst[r * 2] = px >> 24;
      dst[r * 2 + 1] = px >> 16;
      dst[16 + r * 2] = px >> 8;
      dst[16 + r * 2 + 1] = px;
    }
    return;
  }
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  if (!hflip && !vflip) {   // N64: native order is RDRAM order; tiles are 16-byte aligned
    const uint64_t *sp = (const uint64_t *)&vram[adr];
    uint64_t *dp = (uint64_t *)dst;
    dp[0] = sp[0]; dp[1] = sp[1];
    return;
  }
#endif
  // 2bpp: planes 0/1 per row as stored in VRAM
  for (int r = 0; r < 8; r++) {
    uint16_t v = vram[adr + (vflip ? 7 - r : r)];
    if (hflip) v = kReverseByte[v & 0xff] | kReverseByte[v >> 8] << 8;
    dst[r * 2] = v >> 8;             // big-endian halfword (RDRAM order)
    dst[r * 2 + 1] = v & 0xff;
  }
}

// The block holding layer `layer`'s current tile row, with the 34 columns
// this line needs valid (see PpuTileBlock). Blocks are found by key among this
// buffer's blocks (kept from its previous frame); each column is checked once
// per frame against the live tilemap entry and its tile page's write count.
static __attribute__((noinline)) uint16_t GetTileBlock(Ppu *ppu, PpuFrame *f, int layer, const PpuLayerRec *L) {
  CS(0, 1);
  int start = (L->hscroll >> 3) & 63;      // screen * 32 + column
  int tileBase = L->ta0 & ~7;
  bool bpp4 = layer != 2;
  // tm0/tm1 are 32-word aligned and < 0x8000, tileBase is 0x1000-aligned: the key is exact.
  uint32_t key = (uint32_t)layer << 30 | ((uint32_t)(L->tm0 >> 5) << 20) | ((uint32_t)(L->tm1 >> 5) << 10) |
                 ((uint32_t)(tileBase >> 12) << 6);
  uint64_t need;
  if (start + 34 <= 64) need = ((1ull << 34) - 1) << start;
  else need = (~0ull << start) | ((1ull << (start + 34 - 64)) - 1);
  int bi;
#ifdef CAPSTAT
  unsigned long _ta = CsCyc();
#endif
  if (g_cap.blockKey[layer] == key && g_cap.blockIdx[layer] != PPUR_NO_BLOCK) {
    bi = g_cap.blockIdx[layer];
  } else {
    bi = -1;
    for (unsigned h = BlockHash(key); f->bhash[h]; h = (h + 1) & 255) {
      CS(1, 1);
      if (f->bkey[f->bhash[h] - 1] == key) { bi = f->bhash[h] - 1; break; }
    }
    if (bi < 0) {
      g_ppur_bstat[1]++;
      for (bi = 0; bi < f->numBlocks && f->bkey[bi] != ~0u; bi++) {}   // a free slot, or a new one
      if (bi == f->numBlocks) {
        if (f->numBlocks >= PPUR_MAX_BLOCKS)
          return PPUR_NO_BLOCK;
        f->numBlocks++;
      }
      f->bkey[bi] = key;
      BlockHashInsert(f, bi);
      f->bfilled[bi] = 0;
      f->bsusp[bi] = 0;
      f->bvalid[bi] = 0;
      memset(f->bdeps[bi], 0, sizeof(f->bdeps[bi]));
      f->bdpn[bi] = 0;
      f->bstamp[bi] = 0;
      f->bwritten[bi] = 0;
    }
    g_cap.blockKey[layer] = key;
    g_cap.blockIdx[layer] = bi;
  }
#ifdef CAPSTAT
  unsigned long _tb = CsCyc(); g_ct[5] += _tb - _ta;
#endif
  if (f->bstamp[bi] != g_ppur_stamp) {
    // First use this frame: are the VRAM pages its columns came from unchanged
    // since they were last checked? Then all filled columns are still valid.
    // Otherwise they become "suspect": each is checked (tilemap entry and its
    // tile page's write count) only when a line needs it, and the page list is
    // rebuilt from the columns checked.
    f->bstamp[bi] = g_ppur_stamp;
    uint32_t valid = f->bvalid[bi];
    bool same = valid != 0, all = !same;
    for (int w = 0; w < 8 && same; w++) {
      uint32_t m = f->bdeps[bi][w];
      while (m) {
        int p = w * 32 + Ctz32(m);
        m &= m - 1;
        if (g_ppur_pagestamp[p] >= valid) { same = false; break; }
      }
    }
    // (the tilemap rows are deps too: a change there makes every column suspect)
    if (g_ppur_pagestamp[L->tm0 >> 7] >= valid || g_ppur_pagestamp[L->tm1 >> 7] >= valid)
      same = false, all = true;
    if (!same && !all) {
      // Only the columns whose tile pages were written become suspect, if the
      // page list is complete.
      int n = f->bdpn[bi];
      if (n == PPUR_DEP_MANY) {
        all = true;
      } else {
        uint64_t sus = 0;
        for (int i = 0; i < n; i++)
          if (g_ppur_pagestamp[f->bdpage[bi][i]] >= valid) sus |= f->bdmask[bi][i];
        CS(2, 1);
        sus &= f->bfilled[bi];
        f->bsusp[bi] |= sus;
        f->bfilled[bi] &= ~sus;
      }
    }
    if (all && valid) {
      CS(2, 1);
      f->bsusp[bi] |= f->bfilled[bi];
      f->bfilled[bi] = 0;
      memset(f->bdeps[bi], 0, sizeof(f->bdeps[bi]));
      f->bdpn[bi] = 0;
    }
    f->bchecked[bi] = f->bfilled[bi];
    f->bvalid[bi] = g_ppur_stamp;
  }
#ifdef CAPSTAT
  unsigned long _tc = CsCyc(); g_ct[6] += _tc - _tb;
#endif
  uint64_t todo = need & ~f->bchecked[bi];
  if (todo) {
    f->bchecked[bi] |= todo;
    const uint16_t *vram = ppu->vram;
    uint64_t known = f->bfilled[bi] | f->bsusp[bi];
    f->bfilled[bi] |= todo;
    f->bsusp[bi] &= ~todo;
    const uint16_t *bentry = f->bentry[bi];
    const uint32_t *bgen = f->bgen[bi];
    do {
      uint32_t lo = (uint32_t)todo, c = lo ? Ctz32(lo) : 32 + Ctz32((uint32_t)(todo >> 32));
      todo &= todo - 1;
      uint16_t entry = vram[((c & 32) ? L->tm1 : L->tm0) + (c & 31)];
      CS(3, 1);
      if (known >> c & 1) {
        int adr = TileAdr(entry, tileBase, bpp4);
        if (bentry[c] == entry && bgen[c] == g_ppur_pagegen[adr >> 7]) {
          AddDep(f, bi, c, adr);   // still valid
          continue;
        }
      }
      g_ppur_bstat[2]++;
      CS(4, 1);
      CAPSTAT_T(8, FillColumn(ppu, f, bi, layer, L, c, entry));
    } while (todo);
  }
  g_ppur_bstat[0]++;
#ifdef CAPSTAT
  g_ct[7] += CsCyc() - _tc;
#endif
  return bi;
}

static void SetComposeBits(PpuLineRec *rec);

// Mode 7 VRAM as two byte arrays (tilemap = low bytes of words 0..0x3fff,
// tile pixels = high bytes): half the memory to touch per pixel. Refreshed
// page by page from the VRAM dirty bits.
uint8_t g_m7map[0x4000] __attribute__((aligned(16))), g_m7pix[0x4000] __attribute__((aligned(16)));
bool g_ppur_m7_rsp;
volatile bool g_ppur_m7_dirty = true, g_ppur_m7_busy;
long g_ppur_m7stat[4];

static uint32_t g_m7gen = 1;
uint32_t PpuRsp_M7Gen(void) { return g_m7gen; }


static int64_t FloorDiv(int64_t a, int64_t b) { int64_t q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) q--; return q; }
static int64_t CeilDiv(int64_t a, int64_t b) { return -FloorDiv(-a, b); }
// Narrow [*lo, *hi] to the x where 0 <= p + d * x <= 0x3ffff.
static void M7Range(int32_t p, int32_t d, int *lo, int *hi) {
  int64_t a, b;
  if (d == 0) {
    if (p < 0 || p > 0x3ffff) *lo = 1, *hi = 0;
    return;
  }
  if (d > 0) a = CeilDiv(-(int64_t)p, d), b = FloorDiv(0x3ffff - (int64_t)p, d);
  else       a = CeilDiv(0x3ffff - (int64_t)p, d), b = FloorDiv(-(int64_t)p, d);
  if (a > *lo) *lo = a > 256 ? 256 : (int)a;
  if (b < *hi) *hi = b < -1 ? -1 : (int)b;
}

static void RefreshMode7Copies(const uint16_t *vram) {
  for (int w = 0; w < 4; w++) {
    uint32_t bits = g_ppur_m7dirty[w];
    if (!bits) continue;
    while (g_ppur_m7_busy) {}       // the RSP is reading them (only for a moment after a frame starts)
    g_ppur_m7dirty[w] = 0;
    uint8_t changed = 0;   // the generation (part of the row keys) only moves on real changes
    while (bits) {
      int b = Ctz32(bits);
      bits &= bits - 1;
      int base = (w * 32 + b) * 128;
      for (int i = 0; i < 128; i++) {
        uint16_t v = vram[base + i];
        changed |= (g_m7map[base + i] ^ (uint8_t)v) | (g_m7pix[base + i] ^ (uint8_t)(v >> 8));
        g_m7map[base + i] = v & 0xff;
        g_m7pix[base + i] = v >> 8;
      }
    }
    if (changed) g_m7gen++, g_ppur_m7_dirty = true;
  }
}

// BG1 colour indices of a mode 7 line, as PpuDrawBackground_mode7 in ppu.c
// computes them (without mosaic; windows are applied by the renderer).
static __attribute__((noinline)) void Mode7Row(Ppu *ppu, unsigned y, uint8_t *out, uint32_t *key, PpuFrame *f, int idx) {
  RefreshMode7Copies(ppu->vram);
  int hScroll = ((int16_t)(ppu->m7matrix[6] << 3)) >> 3;
  int vScroll = ((int16_t)(ppu->m7matrix[7] << 3)) >> 3;
  int xCenter = ((int16_t)(ppu->m7matrix[4] << 3)) >> 3;
  int yCenter = ((int16_t)(ppu->m7matrix[5] << 3)) >> 3;
  int clippedH = hScroll - xCenter;
  int clippedV = vScroll - yCenter;
  clippedH = (clippedH & 0x2000) ? (clippedH | ~1023) : (clippedH & 1023);
  clippedV = (clippedV & 0x2000) ? (clippedV | ~1023) : (clippedV & 1023);
  uint32_t ry = ppu->m7yFlip ? 255 - y : y;
  uint32_t m7startX = (ppu->m7matrix[0] * clippedH & ~63) + (ppu->m7matrix[1] * ry & ~63) +
    (ppu->m7matrix[1] * clippedV & ~63) + (xCenter << 8);
  uint32_t m7startY = (ppu->m7matrix[2] * clippedH & ~63) + (ppu->m7matrix[3] * ry & ~63) +
    (ppu->m7matrix[3] * clippedV & ~63) + (yCenter << 8);
  uint32_t rx = ppu->m7xFlip ? 255 : 0;
  uint32_t xpos = m7startX + ppu->m7matrix[0] * rx;
  uint32_t ypos = m7startY + ppu->m7matrix[2] * rx;
  uint32_t dx = ppu->m7xFlip ? -ppu->m7matrix[0] : ppu->m7matrix[0];
  uint32_t dy = ppu->m7xFlip ? -ppu->m7matrix[2] : ppu->m7matrix[2];
  // Same inputs as when this frame buffer's row was last computed? Then it is still there.
  uint32_t k0 = xpos, k1 = ypos, k2 = (dx & 0xffff) | dy << 16,
           k3 = g_m7gen << 2 | ppu->m7largeField << 1 | ppu->m7charFill;
  if (key[0] == k0 && key[1] == k1 && key[2] == k2 && key[3] == k3)
    return;
  key[0] = k0, key[1] = k1, key[2] = k2, key[3] = k3;
  const uint8_t *map = g_m7map, *pix = g_m7pix;
  g_ppur_m7stat[0]++; if (dy == 0) g_ppur_m7stat[1]++; if (dy == 0 && dx == 0x100) g_ppur_m7stat[2]++; if (ppu->m7largeField) g_ppur_m7stat[3]++;
  if (dy == 0) {   // no rotation: one tilemap row, and the same row of each tile
    const uint8_t *mrow = map + ((ypos >> 4) & 0x3f80);
    const uint8_t *prow = pix + ((ypos >> 5) & 0x38);
    // Inside the plane for the whole line? (Without largeField the plane repeats.)
    bool inside = !ppu->m7largeField ||
        (ypos <= 0x3ffff && (int32_t)dx >= 0 && xpos <= 0x3ffff && xpos + 255 * dx <= 0x3ffff);
    if (inside && dx == 0x100 && g_ppur_m7_rsp) {
      // Scale 1: a window of plane row Y from column X0; rsp_m7.S builds it.
      uint16_t *j = f->m7job[f->numM7Jobs++];
      j[0] = idx;
      j[1] = (ypos >> 8) & 1023;
      j[2] = (xpos >> 8) & 1023;
      j[3] = 0;
      return;
    }
    if (inside && dx == 0x100) {
      // Scale 1: whole 8-pixel tile rows, 8 bytes at a time (out has 8 bytes of slack).
      typedef struct __attribute__((packed, may_alias)) { uint64_t v; } U64u;
      int o = (xpos >> 8) & 7;
      unsigned col = xpos >> 11;
      int x = -o;
      for (; x < 256; x += 8, col++) {
        const uint8_t *trow = prow + (mrow[col & 0x7f] << 6);
        if (x < 0) {
          for (int k = o; k < 8; k++) out[x + k] = trow[k];
        } else if (x <= 248) {
          ((U64u *)(out + x))->v = *(const uint64_t *)trow;
        } else {   // last partial tile: don't write into the next row (it may be reused)
          for (int k = 0; x + k < 256; k++) out[x + k] = trow[k];
        }
      }
      return;
    }
    if (inside && (int32_t)dx > -0x400 && (int32_t)dx < 0x400) {
      // Zoomed in: runs of pixels from the same tile; transparent tile rows are cleared at once.
      int x = 0;
      while (x < 256) {
        const uint8_t *trow = prow + (mrow[(xpos >> 11) & 0x7f] << 6);
        int n;
        if ((int32_t)dx > 0) n = (int)(((((xpos >> 11) + 1) << 11) - xpos + dx - 1) / dx);
        else if ((int32_t)dx < 0) n = (int)((xpos & 0x7ff) / (uint32_t)-(int32_t)dx) + 1;
        else n = 256;
        if (n > 256 - x) n = 256 - x;
        if (*(const uint64_t *)trow == 0) {
          memset(out + x, 0, n);
          xpos += dx * n;
        } else {
          for (int k = 0; k < n; k++, xpos += dx) out[x + k] = trow[(xpos >> 8) & 7];
        }
        x += n;
      }
      return;
    }
    if (inside) {
      for (int x = 0; x < 256; x++, xpos += dx)
        out[x] = prow[mrow[(xpos >> 11) & 0x7f] << 6 | ((xpos >> 8) & 7)];
      return;
    }
  } else if (!ppu->m7largeField) {
    for (int x = 0; x < 256; x++, xpos += dx, ypos += dy)
      out[x] = pix[map[((ypos >> 4) & 0x3f80) | ((xpos >> 11) & 0x7f)] << 6 | ((ypos >> 5) & 0x38) | ((xpos >> 8) & 7)];
    return;
  }
  bool char_fill = ppu->m7charFill;
  if (ppu->m7largeField && !char_fill) {
    // Outside the 1024x1024 plane is transparent: work out the x range where
    // both coordinates are inside, clear the rest, and loop over that range only.
    int lo = 0, hi = 255;
    M7Range((int32_t)xpos, (int32_t)dx, &lo, &hi);
    M7Range((int32_t)ypos, (int32_t)dy, &lo, &hi);
    if (lo > hi) { memset(out, 0, 256); return; }
    memset(out, 0, lo);
    memset(out + hi + 1, 0, 255 - hi);
    xpos += dx * lo, ypos += dy * lo;
    for (int x = lo; x <= hi; x++, xpos += dx, ypos += dy)
      out[x] = pix[map[((ypos >> 4) & 0x3f80) | ((xpos >> 11) & 0x7f)] << 6 | ((ypos >> 5) & 0x38) | ((xpos >> 8) & 7)];
    return;
  }
  for (int x = 0; x < 256; x++, xpos += dx, ypos += dy) {
    unsigned tile;
    if (ppu->m7largeField && (uint32_t)(xpos | ypos) > 0x3ffff) {
      if (!char_fill) { out[x] = 0; continue; }
      tile = 0;
    } else {
      tile = map[((ypos >> 4) & 0x3f80) | ((xpos >> 11) & 0x7f)];
    }
    out[x] = pix[tile << 6 | ((ypos >> 5) & 0x38) | ((xpos >> 8) & 7)];
  }
}

long g_ppur_modes[16];
// Byte mask of the record fields that decide a line's pixels (all but flags'
// kLR_Copy bit, cgramIdx, sliverStart, block[] and pad), as 8 native-order words.
static uint64_t g_recMask[8];
static void InitRecMask(void) {
  uint8_t m[64];
  memset(m, 0xff, sizeof(m));
  m[offsetof(PpuLineRec, flags)] = (uint8_t)~kLR_Copy;
  m[offsetof(PpuLineRec, cgramIdx)] = 0;
  memset(m + offsetof(PpuLineRec, sliverStart), 0, 2);
  memset(m + offsetof(PpuLineRec, block), 0, sizeof(((PpuLineRec *)0)->block));
  memset(m + offsetof(PpuLineRec, pad), 0, sizeof(((PpuLineRec *)0)->pad));
  memcpy(g_recMask, m, sizeof(m));
}

// Fingerprint of a captured line while its data is still in the cache, so
// PpuRsp_MarkCopies can reject changed lines without reading the records,
// slivers and mode 7 rows back (only equal fingerprints are compared in full).
static uint64_t LineSig(const PpuFrame *f, const PpuLineRec *rec, const uint32_t *m7key) {
  if (!g_recMask[0]) InitRecMask();
  const uint64_t *a = (const uint64_t *)rec;
  uint64_t h = 0;
  #define PPUR_MIX(w) (h = ((h << 7) | (h >> 57)) + (w))
  for (int i = 0; i < 8; i++) PPUR_MIX(a[i] & g_recMask[i]);
  const PpuSliver *sl = &f->slivers[rec->sliverStart];
  for (int i = 0; i < rec->sliverCount; i++) PPUR_MIX((uint64_t)(uint16_t)sl[i].x << 48 ^ (uint64_t)sl[i].z << 32 ^ sl[i].planes);
  if (m7key) {   // the inputs the mode 7 row was computed from (they fix its content)
    PPUR_MIX((uint64_t)m7key[0] << 32 | m7key[1]);
    PPUR_MIX((uint64_t)m7key[2] << 32 | m7key[3]);
  }
  #undef PPUR_MIX
  return h | 1;   // 0 marks lines that are not RSP-drawn
}

// Tile block of layer `layer` for record rec (mode 1), reusing the last one
// while its inputs are the same.
static inline uint16_t LayerBlock(Ppu *ppu, PpuFrame *f, int layer, const PpuLayerRec *L) {
  uint32_t k = (uint32_t)(L->tm0 >> 5) << 16 | (L->tm1 >> 5) << 6 | ((L->hscroll >> 3) & 63);
  uint32_t k2 = L->ta0 & ~7;
  if (k == g_cap.lastKey[layer] && k2 == g_cap.lastKey2[layer])
    return g_cap.lastBlock[layer];
  uint16_t bi;
  CAPSTAT_T(0, bi = GetTileBlock(ppu, f, layer, L));
  g_cap.lastKey[layer] = bi == PPUR_NO_BLOCK ? ~0u : k;
  g_cap.lastKey2[layer] = k2;
  g_cap.lastBlock[layer] = bi;
  return bi;
}

static inline void LayerRows(const BgLayer *bglayer, int line, PpuLayerRec *L) {
  unsigned y = line + bglayer->vScroll;
  int sc_offs = bglayer->tilemapAdr + (((y >> 3) & 0x1f) << 5);
  if ((y & 0x100) && bglayer->tilemapHigher)
    sc_offs += bglayer->tilemapWider ? 0x800 : 0x400;
  L->tm0 = sc_offs & 0x7fff;
  L->tm1 = (sc_offs + (bglayer->tilemapWider ? 0x400 : 0)) & 0x7fff;
  L->ta0 = bglayer->tileAdr + (y & 7);
  L->ta1 = bglayer->tileAdr + 7 - (y & 7);
  L->hscroll = bglayer->hScroll;
}

static uint64_t LineSig(const PpuFrame *f, const PpuLineRec *rec, const uint32_t *m7key);
static void PendLine(Ppu *ppu, PpuFrame *f, PpuLineRec *rec, int line, int idx);

// Most rooms keep BG3 on the sub screen, added to everything, as a layer whose
// tiles all use black palettes (the cleared FX tilemap): the sum changes
// nothing. A line where the sub screen holds only BG3, every BG3 tile of the
// row uses a palette that is black, the fixed colour (shown where BG3 is
// transparent) is black too, and math only adds, looks the same without the
// sub screen and without colour math: the RSP then skips drawing the sub
// screen and composes the line the simple way. (Saves several ms per frame.)
static void SubBlack(Ppu *ppu, PpuLineRec *r) {
  if ((r->drawMask & 0x78) != 0x60 || (r->screenEnabled[1] & 0x10) || r->fixedColor ||
      (r->mathFlags & 6) || (r->flags & kLR_Mode7) || ppu->bgLayer[2].bigTiles)
    return;   // (0x60: sub screen rendered, and of the sub BGs only BG3)
  const PpuLayerRec *L = &r->bg[2];
  uint32_t key = (uint32_t)L->tm0 << 16 | L->tm1;
  uint32_t gen = g_ppur_pagegen[(L->tm0 >> 7) & 255] + g_ppur_pagegen[(L->tm1 >> 7) & 255];
  if (key != g_cap.sbKey || gen != g_cap.sbGen) {   // palettes of the row's 64 entries
    uint8_t pals = 0;
    for (int c = 0; c < 32; c++)
      pals |= 1 << (ppu->vram[(L->tm0 + c) & 0x7fff] >> 10 & 7) | 1 << (ppu->vram[(L->tm1 + c) & 0x7fff] >> 10 & 7);
    g_cap.sbKey = key, g_cap.sbGen = gen, g_cap.sbPals = pals;
  }
  for (int p = 0; p < 8; p++)   // mode 1 BG3: 4 colours per palette, 0 transparent
    if ((g_cap.sbPals >> p & 1) && ((ppu->cgram[p * 4 + 1] | ppu->cgram[p * 4 + 2] | ppu->cgram[p * 4 + 3]) & 0x7fff))
      return;
  r->drawMask &= ~0x60;
  r->composeBits &= ~(kCB_Math | kCB_SubUsable);
}

// A mode 1 line whose record registers equal those of the template: only the
// scroll-dependent fields, tile blocks and sprites are worked out.
static bool CaptureLineFromTemplate(Ppu *ppu, PpuFrame *f, PpuLineRec *rec, int line, int idx) {
  uint64_t *d = (uint64_t *)rec;
  const uint64_t *t = (const uint64_t *)&g_cap.tmpl;
#if defined(__mips__) && !defined(RIG)
  for (int i = 0; i < 4; i++) __asm__ volatile("cache 0x0d, %0" :: "m"(d[i * 2]));
#endif
  for (int i = 0; i < 8; i++) d[i] = t[i];
  f->lsig[idx] = 0;
  g_cap.captured[idx >> 5] |= 1u << (idx & 31);
  if (idx + 1 > f->numLines) f->numLines = idx + 1;
  bool m7 = g_cap.tmpl.flags & kLR_Mode7;
  rec->flags = m7 ? kLR_Valid | kLR_Mode7 : kLR_Valid;
  uint8_t en = ppu->screenEnabled[0] | ppu->screenEnabled[1];
  if (m7) {
    rec->block[0] = idx;
    if ((rec->screenEnabled[0] | rec->screenEnabled[1]) & 1) {
      Mode7Row(ppu, line, f->m7rows[idx], f->m7key[idx], f, idx);
      if (idx + 1 > f->numM7) f->numM7 = idx + 1;
    }
  }
  for (int layer = 0; layer < 3 && !m7; layer++) {
    LayerRows(&ppu->bgLayer[layer], line, &rec->bg[layer]);
    if (en & (1 << layer)) {
      uint16_t bi = LayerBlock(ppu, f, layer, &rec->bg[layer]);
      rec->block[layer] = bi;
      if (bi == PPUR_NO_BLOCK) {
        g_ppur_why[5]++;
        rec->flags = kLR_Valid | kLR_CpuDrawn;
        return false;
      }
    } else {
      rec->block[layer] = PPUR_NO_BLOCK;
    }
  }
  if (PPUR_MAX_SLIVERS - f->numSlivers < g_cap.lineTiles[(uint8_t)(line - 1)]) {
    f->overflow = true;
    rec->flags = kLR_Valid | kLR_CpuDrawn;
    return false;
  }
  if (!m7) {   // this line's BG3 row may differ from the template's
    rec->drawMask = g_cap.tmplDrawMask;
    rec->composeBits = g_cap.tmplComposeBits;
    SubBlack(ppu, rec);
  }
  PendLine(ppu, f, rec, line, idx);
  return true;
}

// Sprites and fingerprint of a captured RSP line are worked out later (PendFlush);
// its slivers get the room they can need now.
static void PendLine(Ppu *ppu, PpuFrame *f, PpuLineRec *rec, int line, int idx) {
  rec->sliverStart = f->numSlivers;
  rec->sliverCount = 0;
  f->numSlivers += g_cap.lineTiles[(uint8_t)(line - 1)];
  g_cap.pendIdx[g_cap.npend] = idx;
  g_cap.pendS0[g_cap.npend] = (ppu->objPriority ? (ppu->oamAdr & 0xfe) : 0) >> 1;
  g_cap.npend++;
}

static void PendFlush(void) {
  Ppu *ppu = g_cap.ppu;
  PpuFrame *f = g_cap.frame;
  for (int k = 0; k < g_cap.npend; k++) {
    int idx = g_cap.pendIdx[k];
    PpuLineRec *rec = &f->lines[idx];
    int max = g_cap.lineTiles[(uint8_t)idx];   // evaluated line = line - 1 = idx
    int n = 0;
    if (max) CAPSTAT_T(1, n = EvaluateSpritesToSlivers(ppu, idx, &f->slivers[rec->sliverStart], max, g_cap.pendS0[k]));
    rec->sliverCount = n;
    if (n) rec->flags |= kLR_HasSprites;
    CAPSTAT_T(4, f->lsig[idx] = LineSig(f, rec, (rec->flags & kLR_Mode7) ? f->m7key[idx] : NULL));
  }
  g_cap.npend = 0;
}

SNES_HOT bool PpuRsp_CaptureLine(Ppu *ppu, int line) {
  PpuFrame *f = g_cap.frame;
  int idx = line - 1;
  if (g_cap.ppu != ppu || idx < 0 || idx >= PPUR_MAX_LINES)
    return false;
  g_cap.linesStarted = true;
  PpuLineRec *rec = &f->lines[idx];
  if (g_cap.tmplValid && g_cap.tmplGen == g_ppu_regGen && !g_cap.cgramDirty && !g_cap.objDirty &&
      !g_cap.vramSnapped && !f->overflow)
    return CaptureLineFromTemplate(ppu, f, rec, line, idx);
  g_cap.tmplValid = false;          // (set again if this line is built in full)
  ZeroRec(rec);
  f->lsig[idx] = 0;                 // set when the line turns out RSP-drawn
  g_cap.captured[idx >> 5] |= 1u << (idx & 31);
  if (idx + 1 > f->numLines) f->numLines = idx + 1;

  if (ppu->forcedBlank) {
    rec->flags = kLR_Valid | kLR_ForcedBlank;
    return true;
  }
  // The RSP draws mode 1 without mosaic (and without VRAM changes since the
  // snapshot), and mode 7 without EXTBG or mosaic: for those the CPU works out
  // BG1's colour indices here and the RSP does the rest (sprites, windows,
  // colour math). Everything else is drawn by the CPU right now.
  bool m7 = ppu->mode == 7 && !ppu->m7extBg && !(ppu->mosaicEnabled & 1);
  if ((ppu->mode != 1 && !m7) || (ppu->mode == 1 && ((ppu->mosaicEnabled & 7) || g_cap.vramSnapped)) ||
      ppu->extraLeftCur || ppu->extraRightCur || f->overflow) {
    g_ppur_modes[(ppu->mode & 7) | ((ppu->mosaicEnabled & 7) ? 8 : 0)]++;
    g_ppur_why[ppu->mode != 1 ? 0 : (ppu->mosaicEnabled & 7) ? 1 : (ppu->extraLeftCur || ppu->extraRightCur) ? 2 : g_cap.vramSnapped ? 3 : 4]++;
    rec->flags = kLR_Valid | kLR_CpuDrawn;
    return false;
  }
  if (g_cap.cgramDirty) {
    if (f->numCgrams >= PPUR_MAX_CGRAMS) {
      rec->flags = kLR_Valid | kLR_CpuDrawn;
      return false;
    }
    memcpy(f->cgram[f->numCgrams++], ppu->cgram, sizeof(ppu->cgram));
    g_cap.cgramDirty = false;
  }
  if (g_cap.objDirty) {
    BuildSpriteBuckets(ppu);
    g_cap.objDirty = false;
  }

  rec->flags = kLR_Valid;
  rec->brightness = ppu->brightness;
  rec->screenEnabled[0] = ppu->screenEnabled[0];
  rec->screenEnabled[1] = ppu->screenEnabled[1];
  if (m7) {   // mode 7 has BG1 and sprites only
    rec->flags |= kLR_Mode7;
    rec->screenEnabled[0] &= 0x11;
    rec->screenEnabled[1] &= 0x11;
  }
  rec->screenWindowed[0] = ppu->screenWindowed[0];
  rec->screenWindowed[1] = ppu->screenWindowed[1];
  uint8_t m = 0;
  for (int i = 0; i < 6; i++) m |= ppu->mathEnabled[i] << i;
  rec->mathEnabled = m;
  rec->cgramIdx = f->numCgrams - 1;
  rec->w1l = ppu->window1left; rec->w1r = ppu->window1right;
  rec->w2l = ppu->window2left; rec->w2r = ppu->window2right;
  rec->clipMode = ppu->clipMode;
  rec->preventMathMode = ppu->preventMathMode;
  rec->mathFlags = (ppu->addSubscreen ? 1 : 0) | (ppu->halfColor ? 2 : 0) | (ppu->subtractColor ? 4 : 0);
  rec->fixedColor = ppu->fixedColorR | ppu->fixedColorG << 5 | ppu->fixedColorB << 10;
  rec->windowsel = ppu->windowsel;
  for (int layer = 0; layer < 3 && !m7; layer++) {
    LayerRows(&ppu->bgLayer[layer], line, &rec->bg[layer]);
    rec->block[layer] = ((ppu->screenEnabled[0] | ppu->screenEnabled[1]) & (1 << layer)) ?
        LayerBlock(ppu, f, layer, &rec->bg[layer]) : PPUR_NO_BLOCK;
  }
  // The RSP needs the colour window to be the same along the whole line when
  // it matters (clip or prevent-math mode uses it). Same rules as WinSetupFlags
  // in rsp_ppu.S. Otherwise the CPU draws the line.
  // A colour window that changes along the line only for the math (not the
  // clipping) is done by the RSP too: it clears the math bit of the main
  // pixels where the window rules math out (kCB_WinMath, Compose in rsp_ppu.S).
  bool winmath = false;
  if (rec->clipMode == 1 || rec->clipMode == 2 || ((rec->preventMathMode == 1 || rec->preventMathMode == 2) && rec->mathEnabled)) {
    unsigned wf = rec->windowsel >> 20;
    bool per_pixel = false, all = false;
    if (wf & 2) { if (rec->w1r < rec->w1l) all = wf & 1; else per_pixel = true; }
    if (!all && (wf & 8)) { if (rec->w2r < rec->w2l) all = wf & 4; else per_pixel = true; }
    if (per_pixel && !all) {
      if (rec->clipMode == 1 || rec->clipMode == 2) {
        g_ppur_why[5]++;
        rec->flags = kLR_Valid | kLR_CpuDrawn;
        return false;
      }
      winmath = true;
    }
  }
  if (m7) {
    rec->block[0] = idx;
    rec->block[1] = rec->block[2] = PPUR_NO_BLOCK;
    if ((rec->screenEnabled[0] | rec->screenEnabled[1]) & 1) {
      Mode7Row(ppu, line, f->m7rows[idx], f->m7key[idx], f, idx);
      if (idx + 1 > f->numM7) f->numM7 = idx + 1;
    }
  }
  // A layer without a tile block (block buffer full) can't be drawn by the RSP.
  for (int layer = 0; layer < 3 && !m7; layer++) {
    if (((ppu->screenEnabled[0] | ppu->screenEnabled[1]) & (1 << layer)) && rec->block[layer] == PPUR_NO_BLOCK) {
      g_ppur_why[5]++;
      rec->flags = kLR_Valid | kLR_CpuDrawn;
      return false;
    }
  }
  // Sprites (same condition as ppu_runLine: evaluated unless forced blank).
  if (PPUR_MAX_SLIVERS - f->numSlivers < g_cap.lineTiles[(uint8_t)(line - 1)]) {
    f->overflow = true;
    rec->flags = kLR_Valid | kLR_CpuDrawn;
    return false;
  }
  SetComposeBits(rec);
  if (winmath) {   // math wherever the window allows it (the RSP masks it per pixel)
    bool half = rec->mathFlags & 2, sub = (rec->drawMask & 0x40) != 0;
    rec->composeBits &= ~kCB_Math;
    if (rec->mathEnabled && (rec->fixedColor != 0 || half || sub)) rec->composeBits |= kCB_Math;
    rec->composeBits |= kCB_WinMath | (rec->preventMathMode == 2 ? kCB_WinMathOut : 0);
  }
  // Rare colour math the RSP's compose loop doesn't do: halving, and
  // subtraction combined with a brightness fade. The CPU draws those lines.
  if ((rec->composeBits & kCB_Math) &&
      ((rec->composeBits & kCB_Half) || ((rec->composeBits & kCB_Subtract) && rec->brightness != 15))) {
    g_ppur_why[5]++;
    rec->flags = kLR_Valid | kLR_CpuDrawn;
    return false;
  }
  g_cap.tmplDrawMask = rec->drawMask;
  g_cap.tmplComposeBits = rec->composeBits;
  SubBlack(ppu, rec);
  PendLine(ppu, f, rec, line, idx);
  g_cap.tmpl = *rec;
  g_cap.tmplGen = g_ppu_regGen;
  g_cap.tmplValid = true;
  return true;
}

// ---------------------------------------------------------------------------
// C reference renderer
// ---------------------------------------------------------------------------
typedef struct {
  int16_t edges[6];
  uint8_t nr;
  uint8_t bits;
} Windows;

enum { kWindow1Inversed = 1, kWindow1Enabled = 2, kWindow2Inversed = 4, kWindow2Enabled = 8 };

static void Windows_Clear(Windows *win) {
  win->edges[0] = 0;
  win->edges[1] = 256;
  win->nr = 1;
  win->bits = 0;
}

// Same algorithm as PpuWindows_Calc (from Snes9x).
static void Windows_Calc(Windows *win, const PpuLineRec *r, unsigned layer) {
  uint32_t winflags = r->windowsel >> (layer * 4);
  unsigned nr = 1;
  int window_right = 256;
  win->edges[0] = 0;
  win->edges[1] = window_right;
  unsigned i, j;
  int t;
  bool w1_ena = (winflags & kWindow1Enabled) && r->w1l <= r->w1r;
  if (w1_ena) {
    if (r->w1l > win->edges[0]) {
      win->edges[nr] = r->w1l;
      win->edges[++nr] = window_right;
    }
    if (r->w1r + 1 < window_right) {
      win->edges[nr] = r->w1r + 1;
      win->edges[++nr] = window_right;
    }
  }
  bool w2_ena = (winflags & kWindow2Enabled) && r->w2l <= r->w2r;
  if (w2_ena) {
    for (i = 0; i <= nr && (t = r->w2l) != win->edges[i]; i++) {
      if (t < win->edges[i]) {
        for (j = nr++; j >= i; j--)
          win->edges[j + 1] = win->edges[j];
        win->edges[i] = t;
        break;
      }
    }
    for (; i <= nr && (t = r->w2r + 1) != win->edges[i]; i++) {
      if (t < win->edges[i]) {
        for (j = nr++; j >= i; j--)
          win->edges[j + 1] = win->edges[j];
        win->edges[i] = t;
        break;
      }
    }
  }
  win->nr = nr;
  uint8_t w1_bits = 0, w2_bits = 0;
  if (w1_ena) {
    for (i = 0; win->edges[i] != r->w1l; i++);
    for (j = i; win->edges[j] != r->w1r + 1; j++);
    w1_bits = ((1 << (j - i)) - 1) << i;
  }
  if ((winflags & (kWindow1Enabled | kWindow1Inversed)) == (kWindow1Enabled | kWindow1Inversed))
    w1_bits = ~w1_bits;
  if (w2_ena) {
    for (i = 0; win->edges[i] != r->w2l; i++);
    for (j = i; win->edges[j] != r->w2r + 1; j++);
    w2_bits = ((1 << (j - i)) - 1) << i;
  }
  if ((winflags & (kWindow2Enabled | kWindow2Inversed)) == (kWindow2Enabled | kWindow2Inversed))
    w2_bits = ~w2_bits;
  win->bits = w1_bits | w2_bits;
}

static void LayerWindows(Windows *win, const PpuLineRec *r, int sub, int layer) {
  if (r->screenWindowed[sub] & (1 << layer)) Windows_Calc(win, r, layer); else Windows_Clear(win);
}

#if PPUR_REFERENCE   // C reference renderer (test builds): draws from the VRAM snapshot
static void ClearBuf(uint16_t *b) {
  for (int i = 0; i < 256; i++) b[i] = 0x0500;
}

// 4bpp and 2bpp background layer, mode 1. Same pixel order and z-tests as ppu.c.
static void DrawBg(const PpuLineRec *r, const uint16_t *vram, uint16_t *buf, int sub, int layer,
                   int bpp, uint16_t zhi, uint16_t zlo) {
  if (!(r->screenEnabled[sub] & (1 << layer)))
    return;
  Windows win;
  LayerWindows(&win, r, sub, layer);
  const PpuLayerRec *L = &r->bg[layer];
  const uint16_t *tps[2] = { &vram[L->tm0], &vram[L->tm1] };
  int pshift = bpp == 4 ? 6 : 8;
  for (int windex = 0; windex < win.nr; windex++) {
    if (win.bits & (1 << windex))
      continue;
    unsigned x = win.edges[windex] + L->hscroll;
    unsigned w = win.edges[windex + 1] - win.edges[windex];
    uint16_t *dstz = buf + win.edges[windex];
    // Walk pixels; the tile changes every 8 pixels of x.
    while (w) {
      const uint16_t *tp = tps[(x >> 8) & 1] + ((x >> 3) & 0x1f);
      uint32_t tile = *tp;
      int ta = (tile & 0x8000) ? L->ta1 : L->ta0;
      uint16_t z = (tile & 0x2000) ? zhi : zlo;
      uint32_t bits;
      if (bpp == 4) {
        const uint16_t *a = &vram[(ta + (tile & 0x3ff) * 16) & 0x7fff];
        bits = a[0] | a[8] << 16;
      } else {
        bits = vram[(ta + (tile & 0x3ff) * 8) & 0x7fff];
      }
      z += (tile & 0x1c00) >> pshift;
      int px = x & 7;
      int cnt = 8 - px;
      if ((unsigned)cnt > w) cnt = w;
      for (int k = 0; k < cnt; k++, px++, dstz++) {
        int i = (tile & 0x4000) ? 7 - px : px;   // bit index: h-flip reverses
        int pixel;
        if (bpp == 4)
          pixel = (bits >> (7 - i)) & 1 | (bits >> (14 - i)) & 2 | (bits >> (21 - i)) & 4 | (bits >> (28 - i)) & 8;
        else
          pixel = (bits >> (7 - i)) & 1 | (bits >> (14 - i)) & 2;
        if (pixel && z > dstz[0]) dstz[0] = z + pixel;
      }
      x += cnt;
      w -= cnt;
    }
  }
}

static void DrawSprites(const PpuLineRec *r, const uint16_t *obj, uint16_t *buf, int sub) {
  if (!(r->screenEnabled[sub] & (1 << 4)))
    return;
  Windows win;
  LayerWindows(&win, r, sub, 4);
  for (int windex = 0; windex < win.nr; windex++) {
    if (win.bits & (1 << windex))
      continue;
    for (int x = win.edges[windex]; x < win.edges[windex + 1]; x++)
      buf[x] = obj[x];   // mode 1: sprites are drawn first, onto the cleared backdrop
  }
}

static void BuildObjBuffer(const PpuFrame *f, const PpuLineRec *r, uint16_t *obj) {
  ClearBuf(obj);
  const PpuSliver *s = &f->slivers[r->sliverStart];
  for (int n = 0; n < r->sliverCount; n++, s++) {
    for (int px = 0; px < 8; px++) {
      int x = s->x + px;
      if (x < 0 || x > 255) continue;
      int sh = 7 - px;
      uint32_t p = s->planes >> sh;
      int pixel = (p & 1) | (p >> 7 & 2) | (p >> 14 & 4) | (p >> 21 & 8);
      if (pixel != 0 && (obj[x] & 0xff) == 0)
        obj[x] = s->z + pixel;
    }
  }
}

// Mode 7 (ppu.c order): BG1 over the backdrop, then sprites where they are higher.
static void DrawMode7(const PpuFrame *f, const PpuLineRec *r, const uint16_t *obj, uint16_t *buf, int sub) {
  Windows win;
  if (r->screenEnabled[sub] & 1) {
    const uint8_t *row = f->m7rows[r->block[0]];
    LayerWindows(&win, r, sub, 0);
    for (int windex = 0; windex < win.nr; windex++) {
      if (win.bits & (1 << windex))
        continue;
      for (int x = win.edges[windex]; x < win.edges[windex + 1]; x++)
        if (row[x]) buf[x] = 0x5000 + row[x];
    }
  }
  if (!(r->flags & kLR_HasSprites) || !(r->screenEnabled[sub] & 0x10))
    return;
  LayerWindows(&win, r, sub, 4);
  for (int windex = 0; windex < win.nr; windex++) {
    if (win.bits & (1 << windex))
      continue;
    for (int x = win.edges[windex]; x < win.edges[windex + 1]; x++)
      if (obj[x] > buf[x]) buf[x] = obj[x];
  }
}

static void DrawBackgrounds(const PpuFrame *f, const PpuLineRec *r, const uint16_t *obj, uint16_t *buf, int sub) {
  const uint16_t *vram = f->vram;
  if (r->flags & kLR_Mode7) {
    DrawMode7(f, r, obj, buf, sub);
    return;
  }
  if (r->flags & kLR_HasSprites)
    DrawSprites(r, obj, buf, sub);
  DrawBg(r, vram, buf, sub, 0, 4, 0xc000, 0x8000);
  DrawBg(r, vram, buf, sub, 1, 4, 0xb100, 0x7100);
  DrawBg(r, vram, buf, sub, 2, 2, 0xf200, 0x1200);
}

typedef struct PpuRspDebug { uint16_t obj[256], main[256], sub[256]; bool rendered_sub; } PpuRspDebug;
static void RenderLine(const PpuFrame *f, const PpuLineRec *r, uint16_t *dst, PpuRspDebug *dbg) {
  if (r->flags & kLR_ForcedBlank) {
    for (int i = 0; i < 256; i++) dst[i] = PPUR_PIX16(0, 0, 0);
    return;
  }
  uint8_t bmul[32 + 31], bmulHalf[64];
  for (int i = 0; i < 32; i++)
    bmulHalf[i * 2] = bmulHalf[i * 2 + 1] = bmul[i] = ((i << 3) | (i >> 2)) * r->brightness / 15;
  memset(&bmul[32], bmul[31], 31);

  const uint16_t *cgram = f->cgram[r->cgramIdx];
  uint16_t obj[256], main[256], subb[256];
  BuildObjBuffer(f, r, obj);

  ClearBuf(main);
  DrawBackgrounds(f, r, obj, main, 0);

  uint32_t math_enabled = r->mathEnabled;
  bool addSubscreen = r->mathFlags & 1, halfColor = r->mathFlags & 2, subtractColor = r->mathFlags & 4;
  bool rendered_subscreen = false;
  if (r->preventMathMode != 3 && addSubscreen && math_enabled) {
    ClearBuf(subb);
    if (r->screenEnabled[1] != 0) {
      DrawBackgrounds(f, r, obj, subb, 1);
      rendered_subscreen = true;
    }
  }

  if (dbg) {
    memcpy(dbg->obj, obj, sizeof(obj)); memcpy(dbg->main, main, sizeof(main));
    memcpy(dbg->sub, subb, sizeof(subb)); dbg->rendered_sub = rendered_subscreen;
  }
  Windows cwin;
  Windows_Calc(&cwin, r, 5);
  static const uint8_t kCwBitsMod[8] = { 0x00, 0xff, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00 };
  uint32_t cw_clip_math = ((cwin.bits & kCwBitsMod[r->clipMode]) ^ kCwBitsMod[r->clipMode + 4]) |
    ((cwin.bits & kCwBitsMod[r->preventMathMode]) ^ kCwBitsMod[r->preventMathMode + 4]) << 8;

  uint32_t windex = 0;
  do {
    uint32_t left = cwin.edges[windex], right = cwin.edges[windex + 1];
    uint32_t clip_color_mask = (cw_clip_math & 1) ? 0x1f : 0;
    uint32_t math_enabled_cur = (cw_clip_math & 0x100) ? math_enabled : 0;
    uint32_t fixed_color = r->fixedColor;
    if (math_enabled_cur == 0 || fixed_color == 0 && !halfColor && !rendered_subscreen) {
      for (uint32_t i = left; i < right; i++) {
        uint32_t color = cgram[main[i] & 0xff];
        dst[i] = PPUR_PIX16(bmul[color & clip_color_mask], bmul[(color >> 5) & clip_color_mask],
                            bmul[(color >> 10) & clip_color_mask]);
      }
    } else {
      const uint8_t *half_color_map = halfColor ? bmulHalf : bmul;
      math_enabled_cur |= addSubscreen << 8 | subtractColor << 9;
      for (uint32_t i = left; i < right; i++) {
        uint32_t color = cgram[main[i] & 0xff], color2;
        uint8_t main_layer = (main[i] >> 8) & 0xf;
        uint32_t rr = color & clip_color_mask;
        uint32_t g = (color >> 5) & clip_color_mask;
        uint32_t b = (color >> 10) & clip_color_mask;
        const uint8_t *color_map = bmul;
        if (math_enabled_cur & (1 << main_layer)) {
          if (math_enabled_cur & 0x100) {
            if ((subb[i] & 0xff) != 0)
              color2 = cgram[subb[i] & 0xff], color_map = half_color_map;
            else
              color2 = fixed_color;
          } else {
            color2 = fixed_color, color_map = half_color_map;
          }
          uint32_t r2 = (color2 & 0x1f), g2 = ((color2 >> 5) & 0x1f), b2 = ((color2 >> 10) & 0x1f);
          if (math_enabled_cur & 0x200) {
            rr = (rr >= r2) ? rr - r2 : 0;
            g = (g >= g2) ? g - g2 : 0;
            b = (b >= b2) ? b - b2 : 0;
          } else {
            rr += r2;
            g += g2;
            b += b2;
          }
        }
        dst[i] = PPUR_PIX16(color_map[rr], color_map[g], color_map[b]);
      }
    }
  } while (cw_clip_math >>= 1, ++windex < cwin.nr);
}

#endif
// The per-line decisions the RSP needs, worked out the same way RenderLine does.
// The colour window is constant along RSP lines wherever it matters.
static void SetComposeBits(PpuLineRec *r) {
  bool addSubscreen = r->mathFlags & 1, halfColor = r->mathFlags & 2, subtractColor = r->mathFlags & 4;
  bool sub_drawn = r->preventMathMode != 3 && addSubscreen && r->mathEnabled;
  bool rendered_sub = sub_drawn && r->screenEnabled[1] != 0;
  // bit 7: sprites only on the main screen and not windowed there, so the RSP
  // can build the sprite line straight into the main screen's buffer
  bool obj_direct = (r->screenEnabled[0] & 0x10) && !(r->screenWindowed[0] & 0x10) &&
                    !(rendered_sub && (r->screenEnabled[1] & 0x10));
  r->drawMask = (r->screenEnabled[0] & 7) | (sub_drawn ? (r->screenEnabled[1] & 7) << 3 : 0) |
                (rendered_sub ? 0x40 : 0) | (obj_direct ? 0x80 : 0);
  Windows cwin;
  Windows_Calc(&cwin, r, 5);
  static const uint8_t kCwBitsMod[8] = { 0x00, 0xff, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00 };
  uint32_t keep = ((cwin.bits & kCwBitsMod[r->clipMode]) ^ kCwBitsMod[r->clipMode + 4]) & 1;
  uint32_t math_on = ((cwin.bits & kCwBitsMod[r->preventMathMode]) ^ kCwBitsMod[r->preventMathMode + 4]) & 1;
  bool math = math_on && r->mathEnabled && (r->fixedColor != 0 || halfColor || rendered_sub);
  r->composeBits = (keep ? kCB_Keep : 0) | (addSubscreen && rendered_sub ? kCB_SubUsable : 0) |
                   (halfColor ? kCB_Half : 0) | (!addSubscreen ? kCB_HalfNoSub : 0) |
                   (subtractColor ? kCB_Subtract : 0) | (math ? kCB_Math : 0);
}

#if PPUR_REFERENCE
void PpuRsp_RenderFrameC(const PpuFrame *f, uint16_t *dst, int stride) {
  for (int idx = 0; idx < f->numLines; idx++) {
    const PpuLineRec *r = &f->lines[idx];
    if (!(r->flags & kLR_Valid) || (r->flags & kLR_CpuDrawn))
      continue;
    RenderLine(f, r, dst + idx * stride, NULL);
  }
}

// Test hook: render one line and also return the intermediate z-buffers.
void PpuRsp_RenderLineDebug(const PpuFrame *f, int idx, uint16_t *dst, void *dbg) {
  RenderLine(f, &f->lines[idx], dst, (PpuRspDebug *)dbg);
}

#endif
// A line's output is a function of its record (minus the indices into this
// frame's arrays), its CGRAM snapshot, its sprite slivers and, per BG layer,
// the 34 block columns it reads. A column's content is fixed by the tilemap
// entry it was built from and the write generation of its tile page at the
// time (bentry / bgen; generations only ever count up). If all of these equal
// the previous frame's, the previous frame's pixels for the line are already
// right. Mode 7, forced-blank and CPU-drawn lines are always drawn.
long g_ppur_cwhy[8];   // lines not copied, by reason: flags, record, cgram, slivers, no block, BG columns, mode 7 row, never copied
// n 64-bit words equal? (both 8-byte aligned)
static bool Equal64(const void *a, const void *b, int n) {
  const uint64_t *x = (const uint64_t *)a, *y = (const uint64_t *)b;
  uint64_t d = 0;
  for (int i = 0; i < n; i++) d |= x[i] ^ y[i];
  return d == 0;
}

int g_ppur_cf_mode;
unsigned g_ppur_cf_gen;
uint16_t (*g_ppur_cf_color)(uint16_t c), (*g_ppur_cf_fixed)(uint16_t c);

void PpuRsp_FilterPalette(PpurCfMemo *m, const uint16_t *src, uint16_t *dst) {
  if (m->gen != g_ppur_cf_gen + 1) {     // new filter (or first use): recompute all
    for (int i = 0; i < 256; i++) m->src[i] = src[i], m->dst[i] = g_ppur_cf_color(src[i] & 0x7fff);
    m->gen = g_ppur_cf_gen + 1;
  } else {
    for (int i = 0; i < 256; i++)
      if (m->src[i] != src[i]) m->src[i] = src[i], m->dst[i] = g_ppur_cf_color(src[i] & 0x7fff);
  }
  memcpy(dst, m->dst, 512);
}

void PpuRsp_ConvertCgrams(PpuFrame *f) {
  if (g_ppur_cf_mode && g_ppur_cf_color) {
    static PpurCfMemo memo;
    uint16_t tmp[256];
    for (int c = 0; c < f->numCgrams; c++) {
      PpuRsp_FilterPalette(&memo, f->cgram[c], tmp);
      for (int i = 0; i < 256; i++) f->cgram[c][i] = PpuRsp_Cg5551(tmp[i]);
    }
    uint16_t last_in = 0, last_out = 0;
    for (int l = 0; l < f->numLines && l < PPUR_MAX_LINES; l++) {
      uint16_t v = f->lines[l].fixedColor & 0x7fff;
      if (v != last_in) last_in = v, last_out = g_ppur_cf_fixed(v);
      f->lines[l].fixedColor = v ? last_out : 0;
    }
    return;
  }
  for (int c = 0; c < f->numCgrams; c++)
    for (int i = 0; i < 256; i++) f->cgram[c][i] = PpuRsp_Cg5551(f->cgram[c][i]);
}

void PpuRsp_CgramsToSnes(PpuFrame *f) {
  for (int c = 0; c < f->numCgrams; c++)
    for (int i = 0; i < 256; i++) {
      uint16_t v = f->cgram[c][i];
      f->cgram[c][i] = (uint16_t)((v >> 11) | (v >> 6 & 31) << 5 | (v >> 1 & 31) << 10);
    }
}

int PpuRsp_MarkCopies(PpuFrame *cur, const PpuFrame *prev) {
  if (!prev || prev->overflow || cur->overflow) return 0;
  int8_t cgeq[PPUR_MAX_CGRAMS][PPUR_MAX_CGRAMS];
  memset(cgeq, -1, sizeof(cgeq));
  int n = cur->numLines < prev->numLines ? cur->numLines : prev->numLines, copies = 0;
  const uint8_t skip = kLR_CpuDrawn | kLR_ForcedBlank;
  if (!g_recMask[0]) InitRecMask();
  struct { int bi, pbi, start; bool ok; } last[3];
  for (int k = 0; k < 3; k++) last[k].bi = -1;
  for (int l = 0; l < n; l++) {
    if (!cur->lsig[l] || cur->lsig[l] != prev->lsig[l]) { g_ppur_cwhy[1]++; continue; }
    PpuLineRec *r = &cur->lines[l];
    const PpuLineRec *p = &prev->lines[l];
    if (!(r->flags & kLR_Valid) || (r->flags & skip)) { g_ppur_cwhy[7]++; continue; }
    {   // the record as 8 words, without cgramIdx, sliverStart, block[] and pad
      const uint64_t *a = (const uint64_t *)r, *b = (const uint64_t *)p;
      uint64_t d = 0;
      for (int i = 0; i < 8; i++) d |= (a[i] ^ b[i]) & g_recMask[i];
      if (d) { g_ppur_cwhy[1]++; continue; }
    }
    int8_t *ce = &cgeq[r->cgramIdx][p->cgramIdx];
    if (*ce < 0) *ce = Equal64(cur->cgram[r->cgramIdx], prev->cgram[p->cgramIdx], 512 / 8);
    if (!*ce) { g_ppur_cwhy[2]++; continue; }
    if (r->sliverCount && !Equal64(&cur->slivers[r->sliverStart], &prev->slivers[p->sliverStart], r->sliverCount))
      { g_ppur_cwhy[3]++; continue; }
    bool ok = true;
    for (int k = 0; k < 3 && ok; k++) {
      if (!(((r->drawMask >> k) | (r->drawMask >> (k + 3))) & 1)) continue;
      uint16_t bi = r->block[k], pbi = p->block[k];
      if (bi == PPUR_NO_BLOCK || pbi == PPUR_NO_BLOCK) { ok = false; g_ppur_cwhy[4]++; break; }
      if (k == 0 && (r->flags & kLR_Mode7)) {   // BG1 is a mode 7 row of colour indices
        // equal keys (scroll / matrix inputs and the mode 7 VRAM generation) mean equal rows
        if (memcmp(cur->m7key[bi], prev->m7key[pbi], sizeof(cur->m7key[0]))) ok = false;
        if (!ok) g_ppur_cwhy[6]++;
        continue;
      }
      // The 34 columns this line reads: same tilemap entries, built from the
      // same write generation of their tile page. The records are equal, so
      // both lines start at the same column. Lines of a tile row share the
      // answer.
      int start = (r->bg[k].hscroll >> 3) & 63;
      if (last[k].bi == bi && last[k].pbi == pbi && last[k].start == start) { ok = last[k].ok; if (!ok) g_ppur_cwhy[5]++; continue; }
      const uint16_t *ce = cur->bentry[bi], *pe = prev->bentry[pbi];
      const uint32_t *cg = cur->bgen[bi], *pg = prev->bgen[pbi];
      for (int i = 0, c = start; i < 34; i++, c = (c + 1) & 63)
        if (ce[c] != pe[c] || cg[c] != pg[c]) { ok = false; break; }
      last[k].bi = bi, last[k].pbi = pbi, last[k].start = start, last[k].ok = ok;
      if (!ok) g_ppur_cwhy[5]++;
    }
    if (!ok) continue;
    r->flags |= kLR_Copy;
    copies++;
  }
  return copies;
}
