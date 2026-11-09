// PC test for sprites in 3D (lowpoly.c: LpBuildSprite / LpDrawSprite), on
// graphics taken straight from your ROM:
//  * the player: a few poses (spritemaps, the tiles the NMI would copy, the
//    power suit palette), as sm_90.c hands them over;
//  * opponents: a few enemies (their tile set, palette and first animation
//    frame), as sm_a0.c hands them over (LowPoly_BeginEnemy / NoteEnemyOam /
//    EndEnemy), with the tiles in VRAM;
//  * a crowded screen, to check the shared texture budget.
// It runs the same code as the N64 build and rasterizes its texture rectangles
// into two images: each column is the 2D sprite, then the 3D version turned one way
// and the other.
//
// Build and run (Linux / WSL, from the project folder):
//   g++ -x c++ -std=gnu++20 -fpermissive -w -O1 -DLP_HOST_TEST -iquote src \
//       tools/lowpoly/sprite3d_test.c -o /tmp/sprite3d_test -lm
//   /tmp/sprite3d_test filesystem/sm.smc hero.ppm enemies.ppm
// The images hold parts of your ROM: don't share them.
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ---- what lowpoly.c needs from libdragon / n64_main.c ----
#define SCREEN_W 320
#define SCREEN_H 240
#define SNES_W 256
#define SNES_H 224
#define SNES_LEFT ((SCREEN_W - SNES_W) / 2)
#define SNES_TOP ((SCREEN_H - SNES_H) / 2)
typedef struct { void *buffer; } surface_t;
#include "snes/ppu_rsp.h"
uint32_t g_ppur_pagegen[256];
static inline uint32_t CfRGBA(uint32_t c) { return c; }
#define PhysicalAddr(p) ((uint32_t)(uintptr_t)(p))
#define malloc_uncached_aligned(a, n) aligned_alloc(16, ((n) + 15) & ~15)
#define assertf(c, ...) do { if (!(c)) { fprintf(stderr, __VA_ARGS__); abort(); } } while (0)
static void display_show(surface_t *) {}
static void register_DP_handler(void (*)(void)) {}
static void set_DP_interrupt(int) {}
static uint64_t get_ticks_us(void) { return 0; }
static void disable_interrupts(void) {}
static void enable_interrupts(void) {}
static void data_cache_hit_writeback(void *, unsigned) {}
#define debugf(...) fprintf(stderr, __VA_ARGS__)
static int g_ppur_cf_mode = 0;

uint8_t g_ram[0x20000];
static uint16_t g_vram[0x8000];
const uint16_t *Snes_N64_Vram(void) { return g_vram; }
// BG layout for the boss test: BG1 map 0x4400, BG2 map 0x4000, both 32 x 32, tiles at 0.
int Snes_N64_BgLayout(int layer, uint16_t *tilemap, uint16_t *tiles, int *wtiles, int *htiles, bool *bigTiles) {
  *tilemap = layer == 1 ? 0x4000 : 0x4400, *tiles = 0, *wtiles = *htiles = 32, *bigTiles = false;
  return 1;
}

#include "platform/n64/lowpoly.c"
static bool FxGameplay(void) { return true; }

// ---- rasterizer for the triangles ----
static uint32_t g_fb[SCREEN_H][SCREEN_W], g_col;
static int g_sc[4] = { 0, 0, SCREEN_W, SCREEN_H };
static const uint8_t *g_tex;            // CI8 texture band and the TLUT, as the RDP would have them
static uint16_t g_tlut[256];
static int g_tw, g_th;
static long g_words;                    // RDP command words written
static bool g_ttex;                     // a texture rectangle's second word comes next
static void LpHostTex(const uint8_t *tex, int w, int h) { g_tex = tex, g_tw = w, g_th = h; }
static void LpHostTlut(int idx, const uint16_t *src, int n) { memcpy(&g_tlut[idx], src, n * 2); }
static void LpHostCmd(uint64_t c) {
  g_words++;
  if (g_ttex) { g_ttex = false; return; }
  int op = (int)(c >> 56) & 0x3f;
  if (op == 0x24) g_ttex = true;
  if (op == 0x3A) g_col = (uint32_t)c;   // prim colour RGBA
  if (op == 0x2D) {                      // scissor (10.2)
    g_sc[0] = (int)(c >> 44 & 0xfff) / 4, g_sc[1] = (int)(c >> 32 & 0xfff) / 4;
    g_sc[2] = (int)(c >> 12 & 0xfff) / 4, g_sc[3] = (int)(c & 0xfff) / 4;
  }
}
static void LpHostTri(const LpVert *a, const LpVert *b, const LpVert *c, bool tex) {
  float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
  if (area == 0) return;
  int x0 = (int)floorf(fminf(a->x, fminf(b->x, c->x))), x1 = (int)ceilf(fmaxf(a->x, fmaxf(b->x, c->x)));
  int y0 = (int)floorf(fminf(a->y, fminf(b->y, c->y))), y1 = (int)ceilf(fmaxf(a->y, fmaxf(b->y, c->y)));
  if (x0 < g_sc[0]) x0 = g_sc[0];
  if (y0 < g_sc[1]) y0 = g_sc[1];
  if (x1 > g_sc[2]) x1 = g_sc[2];
  if (y1 > g_sc[3]) y1 = g_sc[3];
  for (int y = y0; y < y1; y++)
    for (int x = x0; x < x1; x++) {
      float px = x + 0.5f, py = y + 0.5f;
      float w0 = (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
      float w1 = (c->x - b->x) * (py - b->y) - (c->y - b->y) * (px - b->x);
      float w2 = (a->x - c->x) * (py - c->y) - (a->y - c->y) * (px - c->x);
      if (area < 0) w0 = -w0, w1 = -w1, w2 = -w2;
      if (w0 < 0 || w1 < 0 || w2 < 0) continue;
      if (!tex) { g_fb[y][x] = g_col >> 8; continue; }
      // w0 is opposite c, w1 opposite a, w2 opposite b (all scaled by |area|)
      float A = fabsf(area), la = w1 / A, lb = w2 / A, lc = w0 / A;
      int s = (int)floorf(la * a->s + lb * b->s + lc * c->s), t = (int)floorf(la * a->t + lb * b->t + lc * c->t);
      if (s < 0) s = 0;
      if (t < 0) t = 0;
      if (s >= g_tw) s = g_tw - 1;
      if (t >= g_th) t = g_th - 1;
      uint16_t tx = g_tlut[g_tex[t * g_tw + s]];
      int r = (tx >> 11 & 31) << 3, g = (tx >> 6 & 31) << 3, bl = (tx >> 1 & 31) << 3, k = g_col >> 24;
      g_fb[y][x] = (r * k / 255) << 16 | (g * k / 255) << 8 | (bl * k / 255);   // TEX0 * PRIM (grey)
    }
}
// Texture rectangle (1-cycle, 1:1, point sampled): colour TEX0 * PRIM, texels
// whose TLUT colour has alpha 0 not drawn (alpha compare).
static void LpHostRect(int x0, int y0, int x1, int y1, int s, int t) {
  int px0 = x0 / 4, py0 = y0 / 4, px1 = x1 / 4, py1 = y1 / 4, k = g_col >> 24;
  for (int y = py0 < g_sc[1] ? g_sc[1] : py0; y < py1 && y < g_sc[3]; y++)
    for (int x = px0 < g_sc[0] ? g_sc[0] : px0; x < px1 && x < g_sc[2]; x++) {
      int ss = s / 32 + (x - px0), tt = t / 32 + (y - py0);
      if (ss < 0 || tt < 0 || ss >= g_tw || tt >= g_th) continue;
      uint16_t tx = g_tlut[g_tex[tt * g_tw + ss]];
      if (!(tx & 1)) continue;
      int r = (tx >> 11 & 31) << 3, g = (tx >> 6 & 31) << 3, bl = (tx >> 1 & 31) << 3;
      g_fb[y][x] = (r * k / 255) << 16 | (g * k / 255) << 8 | (bl * k / 255);
    }
}

// ---- the ROM ----
static uint8_t *g_rom;
static const uint8_t *R(uint32_t a) { return &g_rom[(((a >> 16) << 15) | (a & 0x7fff)) & 0x3fffff]; }
static uint16_t W(uint32_t a) { const uint8_t *p = R(a); return p[0] | p[1] << 8; }
static void SetPalette(uint32_t src) {   // every sprite palette (CGRAM 128-255)
  for (int p = 8; p < 16; p++)
    for (int i = 0; i < 16; i++) {
      uint16_t c = W(src + 2 * i);
      g_ram[0xC000 + 2 * (p * 16 + i)] = c & 0xff, g_ram[0xC001 + 2 * (p * 16 + i)] = c >> 8;
    }
}

// A spritemap as the game writes it to OAM (base tile r0, palette bits r3),
// then read back as sm_90.c / sm_a0.c do. enemy: through the opponent calls.
static bool Spritemap(uint32_t addr, int xp, int yp, int r0, int r3, bool enemy) {
  const uint8_t *pp = R(addr);
  int n = pp[0] | pp[1] << 8;
  if (n == 0 || n > 64) return false;   // (also: 0xFFFE, a BG tilemap)
  pp += 2;
  for (; n; n--, pp += 5) {
    uint16_t xw = pp[0] | pp[1] << 8;
    int x = (xp + xw) & 0x1ff, y = (yp + pp[2]) & 0xff;
    if (x >= 256) x -= 512;
    if (y >= 0xE0) y -= 256;
    uint16_t cw = (uint16_t)(r3 | (r0 + (pp[3] | pp[4] << 8)));
    int ch = cw & 0xff, fl = cw >> 8;
    if (enemy) LowPoly_NoteEnemyOam(x, y, ch | (fl & 1) << 8, fl, (int16_t)xw < 0);
    else LowPoly_NoteHeroOam(x, y, ch | (fl & 1) << 8, fl, (int16_t)xw < 0);
  }
  return true;
}

// ---- the player ----
static void Half(uint16_t src, int first) {
  const uint8_t *td = R(0x920000 | src);
  uint32_t a = (uint32_t)td[2] << 16 | td[0] | td[1] << 8;
  int p1 = td[3] | td[4] << 8, p2 = td[5] | td[6] << 8;
  LowPoly_NoteHeroTiles(first, R(a), p1);
  if (p2) LowPoly_NoteHeroTiles(first + 0x10, R(a) + p1, p2);
}
static void HeroPose(int pose, int frame, int x, int y) {
  uint16_t top = W(0x92808d + 2 * (W(0x929263 + 2 * pose) + frame)), bot = W(0x92808d + 2 * (W(0x92945d + 2 * pose) + frame));
  if (top) Spritemap(0x920000 | top, x, y, 0, 0, false);
  if (bot) Spritemap(0x920000 | bot, x, y, 0, 0, false);
  const uint8_t *ad = R(0x920000 | (uint16_t)(4 * frame + W(0x92d94e + 2 * pose)));
  Half((uint16_t)(7 * ad[1] + W(0x92d91e + 2 * ad[0])), 0x00);
  if (ad[2] != 255) Half((uint16_t)(7 * ad[3] + W(0x92d938 + 2 * ad[2])), 0x08);
}

// ---- opponents ----
typedef struct { const char *name; uint8_t bank; uint16_t init, ilist; } EnemyCase;
static const EnemyCase kEnemies[] = {
  { "Skree", 0xA3, 0xC6AE, 0xC65E }, { "Waver", 0xA3, 0x86ED, 0x86A7 }, { "Rio", 0xA2, 0xBBCD, 0xBB4B },
  { "Fireflea", 0xA3, 0x8D2D, 0x8C2F }, { "Metroid", 0xA3, 0xEA4F, 0xE9CF }, { "Metalee", 0xA3, 0x8960, 0x8910 },
  { "Kago", 0xA8, 0xAB46, 0xAB1E }, { "WS robot", 0xA8, 0xCB77, 0xC6E5 }, { "Puffer", 0xA8, 0xD8C9, 0xD841 },
};
// The enemy's definition (bank A0, 64 bytes each): found by its AI bank and init.
static uint32_t FindDef(const EnemyCase *e) {
  for (uint32_t a = 0xA0CEBF; a < 0xA0F7FF; a += 0x40)
    if (R(a)[12] == e->bank && W(a + 18) == e->init) return a;
  return 0;
}
// Loads its tiles at character 0x100 (VRAM word 0x7000) and its palette; the
// first spritemap of its instruction list. 0: not found.
static uint32_t LoadEnemy(const EnemyCase *e) {
  uint32_t d = FindDef(e);
  if (!d) return 0;
  int size = W(d) & 0x7fff;
  const uint8_t *td = R(d + 54);
  uint32_t src = (uint32_t)td[2] << 16 | td[0] | td[1] << 8;
  memset(g_vram, 0, sizeof(g_vram));
  for (int i = 0; i < size / 2 && 0x7000 + i < 0x8000; i++) g_vram[0x7000 + i] = R(src + 2 * i)[0] | R(src + 2 * i)[1] << 8;
  SetPalette((uint32_t)e->bank << 16 | W(d + 2));
  uint32_t il = (uint32_t)e->bank << 16 | e->ilist;
  for (int k = 0; k < 8; k++, il += 2)   // skip instructions (>= 0x8000) to the first timer / spritemap pair
    if (W(il) < 0x8000 && W(il) != 0) return (uint32_t)e->bank << 16 | W(il + 2);
  return 0;
}
static bool NoteEnemy(uint32_t sm, int x, int y) {
  if (!LowPoly_BeginEnemy()) return false;
  if (!Spritemap(sm, x, y, 0, 0, true)) return false;   // (its characters already count from 0x100: base tile 0)
  return LowPoly_EndEnemy();
}

// ---- pictures ----
static void Clear(void) {
  for (int y = 0; y < SCREEN_H; y++)
    for (int x = 0; x < SCREEN_W; x++) g_fb[y][x] = ((x / 4 + y / 4) & 1) ? 0x202830 : 0x283038;
}
static void Draw2D(const LpFrame *lf, int i) {   // the decoded texture, flat
  const LpObj *o = &lf->obj[i];
  const uint8_t *t = lf->tex + o->tex;
  for (int y = 0; y < o->gh; y++)
    for (int x = 0; x < o->gw; x++) {
      uint16_t c = lf->ctab[i][t[y * o->tw + x] & 63];
      if (c) g_fb[SNES_TOP + o->gy + y][SNES_LEFT + o->gx + x] = (c >> 11 & 31) << 19 | (c >> 6 & 31) << 11 | (c >> 1 & 31) << 3;
    }
}
typedef struct { int w, h; uint8_t *px; } Img;
static void Blit(Img *im, int col, int row, int cw, int ch, int S, int cx, int cy) {   // crop around (cx, cy), scaled
  for (int y = 0; y < ch * S; y++)
    for (int x = 0; x < cw * S; x++) {
      int sx = SNES_LEFT + cx - cw / 2 + x / S, sy = SNES_TOP + cy - ch / 2 + y / S;
      uint32_t c = (sx >= 0 && sy >= 0 && sx < SCREEN_W && sy < SCREEN_H) ? g_fb[sy][sx] : 0;
      uint8_t *d = &im->px[((row * ch * S + y) * im->w + col * cw * S + x) * 3];
      d[0] = c >> 16, d[1] = c >> 8, d[2] = c;
    }
}
static void Save(const Img *im, const char *path) {
  FILE *o = fopen(path, "wb");
  fprintf(o, "P6\n%d %d\n255\n", im->w, im->h);
  fwrite(im->px, 1, im->w * im->h * 3, o);
  fclose(o);
}

// ---- a boss on BG2 ----
// The WS robot's tiles as BG characters and its spritemap as a BG2 tilemap (a
// stand-in for Kraid & co., whose tilemaps the game builds at run time), with
// a BG1 floor row of priority 0 over its lower part and a priority-3 sprite
// over it: these must come out in front of the 3D boss. Then one tile is
// changed: the canvas updated tile by tile must equal one built from scratch.
static PpuFrame g_fr;
static void Records(int h2, int v2, int h1, int v1) {   // every line: BG1 + BG2, one scroll each
  g_fr.numLines = SNES_H;
  for (int l = 0; l < SNES_H; l++) {
    PpuLineRec *r = &g_fr.lines[l];
    memset(r, 0, sizeof(*r));
    r->flags = kLR_Valid, r->drawMask = 3;
    unsigned y2 = l + 1 + v2, y1 = l + 1 + v1;
    r->bg[1].tm0 = 0x4000 + (((y2 >> 3) & 31) << 5), r->bg[1].ta0 = y2 & 7, r->bg[1].hscroll = h2;
    r->bg[0].tm0 = 0x4400 + (((y1 >> 3) & 31) << 5), r->bg[0].ta0 = y1 & 7, r->bg[0].hscroll = h1;
  }
}
static void BossTest(const char *path) {
  LpFrame *lf = &g_lp_frames[0];
  uint32_t sm = LoadEnemy(&kEnemies[7]);
  if (!sm) { printf("boss test: no tiles\n"); return; }
  memcpy(g_vram, g_vram + 0x7000, 0x1000 * 2);           // its tiles as BG characters 0.. (and still sprites at 0x7000)
  for (int i = 0; i < 128; i++) g_ram[0xC000 + 2 * i] = g_ram[0xC100 + 2 * i], g_ram[0xC001 + 2 * i] = g_ram[0xC101 + 2 * i];
  const int E = 0x1F0, F = 0x1F1;                       // an empty character and a floor character
  memset(&g_vram[E * 16], 0, 32);
  for (int i = 0; i < 16; i++) g_vram[F * 16 + i] = (i & 1) ? 0x0000 : 0xffff;   // stripes, colour 3
  for (int i = 0; i < 1024; i++) g_vram[0x4000 + i] = E, g_vram[0x4400 + i] = E;
  const uint8_t *pp = R(sm);
  int n = pp[0] | pp[1] << 8;
  for (pp += 2; n > 0; n--, pp += 5) {
    int xw = pp[0] | pp[1] << 8, x = (xw & 0x100) ? (xw & 0x1ff) - 0x200 : (xw & 0x1ff), y = (int8_t)pp[2];
    int cw = pp[3] | pp[4] << 8, c = (cw & 0x1ff) - 0x100, fl = cw >> 8, sz = (xw & 0x8000) ? 2 : 1;
    bool hf = fl & 0x40, vf = fl & 0x80;
    for (int qy = 0; qy < sz; qy++)
      for (int qx = 0; qx < sz; qx++) {
        int tx = 10 + ((x + 128) >> 3) - 16 + qx, ty = 14 + ((y + 128) >> 3) - 16 + qy;
        int ch = c + (hf ? sz - 1 - qx : qx) + 16 * (vf ? sz - 1 - qy : qy);
        g_vram[0x4000 + (ty & 31) * 32 + (tx & 31)] = (uint16_t)((ch & 0x3ff) | (fl >> 1 & 7) << 10 | (hf ? 0x4000 : 0) | (vf ? 0x8000 : 0));
      }
  }
  for (int tx = 0; tx < 32; tx++) g_vram[0x4400 + 16 * 32 + tx] = F | 0 << 13;   // BG1 floor, row 16, priority 0
  for (int i = 0; i < 128; i++) g_ram[0x370 + 4 * i + 1] = 0xF0;               // OAM: hidden
  memset(&g_ram[0x570], 0, 32);
  g_ram[0x370] = 60, g_ram[0x371] = 100, g_ram[0x372] = 0x00, g_ram[0x373] = 0x31;   // a priority-3 sprite (character 0x100)
  g_ram[0x179C] = 3, g_ram[0x179D] = 0;                  // boss: Kraid
  int H2 = 4, V2 = 3;
  Records(H2, V2, 0, 0);
  LowPoly_BeginStep();
  LowPoly_Capture(0, true, &g_fr);
  printf("boss: %s, canvas %dx%d tiles at screen %d,%d, %d palettes, %d pieces in front\n", lf->boss.on ? "on" : "OFF",
         g_lp_boss.bw, g_lp_boss.bh, lf->boss.x, lf->boss.y, g_lp_boss.np, lf->npc);
  if (!lf->boss.on) return;
  // pictures: the 2D picture (BG2, then the pieces), and the 3D boss drawn over it
  const int CW = 128, CH = 112, S = 3;
  Img im = { 2 * CW * S, CH * S, NULL };
  im.px = (uint8_t *)calloc(1, im.w * im.h * 3);
  int cx = lf->boss.x + g_lp_boss.bw * 4, cy = lf->boss.y + g_lp_boss.bh * 4;
  for (int pass = 0; pass < 2; pass++) {
    Clear();
    for (int y = 0; y < g_lp_boss.bh * 8; y++)
      for (int x = 0; x < g_lp_boss.bw * 8; x++) {
        uint8_t v = g_lp_boss.mem[y * g_lp_boss.tw + x];
        int sx = SNES_LEFT + lf->boss.x + x, sy = SNES_TOP + lf->boss.y + y;
        uint16_t c = lf->bossct[v & 63];
        if (v && sy >= SNES_TOP + LP_HUD && sy < SCREEN_H && sx >= 0 && sx < SCREEN_W)
          g_fb[sy][sx] = (c >> 11 & 31) << 19 | (c >> 6 & 31) << 11 | (c >> 1 & 31) << 3;
      }
    for (int i = 0; i < lf->npc; i++)
      for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
          uint8_t v = lf->tex[lf->pctex + i * 64 + y * 8 + x];
          uint16_t c = lf->fullpal[v];
          if (v && (c & 1)) g_fb[SNES_TOP + lf->pc[i].y + y][SNES_LEFT + lf->pc[i].x + x] = (c >> 11 & 31) << 19 | (c >> 6 & 31) << 11 | (c >> 1 & 31) << 3;
        }
    g_lp_ntri = 0, g_words = 0;
    if (pass) LpDrawBoss(lf);
    if (pass) printf("boss: %u texture rectangles, %ld command words\n", g_lp_ntri, g_words);
    Blit(&im, pass, 0, CW, CH, S, cx, cy);
  }
  Save(&im, path);
  // a tile changes: the update must equal a canvas built from scratch
  static uint8_t inc[65536];
  int at = 0x4000 + 12 * 32 + 12;
  g_vram[at] = (uint16_t)((g_vram[at] & ~0x3ff) | ((g_vram[at] + 5) & 0x3ff));
  g_ppur_pagegen[at >> 7]++;
  LowPoly_BeginStep();
  LowPoly_Capture(0, true, &g_fr);
  int bytes = g_lp_boss.tw * g_lp_boss.bh * 8;
  memcpy(inc, g_lp_boss.mem, bytes);
  g_lp_boss.valid = false;
  LowPoly_BeginStep();
  LowPoly_Capture(0, true, &g_fr);
  printf("boss: tile changed, updated canvas %s the rebuilt one (%d bytes)\n", memcmp(inc, g_lp_boss.mem, bytes) ? "DIFFERS from" : "equals", bytes);
  // a wave (BG2 scroll differs between lines): stays 2D
  Records(H2, V2, 0, 0);
  g_fr.lines[150].bg[1].hscroll = H2 + 3;
  LowPoly_BeginStep();
  LowPoly_Capture(0, true, &g_fr);
  printf("boss: with a wave on one line it is %s\n", lf->boss.on ? "ON (wrong)" : "left 2D (right)");

  // The player behind things: a BG1 row of priority 1 across her legs and a
  // sprite written before hers (OAM 0) over her head must be drawn again in
  // front of the 3D player; a priority-0 BG1 row must not.
  g_ram[0x179C] = 0;                                     // no boss
  for (int i = 0; i < 1024; i++) g_vram[0x4000 + i] = E, g_vram[0x4400 + i] = E;
  for (int tx = 0; tx < 32; tx++) g_vram[0x4400 + 16 * 32 + tx] = F | 0x2000;   // priority 1: in front of her
  for (int tx = 0; tx < 32; tx++) g_vram[0x4400 + 12 * 32 + tx] = F;            // priority 0: behind her
  SetPalette(0x9b9400);
  for (int i = 0; i < 128; i++) g_ram[0x370 + 4 * i + 1] = 0xF0;
  g_ram[0x370] = 124, g_ram[0x371] = 80, g_ram[0x372] = 0x00, g_ram[0x373] = 0x21;   // a sprite before hers
  Records(0, 0, 0, 0);
  LowPoly_BeginStep();
  HeroPose(0x01, 0, 128, 112);
  LowPoly_NoteHeroOamFirst(1);
  g_lp_step.h.valid = true;
  LowPoly_Capture(0, true, &g_fr);
  printf("player: %d pieces of the 2D picture in front of her (expected: the priority-1 row over her and the sprite)\n", lf->npc - lf->nbpc);
  Img hm = { 96 * S, 96 * S, NULL };
  hm.px = (uint8_t *)calloc(1, hm.w * hm.h * 3);
  Clear();
  for (int i = 0; i < 40; i++) LpDrawHeroSprite(lf);
  Blit(&hm, 0, 0, 96, 96, S, 128, 100);
  char hp[512];
  snprintf(hp, sizeof(hp), "%s.hero.ppm", path);
  Save(&hm, hp);
}

int main(int argc, char **argv) {
  if (argc < 4) { fprintf(stderr, "usage: %s sm.smc hero.ppm enemies.ppm [boss.ppm]\n", argv[0]); return 1; }
  FILE *f = fopen(argv[1], "rb");
  if (!f) { perror(argv[1]); return 1; }
  g_rom = (uint8_t *)calloc(1, 0x400000);
  fread(g_rom, 1, 0x400000, f);
  fclose(f);
  LowPoly_Init();
  LpFrame *lf = &g_lp_frames[0];

  // The player.
  static const struct { int pose, frame; const char *name; } kPoses[] = {
    { 0x01, 0, "stand R" }, { 0x02, 0, "stand L" }, { 0x09, 3, "run R" }, { 0x15, 0, "jump aim up" },
    { 0x19, 2, "spin jump" }, { 0x27, 0, "crouch" }, { 0x1D, 2, "morph ball" },
  };
  const int np = sizeof(kPoses) / sizeof(kPoses[0]), CW = 72, CH = 80, S = 3;
  Img hi = { np * CW * S, 3 * CH * S, NULL };
  hi.px = (uint8_t *)calloc(1, hi.w * hi.h * 3);
  SetPalette(0x9b9400);                 // power suit
  for (int k = 0; k < np; k++)
    for (int row = 0; row < 3; row++) {
      LowPoly_BeginStep();
      HeroPose(kPoses[k].pose, kPoses[k].frame, 128, 112);
      g_lp_step.h.valid = true, g_lp_step.h.left = row == 2;
      Clear();
      LowPoly_Capture(0, true, NULL);
      if (!lf->hero) { printf("%s: no hero object\n", kPoses[k].name); continue; }
      if (row == 0) Draw2D(lf, 0);
      else {
        for (int i = 0; i < 40; i++) { g_lp_ntri = 0; g_words = 0; LpDrawHeroSprite(lf); }   // let the turn settle
        if (row == 1) printf("player %-12s grid %3dx%-3d %3u texture rectangles, %4ld command words\n", kPoses[k].name,
                             lf->obj[0].gw, lf->obj[0].gh, g_lp_ntri, g_words);
      }
      Blit(&hi, k, row, CW, CH, S, 128, 104);
    }
  Save(&hi, argv[2]);

  // Opponents, one at a time: at the screen's left (the right side shows) and right.
  const int ne = sizeof(kEnemies) / sizeof(kEnemies[0]), EW = 64, EH = 64;
  Img ei = { ne * EW * S, 3 * EH * S, NULL };
  ei.px = (uint8_t *)calloc(1, ei.w * ei.h * 3);
  for (int k = 0; k < ne; k++) {
    uint32_t sm = LoadEnemy(&kEnemies[k]);
    if (!sm) { printf("enemy %-10s not found\n", kEnemies[k].name); continue; }
    for (int row = 0; row < 3; row++) {
      int x = row == 2 ? 200 : 56, y = 112;
      LowPoly_BeginStep();
      if (!NoteEnemy(sm, x, y)) { printf("enemy %-10s refused (spritemap %06X)\n", kEnemies[k].name, sm); break; }
      Clear();
      LowPoly_Capture(0, true, NULL);
      if (!lf->nobj) { printf("enemy %-10s no object\n", kEnemies[k].name); break; }
      g_lp_ntri = 0, g_words = 0;
      if (row == 0) Draw2D(lf, 0);
      else LpDrawEnemySprites(lf);
      if (row == 1) printf("enemy  %-12s grid %3dx%-3d %3u texture rectangles, %4ld command words\n", kEnemies[k].name,
                           lf->obj[0].gw, lf->obj[0].gh, g_lp_ntri, g_words);
      Blit(&ei, k, row, EW, EH, S, x, y);
    }
  }
  Save(&ei, argv[3]);

  // A crowded screen: the player and twelve opponents (Metroids) at once.
  LowPoly_BeginStep();
  SetPalette(0x9b9400);
  HeroPose(0x01, 0, 128, 150);
  uint32_t sm = LoadEnemy(&kEnemies[4]);
  int taken = 0;
  for (int i = 0; i < 12; i++) taken += NoteEnemy(sm, 24 + (i % 6) * 40, 50 + (i / 6) * 50);
  g_lp_step.h.valid = true;
  LowPoly_Capture(0, true, NULL);
  g_lp_ntri = 0, g_words = 0;
  LpDrawEnemySprites(lf);
  LpDrawHeroSprite(lf);
  printf("crowd: %d of 12 opponents taken, %d objects, %d texels (of %d), %u texture rectangles, %ld command words (%ld bytes)\n",
         taken, lf->nobj, lf->ntex, LP_TEXPOOL, g_lp_ntri, g_words, g_words * 8);
  if (argc >= 5) BossTest(argv[4]);
  return 0;
}
