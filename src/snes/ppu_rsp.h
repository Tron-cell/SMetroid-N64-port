// Frame capture for the RSP renderer.
//
// Instead of drawing each scanline on the CPU, ppu_runLine() records what the
// line needs (PpuLineRec) and the visible sprite slivers. At the end of the
// frame the RSP (or PpuRsp_RenderFrameC, the plain C reference with identical
// output) draws all lines from this capture plus a snapshot of VRAM and CGRAM.
//
// Everything here is laid out for the RSP: big-endian on N64, 8-byte aligned
// blocks, halfword fields at even offsets.
#ifndef PPU_RSP_H
#define PPU_RSP_H

#include <stdint.h>
#include <stdbool.h>

// Test builds keep a VRAM snapshot per frame for the C reference renderer
// (PpuRsp_RenderFrameC); the RSP itself doesn't need it.
#if defined(RIG) || defined(RSP_SELFCHECK) || !defined(__mips__)
#define PPUR_REFERENCE 1
#else
#define PPUR_REFERENCE 0
#endif
#ifdef CAPSTAT
extern long g_cs[8]; extern unsigned long g_ct[10];
static inline unsigned long CsCyc(void) { unsigned long c; __asm__ volatile("mfc0 %0, $9" : "=r"(c)); return c; }
#define CAPSTAT_T(i, stmt) do { unsigned long _t0 = CsCyc(); stmt; g_ct[i] += CsCyc() - _t0; } while (0)
#else
#define CAPSTAT_T(i, stmt) do { stmt; } while (0)
#endif
#define PPUR_MAX_LINES 240        // visible lines 1..239 -> records 0..238
#define PPUR_MAX_SLIVERS 3072     // per frame (max 34 per line; ~2400 seen in tests; more falls back to the CPU)
#define PPUR_MAX_CGRAMS 16        // CGRAM snapshots per frame (palette changes mid-frame)
#define PPUR_DEP_PAGES 8          // per block: VRAM pages tracked with the columns that use them
#define PPUR_DEP_MANY 0xff
#define PPUR_MAX_BLOCKS 96        // tile blocks per frame (see PpuTileBlock); lines that don't get one are drawn by the CPU
#define PPUR_NO_BLOCK 0xffff

enum {
  kLR_ForcedBlank = 1,   // line is black
  kLR_CpuDrawn = 2,      // line was drawn by the CPU (mode 7): RSP must not touch it
  kLR_HasSprites = 4,    // sprite slivers present
  kLR_Valid = 8,         // record was written this frame
  kLR_Mode7 = 16,        // mode 7: BG1 comes from m7rows[block[0]] (pixel indices worked out by the CPU)
  kLR_Copy = 32,         // same output as this line in the previous frame: the RSP copies it from there
};

enum {                   // PpuLineRec.composeBits
  kCB_Keep = 1,          // colour window doesn't clip to black
  kCB_SubUsable = 2,     // addSubscreen and the sub screen was rendered
  kCB_Half = 4,          // halfColor
  kCB_HalfNoSub = 8,     // !addSubscreen (halving also applies with the fixed colour)
  kCB_Subtract = 16,
  kCB_Math = 32,         // some colour math can happen on this line
  kCB_WinMath = 64,      // colour math only inside the colour window (it changes along the line):
                         // the RSP clears the math bit (z 0x0800) of the main pixels outside it
  kCB_WinMathOut = 128,  // ... only outside it (with kCB_WinMath)
};

typedef struct PpuLayerRec {   // 10 bytes per BG layer
  uint16_t tm0, tm1;          // VRAM word address of the 32-entry tilemap row in screen 0 / 1
  uint16_t ta0, ta1;          // tile base + row (normal) / tile base + 7 - row (v-flipped)
  uint16_t hscroll;
} PpuLayerRec;

// One BG layer's whole tile row (both 32-column screens, 64 columns), built by
// the CPU when the layer's tile row changes (normally every 8 lines). A line
// uses 34 columns from (hscroll >> 3) on, so lines that only change hscroll
// (wave effects) share the block; columns are filled the first time a line
// needs them. Tiles are stored as they appear on screen (flips already
// applied), so the RSP always reads row (y & 7); it fetches just that row's
// quarter of each tile with strided DMA. z[64..97] repeats z[0..33] so the
// RSP can read 34 z values from any column without wrapping.
typedef struct PpuTileBlock {   // 2248 bytes
  uint16_t z[100];              // biased z (C value ^ 0x8000) incl. palette, per column
  uint8_t tiles[64][32];        // 4bpp: nibble per pixel, rows 0-7 pixels 0-3, then rows 0-7 pixels 4-7; 2bpp: rows 0-7 planes 0/1
} PpuTileBlock;

typedef struct PpuLineRec {     // 64 bytes
  uint8_t flags;
  uint8_t brightness;
  uint8_t screenEnabled[2];
  uint8_t screenWindowed[2];
  uint8_t mathEnabled;          // bits 0..5
  uint8_t cgramIdx;             // which CGRAM snapshot this line uses
  uint8_t w1l, w1r, w2l, w2r;   // window 1/2 left/right
  uint8_t clipMode, preventMathMode;
  uint8_t mathFlags;            // bit0 addSubscreen, bit1 halfColor, bit2 subtractColor
  uint8_t sliverCount;
  uint16_t fixedColor;          // 15-bit BGR
  uint16_t sliverStart;         // index into PpuFrame.slivers
  uint32_t windowsel;           // 4 bits per layer 0..5
  PpuLayerRec bg[3];            // 30 bytes
  uint16_t block[3];            // tile block per layer, or PPUR_NO_BLOCK
  // Decisions made once by the CPU for the RSP (the C reference renderer
  // works them out again from the fields above, so it checks these too):
  uint8_t drawMask;             // bits 0-2 main BG1-3, bits 3-5 sub BG1-3, bit 6 sub screen rendered, bit 7 sprites straight into the main buffer (RSP)
  uint8_t composeBits;          // see kCB_*
  uint8_t pad[2];
} PpuLineRec;

typedef struct PpuSliver {      // 8 bytes: one 8-pixel row of a sprite tile
  int16_t x;                    // screen x of the leftmost pixel (may be negative)
  uint16_t z;                   // priority << 8 | palette base (0x80 + 16 * pal)
  uint32_t planes;              // plane0 | plane1 << 8 | plane2 << 16 | plane3 << 24, already h-flipped,
                                // bit 7 of each plane byte = leftmost pixel
} PpuSliver;

// 16-byte aligned (and so a multiple of 16 in size) so that every frame of an
// array keeps the RSP DMA alignment: the RSP drops the low 3 address bits.
typedef struct __attribute__((aligned(16))) PpuFrame {
  PpuLineRec lines[PPUR_MAX_LINES];
  uint16_t cgram[PPUR_MAX_CGRAMS][256];
  PpuSliver slivers[PPUR_MAX_SLIVERS];
#if PPUR_REFERENCE
  uint16_t vram[0x8000];        // snapshot at the end of the frame (C reference only)
#endif
  PpuTileBlock blocks[PPUR_MAX_BLOCKS];
  // Block bookkeeping. Blocks are kept from one use of this frame buffer to the
  // next and revalidated column by column (tilemap entry and tile page unchanged).
  uint32_t bkey[PPUR_MAX_BLOCKS];        // layer, tilemap rows and tile base
  uint64_t bfilled[PPUR_MAX_BLOCKS];     // columns holding data
  uint64_t bchecked[PPUR_MAX_BLOCKS];    // columns validated in frame bstamp
  uint64_t bsusp[PPUR_MAX_BLOCKS];       // columns holding data not checked since a page they may use was written
  uint8_t bhash[256];                    // block index + 1 by key hash (open addressing), 0 = empty
  uint32_t bstamp[PPUR_MAX_BLOCKS];
  uint8_t bwritten[PPUR_MAX_BLOCKS];     // changed this frame: needs a cache writeback
  uint16_t bentry[PPUR_MAX_BLOCKS][64];  // tilemap entry each column was built from
  uint32_t bgen[PPUR_MAX_BLOCKS][64];    // and its tile page's write generation
  uint32_t bdeps[PPUR_MAX_BLOCKS][8];    // VRAM pages the filled columns were built from
  uint8_t bdpn[PPUR_MAX_BLOCKS];         // entries in bdpage / bdmask, PPUR_DEP_MANY when they didn't fit
  uint8_t bdpage[PPUR_MAX_BLOCKS][PPUR_DEP_PAGES];   // those pages (low 8 bits of the page number),
  uint64_t bdmask[PPUR_MAX_BLOCKS][PPUR_DEP_PAGES];  // and which columns use each
  uint32_t bvalid[PPUR_MAX_BLOCKS];      // stamp of the frame all filled columns were last checked
  void *bppu;
  uint32_t blastStamp;                   // stamp of this buffer's previous frame
  uint8_t m7rows[PPUR_MAX_LINES][256] __attribute__((aligned(8)));   // mode 7 lines: BG1 colour index per pixel (0 = transparent)
  uint64_t lsig[PPUR_MAX_LINES];         // per RSP line: fingerprint of record, slivers and mode 7 row (PpuRsp_MarkCopies)
  uint32_t m7key[PPUR_MAX_LINES][4];     // what each row was computed from (kept between frames: unchanged rows are reused)
  uint16_t m7job[PPUR_MAX_LINES][4] __attribute__((aligned(8)));   // rows the RSP builds (g_ppur_m7_rsp): index, Y, X0, 0
  int numM7Jobs;
  int numM7;
  int numBlocks;
  int numLines;                 // highest line index used + 1
  int numCgrams;
  int numSlivers;
  bool overflow;                // capture overflowed: frame must be drawn by the CPU
} PpuFrame;

typedef struct Ppu Ppu;

// Unrotated, unscaled mode 7 rows are listed in m7job for the RSP
// (rsp_m7.S) instead of being built by the CPU. Off unless the platform sets it.
extern bool g_ppur_m7_rsp;
// Mode 7 VRAM as two byte arrays (16 KB each): what rsp_m7.S reads.
extern uint8_t g_m7map[0x4000], g_m7pix[0x4000];
// Set when g_m7map / g_m7pix changed (the platform writes them back from the
// cache and clears it). The platform sets g_ppur_m7_busy while rsp_m7.S runs:
// they aren't changed then.
extern volatile bool g_ppur_m7_dirty, g_ppur_m7_busy;
uint32_t PpuRsp_M7Gen(void);   // changes whenever g_m7map / g_m7pix do

// Capture control. When a frame is set, ppu_runLine() records instead of drawing
// (except mode-7 lines, which it still draws itself and flags kLR_CpuDrawn).
void PpuRsp_BeginFrame(Ppu *ppu, PpuFrame *frame);
void PpuRsp_EndFrame(Ppu *ppu);          // snapshots VRAM; frame is complete
bool PpuRsp_CaptureLine(Ppu *ppu, int line);  // returns false if the CPU must draw this line
void PpuRsp_NoteCgramWrite(Ppu *ppu);    // called from ppu_write on CGRAM changes
void PpuRsp_NoteObjChange(Ppu *ppu);     // OAM / OBSEL changed: rebuild sprite buckets
void PpuRsp_MarkAllVramDirty(void);      // VRAM replaced wholesale (e.g. a snapshot restore)
extern uint32_t g_ppur_vdirty[8];        // VRAM pages written since the last frame snapshot
extern uint32_t g_ppur_m7dirty[8];       // VRAM pages written since the mode 7 byte copies were refreshed
extern uint32_t g_ppur_pagegen[256];     // per VRAM page: write count (tile blocks compare it)
extern uint32_t g_ppur_pagestamp[256];   // per VRAM page: frame stamp of the last write
extern uint32_t g_ppur_stamp;            // frame stamp (counts captured frames)
// Flags (kLR_Copy) the lines of cur whose output must equal the same line of
// prev, the previous captured frame (NULL: none). Returns the number flagged.
int PpuRsp_MarkCopies(PpuFrame *cur, const PpuFrame *prev);
// The RSP takes its palettes as RGBA5551 (r << 11 | g << 6 | b << 1 | 1): the
// N64 build converts a finished frame's CGRAM snapshots in place, before
// PpuRsp_MarkCopies (which then compares converted with converted). The C
// reference renderer wants them back as SNES colours (PpuRsp_CgramsToSnes).
void PpuRsp_ConvertCgrams(PpuFrame *f);
// Colour filter (N64: L + C-up cycles): when g_ppur_cf_mode != 0, colours go through
// g_ppur_cf_color (SNES BGR15 -> BGR15) and colour-math fixed colours through
// g_ppur_cf_fixed, both in PpuRsp_ConvertCgrams and for lines the CPU draws (ppu.c).
// g_ppur_cf_gen changes with the filter (memos keyed on it).
extern int g_ppur_cf_mode;
extern unsigned g_ppur_cf_gen;
extern uint16_t (*g_ppur_cf_color)(uint16_t c), (*g_ppur_cf_fixed)(uint16_t c);
// Filtered copy of a 256-colour palette; only entries that changed since the last call
// with the same memo are recomputed (memo: 2 x 256 entries + generation, zeroed at first).
typedef struct { uint16_t src[256], dst[256]; unsigned gen; } PpurCfMemo;
void PpuRsp_FilterPalette(PpurCfMemo *m, const uint16_t *src, uint16_t *dst);
void PpuRsp_CgramsToSnes(PpuFrame *f);
static inline uint16_t PpuRsp_Cg5551(uint16_t c) { return (uint16_t)((c & 31) << 11 | (c >> 5 & 31) << 6 | (c >> 10 & 31) << 1 | 1); }
#define PPUR_MARK_VRAM_DIRTY(a) (g_ppur_vdirty[((a) >> 12) & 7] |= 1u << (((a) >> 7) & 31), \
                                 g_ppur_m7dirty[((a) >> 12) & 7] |= 1u << (((a) >> 7) & 31), \
                                 g_ppur_pagegen[((a) >> 7) & 255]++, \
                                 g_ppur_pagestamp[((a) >> 7) & 255] = g_ppur_stamp)

// Plain C reference renderer: draws the captured frame as RGBA5551 into
// dst (stride in pixels), lines y=1.. into rows 0... Skips kLR_CpuDrawn lines.
void PpuRsp_RenderFrameC(const PpuFrame *frame, uint16_t *dst, int stride);

// RGBA5551 from 5-bit channels after brightness (r,g,b are 8-bit brightness-mult values).
#define PPUR_PIX16(r8, g8, b8) (uint16_t)((((r8) >> 3) << 11) | (((g8) >> 3) << 6) | (((b8) >> 3) << 1) | 1)

#endif
