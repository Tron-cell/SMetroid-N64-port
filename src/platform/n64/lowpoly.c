// Low-poly look: the RDP draws flat-shaded, faceted polygon models over the
// finished picture in place of the player's and the enemies' sprites.
//
// - The hero is Kestrel, an original design (purple full-face helmet, green
//   armor over a purple suit; see the design doc). It is not a model of the
//   game's character; it only follows the player's position, facing, height,
//   aim and movement (walk, jump, spin, ball).
// - Enemies that rise up and spit fireballs become the Burrowdrake, an original
//   bipedal dragon (green in the first area, crimson elsewhere), with a small
//   bilinear-filtered scale texture. Other opponents (enemies and bosses drawn
//   from sprite maps, with contact damage) become solid-colour faceted shapes
//   (octahedron, icosahedron, bipyramid, cube, crystal; a spiked ball for big
//   ones) sized to the sprite and coloured from its palette. Flashes white when
//   hit, icy when frozen.
// - The HUD, items, shots, effects and backgrounds stay 2D.
//
// How: after the logic steps, LowPoly_Capture() keeps what to draw and the rows
// it may cover. Once the RSP has finished that frame's picture, LowPoly_Draw()
// transforms the models, writes an RDP command list and starts the RDP on it
// directly (no rspq: the RSP runs this port's own microcode), while the RSP
// already works on the next frame. Rows drawn this way are never copied by the
// RSP into the next frame (LowPoly_NoCopy), and each model is scissored to its
// rows. No Z-buffer (there's no memory to spare for one): faces are culled and
// drawn back to front, whole blocks for the hero, triangles for the shapes.
//
// L + R + C-up toggles the look. Included from n64_main.c.

#include <math.h>
#include <malloc.h>

#define LP_MAXEN   24                   // opponents per frame
#define LP_HUD     32                   // HUD rows at the top of the game picture: never drawn over

bool g_lowpoly_on = true;               // L + R + C-up toggles

// ---- What the game reports during a logic step ------------------------------
typedef struct {
  bool valid, shown, left;
  int x, feetY, radius, wx, move, aim;  // screen x, feet y, half height, world x, movement type, shot direction
} LpHero;
typedef struct {
  int16_t x0, y0, x1, y1;               // sprite box on screen (SNES pixels, x1 / y1 exclusive)
  uint16_t id;                          // enemy type (header pointer)
  uint8_t r, g, b, boss;
  uint8_t model;                        // 0 faceted shape, 1 / 2 Burrowdrake green / crimson (+4: flash or frozen tint)
} LpEnemy;
static struct { LpHero h; int nen; LpEnemy en[LP_MAXEN]; } g_lp_step;
static uint32_t g_lp_ticks;             // logic steps (animation clock)

// The hero's own sprite, as the game wrote it this step (sm_90.c): its OAM
// entries, and the tiles the NMI is about to copy into VRAM for them (straight
// from the ROM: characters 0x00-0x1F). The 3D hero is built from these pixels.
#define LP_HSPR 48
typedef struct { int16_t x, y; uint16_t ch; uint8_t attr, big; } LpSpr;
static struct { int n, first; LpSpr s[LP_HSPR]; uint32_t tmask; uint8_t tile[32][32]; } g_lp_hs;   // first: her first OAM entry
void LowPoly_NoteHeroOamFirst(int index) { if (index < g_lp_hs.first) g_lp_hs.first = index; }

static bool FxGameplay(void);            // (n64_main.c) gameplay, doors, demos...
static bool g_lp_nomem;                 // not enough memory at start-up: the look stays off (all 2D)
static int g_lp_freekb = -1;            // free heap after LowPoly_Init (KB)
bool LowPoly_Active(void) { return g_lowpoly_on && !g_lp_nomem && FxGameplay(); }   // sprites are replaced now

static void LpEsReset(void);            // (below: the opponents' sprites this step)
void LowPoly_BeginStep(void) {
  g_lp_step.h.valid = false; g_lp_step.nen = 0; g_lp_ticks++;
  g_lp_hs.n = 0; g_lp_hs.tmask = 0; g_lp_hs.first = 128;
  LpEsReset();
}

void LowPoly_NoteHeroOam(int x, int y, int ch, int attr, bool big) {
  if (g_lp_hs.n >= LP_HSPR) return;
  g_lp_hs.s[g_lp_hs.n++] = (LpSpr){ (int16_t)x, (int16_t)y, (uint16_t)ch, (uint8_t)attr, big };
}

void LowPoly_NoteHeroTiles(int firstChar, const uint8_t *src, int bytes) {
  for (int i = 0; i < bytes / 32 && i < 8 && firstChar + i < 32; i++) {
    memcpy(g_lp_hs.tile[firstChar + i], src + i * 32, 32);
    g_lp_hs.tmask |= 1u << (firstChar + i);
  }
}

void LowPoly_NoteHero(int x, int feetY, int radius, int wx, int dir, int move, int aim, bool shown) {
  LpHero *h = &g_lp_step.h;
  h->valid = true, h->shown = shown, h->left = dir == 4;
  h->x = x, h->feetY = feetY, h->radius = radius, h->wx = wx, h->move = move, h->aim = aim;
}

// rgb: 0xRRGGBB. Returns false when the list is full (the sprite is drawn then).
bool LowPoly_NoteEnemy(int x0, int y0, int x1, int y1, unsigned id, uint32_t rgb, bool boss, int model) {
  if (g_lp_step.nen >= LP_MAXEN) return false;
  LpEnemy *e = &g_lp_step.en[g_lp_step.nen++];
  e->x0 = x0, e->y0 = y0, e->x1 = x1, e->y1 = y1, e->id = id;
  e->r = rgb >> 16, e->g = rgb >> 8, e->b = rgb, e->boss = boss, e->model = model;
  return true;
}

// ---- Models -----------------------------------------------------------------
typedef struct { float x, y, z; } LpV3;

// Block models: hexahedra in model units (1 = one SNES pixel at scale 1), +X
// forward, +Y up, +Z towards the viewer, feet at y = 0. taper scales the top
// face (x, z) about its centre, tback the -X end face (y, z); rz turns the block
// about Z around its centre. limb: 0 body, 1 / 2 near / far leg, 3 / 4 near /
// far arm, 5 wings, 6 tail. mat: 0 lit + texture, 1 lit, 2 unlit (glow).
typedef struct {
  float x0, y0, z0, x1, y1, z1, taper, tback, rz;
  uint32_t rgb;
  uint8_t limb, mat;
} LpBlock;
#include "lowpoly_models.h"           // kKestrel, kDrakeGreen, kDrakeRed (generated)
#define LP_COUNT(a) (int)(sizeof(a) / sizeof((a)[0]))
#define LP_MAXBLOCKS 48

// Shapes: unit-radius meshes built at start-up.
typedef struct { int nv, nt; LpV3 v[42]; uint8_t t[80][3]; } LpMesh;
enum { kLpOcta, kLpIco, kLpBipyr, kLpCube, kLpCrystal, kLpSpiky, kLpNumMesh };
static LpMesh g_lp_mesh[kLpNumMesh];

static void LpAddTri(LpMesh *m, int a, int b, int c) { m->t[m->nt][0] = a, m->t[m->nt][1] = b, m->t[m->nt][2] = c; m->nt++; }
static int LpAddV(LpMesh *m, float x, float y, float z) { m->v[m->nv] = (LpV3){ x, y, z }; return m->nv++; }

static void LpBuildMeshes(void) {
  LpMesh *m = &g_lp_mesh[kLpOcta];
  LpAddV(m, 1, 0, 0), LpAddV(m, -1, 0, 0), LpAddV(m, 0, 1, 0), LpAddV(m, 0, -1, 0), LpAddV(m, 0, 0, 1), LpAddV(m, 0, 0, -1);
  for (int i = 0; i < 8; i++) LpAddTri(m, i & 1, 2 + (i >> 1 & 1), 4 + (i >> 2 & 1));

  m = &g_lp_mesh[kLpIco];
  const float p = 0.5257311f, q = 0.8506508f;
  static const float kIv[12][3] = { {-p,q,0},{p,q,0},{-p,-q,0},{p,-q,0},{0,-p,q},{0,p,q},{0,-p,-q},{0,p,-q},{q,0,-p},{q,0,p},{-q,0,-p},{-q,0,p} };
  static const uint8_t kIt[20][3] = { {0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},{1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
                                      {3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},{4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1} };
  for (int i = 0; i < 12; i++) LpAddV(m, kIv[i][0], kIv[i][1], kIv[i][2]);
  for (int i = 0; i < 20; i++) LpAddTri(m, kIt[i][0], kIt[i][1], kIt[i][2]);

  m = &g_lp_mesh[kLpBipyr];             // hexagonal bipyramid
  for (int i = 0; i < 6; i++) LpAddV(m, cosf(i * 1.0471976f), 0, sinf(i * 1.0471976f));
  LpAddV(m, 0, 1.2f, 0), LpAddV(m, 0, -1.2f, 0);
  for (int i = 0; i < 6; i++) LpAddTri(m, i, (i + 1) % 6, 6), LpAddTri(m, i, (i + 1) % 6, 7);

  m = &g_lp_mesh[kLpCube];
  for (int i = 0; i < 8; i++) LpAddV(m, (i & 1) ? 0.75f : -0.75f, (i & 2) ? 0.75f : -0.75f, (i & 4) ? 0.75f : -0.75f);
  static const uint8_t kCf[6][4] = { {0,2,3,1}, {4,5,7,6}, {0,1,5,4}, {2,6,7,3}, {0,4,6,2}, {1,3,7,5} };
  for (int f = 0; f < 6; f++) LpAddTri(m, kCf[f][0], kCf[f][1], kCf[f][2]), LpAddTri(m, kCf[f][0], kCf[f][2], kCf[f][3]);

  m = &g_lp_mesh[kLpCrystal];           // pentagonal prism with pointed ends
  for (int k = 0; k < 2; k++)
    for (int i = 0; i < 5; i++) LpAddV(m, 0.8f * cosf(i * 1.2566371f), k ? 0.45f : -0.45f, 0.8f * sinf(i * 1.2566371f));
  LpAddV(m, 0, 1.15f, 0), LpAddV(m, 0, -1.15f, 0);
  for (int i = 0; i < 5; i++) {
    int j = (i + 1) % 5;
    LpAddTri(m, i, j, 5 + j), LpAddTri(m, i, 5 + j, 5 + i);
    LpAddTri(m, 5 + i, 5 + j, 10), LpAddTri(m, i, j, 11);
  }

  m = &g_lp_mesh[kLpSpiky];             // icosahedron with a spike on every face
  const LpMesh *ico = &g_lp_mesh[kLpIco];
  for (int i = 0; i < 12; i++) LpAddV(m, ico->v[i].x * 0.8f, ico->v[i].y * 0.8f, ico->v[i].z * 0.8f);
  for (int f = 0; f < 20; f++) {
    const uint8_t *t = ico->t[f];
    LpV3 a = ico->v[t[0]], b = ico->v[t[1]], c = ico->v[t[2]];
    float k = 1.25f / 3.0f;             // apex well outside the faces
    int ap = LpAddV(m, (a.x + b.x + c.x) * k, (a.y + b.y + c.y) * k, (a.z + b.z + c.z) * k);
    if (ap >= 42) { m->nv--; break; }
    LpAddTri(m, t[0], t[1], ap), LpAddTri(m, t[1], t[2], ap), LpAddTri(m, t[2], t[0], ap);
  }
}

// ---- RDP commands -------------------------------------------------------------
typedef struct { float x, y, z, s, t; } LpVert;   // screen x / y, z 0..1 (nearer = smaller), texel s / t

static inline int32_t LpFloorI(float x) { int32_t i = (int32_t)x; return i - (x < (float)i); }
static int32_t LpFix16(float f) {
  if (f >= 32767.f) return 0x7fffffff;
  if (f < -32768.f) return (int32_t)0x80000000;
  return LpFloorI(f * 65536.f);
}

// Triangle (RDP command 0x08 + 2 if textured + 1 if Z-buffered). Edge, texture
// and Z setup as libdragon's rdpq_triangle_cpu (texture: no perspective, w = 1).
static uint64_t *LpTri(uint64_t *w, const LpVert *v1, const LpVert *v2, const LpVert *v3, bool tex, bool zb) {
  const LpVert *t;
  if (v1->y > v2->y) t = v1, v1 = v2, v2 = t;
  if (v2->y > v3->y) t = v2, v2 = v3, v3 = t;
  if (v1->y > v2->y) t = v1, v1 = v2, v2 = t;
  int32_t y1f = LpFloorI(v1->y * 4), y2f = LpFloorI(v2->y * 4), y3f = LpFloorI(v3->y * 4);
  float y1 = y1f * 0.25f, y2 = y2f * 0.25f, y3 = y3f * 0.25f;
  float x1 = v1->x, x2 = v2->x, x3 = v3->x;
  float hx = x3 - x1, hy = y3 - y1, mx = x2 - x1, my = y2 - y1, lx = x3 - x2, ly = y3 - y2;
  float nz = hx * my - hy * mx;
  uint32_t lft = nz < 0;
  // y is snapped to quarter pixels, so a non-zero height is >= 0.25 and every
  // slope fits 16.16 (screen widths are < 1024): plain conversions suffice
  float ish = hy != 0 ? hx / hy : 0;
  float ism = my != 0 ? mx / my : 0;
  float isl = ly != 0 ? lx / ly : 0;
  float fy = (float)(y1f >> 2) - y1;               // from y1 up to its whole scanline (y1f >> 2 = floor)
  float xh = x1 + fy * ish, xm = x1 + fy * ism, xl = x2;
  uint32_t cmd = 0x08 | (tex ? 2 : 0) | (zb ? 1 : 0);
  #define LP_FX(f) ((uint32_t)(int32_t)((f) * 65536.0f))
  w[0] = (uint64_t)(cmd << 24 | lft << 23 | (y3f & 0x3fff)) << 32 | (uint32_t)((y2f & 0x3fff) << 16 | (y1f & 0x3fff));
  w[1] = (uint64_t)LP_FX(xl) << 32 | LP_FX(isl);
  w[2] = (uint64_t)LP_FX(xh) << 32 | LP_FX(ish);
  w[3] = (uint64_t)LP_FX(xm) << 32 | LP_FX(ism);
  #undef LP_FX
  w += 4;
  if (!tex && !zb) return w;
  float attr = fabsf(nz) > 1e-20f ? -1.0f / nz : 0;
  if (tex) {
    float s1 = v1->s * 32.f, t1 = v1->t * 32.f, s2 = v2->s * 32.f, t2 = v2->t * 32.f, s3 = v3->s * 32.f, t3 = v3->t * 32.f;
    float ms = s2 - s1, mt = t2 - t1, hs = s3 - s1, ht = t3 - t1;
    float dsdx = (hy * ms - my * hs) * attr, dtdx = (hy * mt - my * ht) * attr;
    float dsdy = (mx * hs - hx * ms) * attr, dtdy = (mx * ht - hx * mt) * attr;
    float dsde = dsdy + dsdx * ish, dtde = dtdy + dtdx * ish;
    int32_t S = LpFix16(s1 + fy * dsde), T = LpFix16(t1 + fy * dtde), W = LpFix16(0x7fff);
    int32_t SX = LpFix16(dsdx), TX = LpFix16(dtdx), SE = LpFix16(dsde), TE = LpFix16(dtde), SY = LpFix16(dsdy), TY = LpFix16(dtdy);
    #define LP_HI(a, b) ((uint32_t)(((a) & 0xffff0000) | (0xffff & ((b) >> 16))))
    #define LP_LO(a, b) ((uint32_t)(((a) << 16) | ((b) & 0xffff)))
    w[0] = (uint64_t)LP_HI(S, T) << 32 | (uint32_t)(W & 0xffff0000);
    w[1] = (uint64_t)LP_HI(SX, TX) << 32;
    w[2] = (uint64_t)LP_LO(S, T) << 32 | (uint32_t)(W << 16);
    w[3] = (uint64_t)LP_LO(SX, TX) << 32;
    w[4] = (uint64_t)LP_HI(SE, TE) << 32;
    w[5] = (uint64_t)LP_HI(SY, TY) << 32;
    w[6] = (uint64_t)LP_LO(SE, TE) << 32;
    w[7] = (uint64_t)LP_LO(SY, TY) << 32;
    #undef LP_HI
    #undef LP_LO
    w += 8;
  }
  if (zb) {
    float z1 = v1->z * 0x7fff, z2 = v2->z * 0x7fff, z3 = v3->z * 0x7fff;
    float mz = z2 - z1, hz = z3 - z1;
    float dzdx = (hy * mz - my * hz) * attr, dzdy = (mx * hz - hx * mz) * attr;
    float dzde = dzdy + dzdx * ish;
    w[0] = (uint64_t)(uint32_t)LpFix16(z1 + fy * dzde) << 32 | (uint32_t)LpFix16(dzdx);
    w[1] = (uint64_t)(uint32_t)LpFix16(dzde) << 32 | (uint32_t)LpFix16(dzdy);
    w += 2;
  }
  return w;
}

#define LP_SYNC_PIPE   (0x27ull << 56)
#define LP_SYNC_LOAD   (0x26ull << 56)
#define LP_SYNC_TILE   (0x28ull << 56)
#define LP_SYNC_FULL   (0x29ull << 56)
#define LP_SCISSOR(x0, y0, x1, y1) (0x2Dull << 56 | (uint64_t)((x0) * 4) << 44 | (uint64_t)((y0) * 4) << 32 | (uint64_t)((x1) * 4) << 12 | (uint64_t)((y1) * 4))
#define LP_OTHER_MODES(m) (0x2Full << 56 | (m))
#define LP_FILL_COLOR(c) (0x37ull << 56 | (uint32_t)(c))
#define LP_FILL_RECT(x0, y0, x1, y1) (0x36ull << 56 | (uint64_t)((x1) * 4) << 44 | (uint64_t)((y1) * 4) << 32 | (uint64_t)((x0) * 4) << 12 | (uint64_t)((y0) * 4))
#define LP_COLOR_IMAGE(w, addr) (0x3Full << 56 | 2ull << 51 | (uint64_t)((w) - 1) << 32 | (uint32_t)(addr))
#define LP_Z_IMAGE(addr) (0x3Eull << 56 | (uint32_t)(addr))
#define LP_TEX_IMAGE(w, addr) (0x3Dull << 56 | 2ull << 51 | (uint64_t)((w) - 1) << 32 | (uint32_t)(addr))   // RGBA16
// tile: RGBA16, line in 64-bit words, TMEM 0, wrapping with 2^mask texels
#define LP_SET_TILE(tile, line, mask) (0x35ull << 56 | 2ull << 51 | (uint64_t)(line) << 41 | \
                                       (uint64_t)(tile) << 24 | (uint64_t)(mask) << 14 | (uint64_t)(mask) << 4)
#define LP_LOAD_TILE(tile, w, h) (0x34ull << 56 | (uint64_t)(tile) << 24 | (uint64_t)(((w) - 1) * 4) << 12 | (uint64_t)(((h) - 1) * 4))
#define LP_TILE_SIZE(tile, w, h) (0x32ull << 56 | (uint64_t)(tile) << 24 | (uint64_t)(((w) - 1) * 4) << 12 | (uint64_t)(((h) - 1) * 4))
#define LP_PRIM_COLOR(rgba) (0x3Aull << 56 | CfRGBA((uint32_t)(rgba)))   // (through the colour filter)
// combiners (libdragon RDPQ_COMBINER1): PRIM; TEX0 * PRIM (alpha PRIM)
#define LP_CC_PRIM    (0x3Cull << 56 | 0x00887f1088fdf6fbull)
#define LP_CC_TEXPRIM (0x3Cull << 56 | 0x0011fe2388fff7fbull)
// 1-cycle, no dither, coverage zap, RGB texture filter: flat / bilinear texture
#define LP_SOM_FLAT  0x00000cf000000200ull
#define LP_SOM_TEX   0x00002cf000000200ull
// The sprites' textures: CI8 (colour indices) through a TLUT of RGBA16 colours
// in the upper half of TMEM (as libdragon's rdpq_tex_upload_tlut loads it).
// + TLUT (RGBA16) + alpha compare (texels whose TLUT colour has alpha 0 are not drawn)
#define LP_SOM_LAYER (LP_SOM_FLAT | 2ull << 46 | 1ull)
#define LP_TEX_IMAGE_CI8(w, addr) (0x3Dull << 56 | 2ull << 53 | 1ull << 51 | (uint64_t)((w) - 1) << 32 | (uint32_t)(addr))
#define LP_SET_TILE_CI8(tile, line) (0x35ull << 56 | 2ull << 53 | 1ull << 51 | (uint64_t)(line) << 41 | (uint64_t)(tile) << 24)
// TLUT entries idx.. in the upper half of TMEM (one 64-bit word per entry)
#define LP_SET_TILE_TLUT(tile, idx) (0x35ull << 56 | 4ull << 53 | (uint64_t)((256 + 8) / 8) << 41 | (uint64_t)(256 + (idx)) << 32 | (uint64_t)(tile) << 24)
#define LP_LOAD_TLUT(tile, n) (0x30ull << 56 | (uint64_t)(tile) << 24 | (uint64_t)((n) - 1) << 14)
#define LP_BLEND_COLOR(rgba) (0x39ull << 56 | (uint32_t)(rgba))
// colour TEX0 * PRIM, alpha TEX0 (libdragon RDPQ_COMBINER1((TEX0,0,PRIM,0),(0,0,0,TEX0)))
#define LP_CC_TEXPRIM_TA (0x3Cull << 56 | 0x0011fe2388fff3f9ull)
// texture rectangle, 10.2 screen coordinates (x1 / y1 exclusive), s / t in 10.5, 1:1
#define LP_TEXRECT0(x0, y0, x1, y1, tile) (0x24ull << 56 | (uint64_t)((x1) & 0xfff) << 44 | (uint64_t)((y1) & 0xfff) << 32 | \
                                           (uint64_t)(tile) << 24 | (uint64_t)((x0) & 0xfff) << 12 | (uint64_t)((y0) & 0xfff))
#define LP_TEXRECT1(s, t) ((uint64_t)((s) & 0xffff) << 48 | (uint64_t)((t) & 0xffff) << 32 | 0x04000400ull)

#define LP_TEX 16                       // texture size (texels)

// ---- State -------------------------------------------------------------------
#ifndef LP_HERO_KESTREL
#define LP_HERO_KESTREL 0               // 1: the hero is the Kestrel block model, not the player's own sprite in 3D
#endif
#ifndef LP_ENEMY_SHAPES
#define LP_ENEMY_SHAPES 0               // 1: opponents are faceted shapes / the Burrowdrake, not their own sprites in 3D
#endif
bool LowPoly_EnemySprites(void) { return !LP_ENEMY_SHAPES; }
// Sprites in 3D (the player, and the opponents): the sprite is a stack of thin
// layers, each the sprite's own pixels (a texture) drawn by the RDP as one
// texture rectangle, slightly further along the view than the one behind it.
// Pixels further inside the silhouette (d: 1 at the outline ... LP_DCAP) reach
// further forward, so the figure is rounded; the layers' edges show as its
// sides. No triangles: the CPU only writes a few commands per layer.
#define LP_DCAP 4
#define LP_GW 128                       // largest sprite grid (pixels); bigger opponents stay 2D
#define LP_GH 112
#define LP_HERO_GW 64                   // the player's grid (clipped to it)
#define LP_HERO_GH 72
#define LP_TEXPOOL 16384                // texels (bytes: CI8) per frame for all the sprites' textures
#define LP_ETEXELS (LP_TEXPOOL - LP_HERO_GW * LP_HERO_GH)   // ... of which the opponents may use
#define LP_TEXELS 2048                  // CI8 texels that fit TMEM next to the TLUT: taller sprites are loaded in bands
#define LP_MAXPAL 4                     // sprite palettes one object may use (more: it stays 2D)
typedef struct {
  int16_t gx, gy, gw, gh;               // the sprite's grid on the game picture
  int16_t tw, bh;                       // texture line (texels, multiple of 8), band height (rows)
  uint16_t tex;                         // first texel in the pool
  int16_t row0, row1;                   // game-picture rows it may touch
  bool big;                             // in the big pool (a boss drawn from sprites)
} LpObj;
#define LP_MAXOBJ (1 + LP_MAXEN)
#define LP_MAXPC 128                    // pieces of the 2D picture drawn again in front of a BG2 boss

typedef struct {
  bool on;                              // drawn
  uint8_t *tex;                         // texture pool (LP_TEXPOOL texels, CI8: (d - 1) << 6 | palette slot << 4 | colour)
  int ntex, ntexb, nobj;                // used: texels (pool, big pool), objects
  bool hero;                            // obj[0] is the player
  LpObj obj[LP_MAXOBJ];
  uint16_t ctab[LP_MAXOBJ][64] __attribute__((aligned(16)));   // each object's palettes (RGBA16; colour 0 transparent)
  struct { bool on; int16_t x, y, row0, row1; uint32_t stamp; } boss;   // the BG2 boss: canvas at x, y
  uint16_t bossct[64] __attribute__((aligned(16)));
  int npc, nbpc;                        // pieces of the 2D picture drawn again in front (0 .. nbpc: the boss's,
                                        // nbpc .. npc: the player's), 8 x 8 each, a strip at tex + pctex
  uint16_t pctex;
  struct { int16_t x, y; } pc[LP_MAXPC];
  uint16_t fullpal[256] __attribute__((aligned(16)));   // all 16 palettes (the pieces' colours)
  LpHero h;
  int nen;
  LpEnemy en[LP_MAXEN];
  int16_t er0[LP_MAXEN], er1[LP_MAXEN]; // rows each opponent may touch (game picture rows)
  int16_t hr0, hr1;                     // rows the hero may touch
  uint32_t rows[(SNES_H + 31) / 32];    // union: game-picture rows drawn over
} LpFrame;
static LpFrame g_lp_frames[2];

// The opponents' sprites as the game wrote them this step (sm_a0.c), one group
// per opponent. A group too big for the grid, or past the texture budget, is
// refused: the game keeps its sprites (2D).
#define LP_ESPR 256
typedef struct { uint16_t first, n; bool big; } LpGroup;
static struct { int n, ng, texels, btexels; LpSpr s[LP_ESPR]; LpGroup g[LP_MAXEN]; } g_lp_es;
static void LpEsReset(void) { g_lp_es.n = g_lp_es.ng = g_lp_es.texels = g_lp_es.btexels = 0; }
static bool LpBigPoolOk(void);          // (below: a boss drawn from sprites may use the big pool)
#define LP_BIGPOOL 32768                // per frame, for bosses drawn from sprites (shares the BG2 boss's memory)
#define LP_BIG_GW 256                   // largest grid there
#define LP_BIG_GH 224

static void LpSprBox(const LpSpr *s, int n, int *b) {
  b[0] = b[1] = 9999, b[2] = b[3] = -9999;
  for (int i = 0; i < n; i++) {
    int sz = s[i].big ? 16 : 8;
    if (s[i].x < b[0]) b[0] = s[i].x;
    if (s[i].y < b[1]) b[1] = s[i].y;
    if (s[i].x + sz > b[2]) b[2] = s[i].x + sz;
    if (s[i].y + sz > b[3]) b[3] = s[i].y + sz;
  }
}

bool LowPoly_BeginEnemy(void) {
  if (g_lp_es.ng >= LP_MAXEN) return false;
  LpGroup *g = &g_lp_es.g[g_lp_es.ng];
  g_lp_es.n = g_lp_es.ng ? g[-1].first + g[-1].n : 0;   // (drops a group that was never ended)
  g->first = g_lp_es.n, g->n = 0;
  return true;
}
bool LowPoly_NoteEnemyOam(int x, int y, int ch, int attr, bool big) {
  if (g_lp_es.n >= LP_ESPR) return false;
  g_lp_es.s[g_lp_es.n++] = (LpSpr){ (int16_t)x, (int16_t)y, (uint16_t)ch, (uint8_t)attr, big };
  g_lp_es.g[g_lp_es.ng].n++;
  return true;
}
bool LowPoly_EndEnemy(void) {
  LpGroup *g = &g_lp_es.g[g_lp_es.ng];
  int b[4];
  LpSprBox(&g_lp_es.s[g->first], g->n, b);
  int gw = b[2] - b[0], gh = b[3] - b[1], tx = ((gw + 7) & ~7) * gh;
  unsigned pals = 0;
  for (int i = 0; i < g->n; i++) pals |= 1u << (g_lp_es.s[g->first + i].attr >> 1 & 7);
  g->big = false;
  if (!g->n || __builtin_popcount(pals) > LP_MAXPAL) { g_lp_es.n = g->first; return false; }
  if (gw > LP_GW || gh > LP_GH || g_lp_es.texels + tx > LP_ETEXELS) {
    // too big for the opponents' pool: a boss drawn from sprites (Ridley...) gets the big pool
    if (!LpBigPoolOk() || gw > LP_BIG_GW || gh > LP_BIG_GH || g_lp_es.btexels + tx > LP_BIGPOOL) { g_lp_es.n = g->first; return false; }
    g->big = true;
    g_lp_es.btexels += tx;
    g_lp_es.ng++;
    return true;
  }
  g_lp_es.texels += tx;
  g_lp_es.ng++;
  return true;
}

#define LP_CHUNK 256                   // 64-bit words per command buffer (two, used in turn)
static uint64_t *g_lp_cmds;             // command buffers (uncached)
static uint64_t *g_lp_w, *g_lp_wend;    // write position, end of the buffer being written
static uint64_t *g_lp_wstart;           // its first command not yet given to the RDP
static uint32_t g_lp_ntri;              // per-second readout: triangles
static uint32_t g_lp_tt[4];             // LPPROF: ticks in transform, faces, triangle setup, submit

#define LP_DP ((volatile uint32_t *)0xA4100000)   // RDP command registers: START, END, CURRENT, STATUS
// Gives the RDP the commands written so far. The other buffer is reused once
// the RDP has started on these (so it has finished the other's).
static void LpSubmit(bool more) {
  if (g_lp_w == g_lp_wstart) return;
  while (LP_DP[3] & 0x400) {}           // the previous submission not started yet
  LP_DP[0] = PhysicalAddr(g_lp_wstart);
  LP_DP[1] = PhysicalAddr(g_lp_w);
  if (!more) return;
  uint64_t *other = g_lp_wend == g_lp_cmds + LP_CHUNK ? g_lp_cmds + LP_CHUNK : g_lp_cmds;
  while (LP_DP[3] & 0x400) {}           // these started: the other buffer is free
  g_lp_w = g_lp_wstart = other;
  g_lp_wend = other + LP_CHUNK;
}
#ifdef LP_HOST_TEST                     // tools/lowpoly/sprite3d_test.c: commands and triangles go to a PC rasterizer
static void LpHostCmd(uint64_t c);
static void LpHostTri(const LpVert *v1, const LpVert *v2, const LpVert *v3, bool tex);
static void LpHostTex(const uint8_t *tex, int w, int h);
static void LpHostTlut(int idx, const uint16_t *src, int n);
static void LpHostRect(int x0, int y0, int x1, int y1, int s, int t);   // 10.2 / 10.5, as the command
#endif
static inline void LpPut(uint64_t c) {
#ifdef LP_HOST_TEST
  LpHostCmd(c); return;
#endif
  if (g_lp_w == g_lp_wend) LpSubmit(true);
  *g_lp_w++ = c;
}
static inline void LpPutTri(const LpVert *v1, const LpVert *v2, const LpVert *v3, bool tex, bool zb) {
#ifdef LP_HOST_TEST
  g_lp_ntri++; LpHostTri(v1, v2, v3, tex); return;
#endif
#ifdef LPPROF
  uint32_t t0 = TICKS_READ();
#endif
  if (g_lp_wend - g_lp_w < 16) LpSubmit(true);
#ifdef LPPROF
  uint32_t t1 = TICKS_READ();
#endif
  g_lp_ntri++;
  g_lp_w = LpTri(g_lp_w, v1, v2, v3, tex, zb);
#ifdef LPPROF
  uint32_t t2 = TICKS_READ();
  g_lp_tt[3] += t1 - t0; g_lp_tt[2] += t2 - t1;
#endif
}
static uint16_t *g_lp_tex[2];           // 16 x 16 RGBA16 textures: 0 panels, 1 scales
static float g_lp_yaw = 0.6f, g_lp_phase, g_lp_roll, g_lp_ball;
static int g_lp_lastwx = -1;
static uint64_t g_lp_us;                // per-second readout: CPU + RDP time
static uint16_t g_lp_zero[64] __attribute__((aligned(16)));   // transparent TLUT entries (a layer hides a class of pixels)

static bool LpBossInit(int freekb);     // (below: the bosses' memory, if there is room)
static bool g_lp_bossmem;               // bosses in 3D (memory for them)
static void LowPoly_Init(void) {
  data_cache_hit_writeback(g_lp_zero, sizeof(g_lp_zero));      // (zero in RDRAM, whatever the cache held)
  g_lp_cmds = (uint64_t *)malloc_uncached_aligned(16, 2 * LP_CHUNK * 8);
  g_lp_tex[0] = (uint16_t *)malloc_uncached_aligned(16, LP_TEX * LP_TEX * 2);
  g_lp_tex[1] = (uint16_t *)malloc_uncached_aligned(16, LP_TEX * LP_TEX * 2);
  for (int i = 0; i < 2; i++)           // sprite textures
    g_lp_frames[i].tex = (uint8_t *)memalign(16, LP_TEXPOOL);
  if (!g_lp_cmds || !g_lp_tex[0] || !g_lp_tex[1] || !g_lp_frames[0].tex || !g_lp_frames[1].tex) {
    g_lp_nomem = true;                  // the game goes on in 2D (the bottom border says so at start-up)
    debugf("lowpoly: out of memory, the low-poly look is off\n");
    return;
  }
#ifndef LP_HOST_TEST
  heap_stats_t hs;
  sys_get_heap_stats(&hs);
  g_lp_freekb = (hs.total - hs.used) / 1024;
#endif
  g_lp_bossmem = LpBossInit(g_lp_freekb);
#ifndef LP_HOST_TEST
  sys_get_heap_stats(&hs);
  g_lp_freekb = (hs.total - hs.used) / 1024;
#endif
  debugf("lowpoly: %d KB free after its buffers (bosses %s)\n", g_lp_freekb, g_lp_bossmem ? "in 3D" : "2D: no room");
  // The texture: panels with darker seams, a bevel highlight and a little
  // grain, grey (it is multiplied by each block's colour).
  for (int y = 0; y < LP_TEX; y++)
    for (int x = 0; x < LP_TEX; x++) {
      uint32_t hsh = (uint32_t)(x * 73856093u ^ y * 19349663u);
      hsh ^= hsh >> 13; hsh *= 0x5bd1e995u; hsh ^= hsh >> 15;
      int v = 236 + (int)(hsh & 15) - 8;
      if ((x & 7) == 0 || (y & 7) == 0) v = 170;           // seams
      else if ((x & 7) == 1 || (y & 7) == 1) v = 255;      // bevel highlight
      if (((x >> 3) ^ (y >> 3)) & 1) v -= 14;              // alternate panels
      if (v > 255) v = 255;
      g_lp_tex[0][y * LP_TEX + x] = (uint16_t)((v >> 3) << 11 | (v >> 3) << 6 | (v >> 3) << 1 | 1);
      // scales: offset rows of rounded bumps with dark edges (tinted by each block's colour)
      int ox = (x + ((y >> 2) & 1) * 4) & 7, oy = y & 3;
      int u = 245 - ((ox - 4) * (ox - 4) * 5 + oy * oy * 13);
      if (oy == 3) u = 150;
      if (u < 140) u = 140;
      g_lp_tex[1][y * LP_TEX + x] = (uint16_t)((u >> 3) << 11 | (u >> 3) << 6 | (u >> 3) << 1 | 1);
    }
  LpBuildMeshes();
}

static uint32_t LpShade(int r, int g, int b, float k) {
  r = (int)(r * k), g = (int)(g * k), b = (int)(b * k);
  if (r > 255) r = 255;
  if (g > 255) g = 255;
  if (b > 255) b = 255;
  return (uint32_t)r << 24 | g << 16 | b << 8 | 0xff;
}

static const LpV3 kLpLight = { -0.42f, 0.66f, 0.62f };   // towards the light (view space, +z to the viewer)
static inline float LpLight(LpV3 n) {
  float l = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
  float d = (n.x * kLpLight.x + n.y * kLpLight.y + n.z * kLpLight.z) / (l > 1e-6f ? l : 1);
  return 0.42f + 0.72f * (d > 0 ? d : 0);
}
static inline LpV3 LpCross(LpV3 a, LpV3 b, LpV3 c) {
  LpV3 u = { b.x - a.x, b.y - a.y, b.z - a.z }, v = { c.x - a.x, c.y - a.y, c.z - a.z };
  return (LpV3){ u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x };
}
// Rows [y0, y1) of the game picture, clipped below the HUD; marks them.
static bool LpRows(LpFrame *lf, int y0, int y1, int16_t *r0, int16_t *r1) {
  if (y0 < LP_HUD) y0 = LP_HUD;
  if (y1 > SNES_H) y1 = SNES_H;
  *r0 = y0, *r1 = y1;
  if (y1 <= y0) return false;
  for (int y = y0; y < y1; y++) lf->rows[y >> 5] |= 1u << (y & 31);
  return true;
}

// Pitch: the scene is seen slightly from above.
#define LP_CP 0.9838437f                // cos 0.18
#define LP_SP 0.1790296f                // sin 0.18

// Sizes of an opponent's shape (half extents) from its sprite box.
static inline void LpEnemySize(const LpEnemy *e, float *sx, float *sy, float *sz) {
  float bw = e->x1 - e->x0, bh = e->y1 - e->y0;
  *sx = bw * 0.46f, *sy = bh * 0.46f, *sz = *sx < *sy ? *sx : *sy;
}

// Burrowdrake size: its 34-unit body matches the sprite box height.
static inline float LpDrakeScale(const LpEnemy *e) {
  float sc = (e->y1 - e->y0) / 34.0f;
  return sc < 0.5f ? 0.5f : sc > 2.5f ? 2.5f : sc;
}

// ---- Block models (hero, Burrowdrake) --------------------------------------------
#define LP_TWO_PI 6.2831853f
static inline float LpWrap(float a) {   // keep angles small: large arguments make sinf / cosf slow
  while (a > 3.14159265f) a -= LP_TWO_PI;
  while (a < -3.14159265f) a += LP_TWO_PI;
  return a;
}

// Pose of a block model: limb angles (about Z) and their pivots, whole-body roll.
typedef struct {
  float leg[2], arm[2], tail, roll;     // near / far
  float legY, armY, rollY;              // pivot heights
  float scale, scaleY;                  // uniform scale, extra vertical scale (crouch)
  bool ball;
} LpPose;

// Per model, prepared once: each block's 8 corners in model space (taper,
// tback and rz applied), its centre and its 6 outward unit face normals.
static const uint8_t kLpF[6][4] = { {0,2,3,1}, {4,5,7,6}, {0,1,5,4}, {2,6,7,3}, {0,4,6,2}, {1,3,7,5} };
// Floats (int <-> float conversions cost more than the memory saved).
// For the per-frame transform a block is also kept as its centre m and three
// half-axes: X = hx * (cos rz, sin rz, 0), Y = hy * (-sin rz, cos rz, 0),
// Z = (0, 0, hz). Corner i = m + sx X + sy Y + sz Z with |sx| = taper on the
// top, |sy| = tback at the back, |sz| = both: four vectors to transform per
// block instead of eight corners.
typedef struct { LpV3 v[8], c, n[6], m; float ux, uy, wx, wy, hz, taper, tback; uint8_t hide; } LpPrep;
#define LP_PREP_MAX 66                  // blocks of all prepared models together (Kestrel + Burrowdrake)
static LpPrep g_lp_prep[LP_PREP_MAX];
static int g_lp_nprep;
typedef struct { const LpBlock *m; int n; LpPrep *p; } LpModelCache;
static LpModelCache g_lp_cache[4];

// Models with the same geometry (the Burrowdrake colours) share one entry: pass
// the same geometry table as `geo`.
static const LpPrep *LpPrepare(const LpBlock *m, int n) {
  for (int i = 0; i < 4; i++) if (g_lp_cache[i].m == m) return g_lp_cache[i].p;
  int slot = 0;
  while (slot < 3 && g_lp_cache[slot].m) slot++;
  assertf(g_lp_nprep + n <= LP_PREP_MAX, "lowpoly: too many blocks");
  LpPrep *pp = &g_lp_prep[g_lp_nprep];
  g_lp_nprep += n;
  for (int b = 0; b < n; b++) {
    const LpBlock *bk = &m[b];
    LpPrep *q = &pp[b];
    float mx = (bk->x0 + bk->x1) * 0.5f, my = (bk->y0 + bk->y1) * 0.5f, mz = (bk->z0 + bk->z1) * 0.5f;
    float rc = cosf(bk->rz), rs = sinf(bk->rz);
    LpV3 v[8], c = { 0, 0, 0 };
    for (int i = 0; i < 8; i++) {
      float x = (i & 1) ? bk->x1 : bk->x0, y = (i & 2) ? bk->y1 : bk->y0, z = (i & 4) ? bk->z1 : bk->z0;
      if (i & 2) x = mx + (x - mx) * bk->taper, z = mz + (z - mz) * bk->taper;
      if (!(i & 1)) y = my + (y - my) * bk->tback, z = mz + (z - mz) * bk->tback;
      float xx = x - mx, yy = y - my;
      x = mx + xx * rc - yy * rs; y = my + xx * rs + yy * rc;
      v[i] = (LpV3){ x, y, z };
      c.x += x * 0.125f, c.y += y * 0.125f, c.z += z * 0.125f;
      q->v[i] = v[i];
    }
    q->c = c;
    float hx = (bk->x1 - bk->x0) * 0.5f, hy = (bk->y1 - bk->y0) * 0.5f;
    q->m = (LpV3){ mx, my, mz };
    q->ux = hx * rc, q->uy = hx * rs, q->wx = -hy * rs, q->wy = hy * rc;
    q->hz = (bk->z1 - bk->z0) * 0.5f;
    q->taper = bk->taper, q->tback = bk->tback;
    for (int f = 0; f < 6; f++) {
      const uint8_t *k = kLpF[f];
      LpV3 nn = LpCross(v[k[0]], v[k[1]], v[k[2]]);
      if (nn.x * nn.x + nn.y * nn.y + nn.z * nn.z < 1e-8f) nn = LpCross(v[k[0]], v[k[2]], v[k[3]]);
      LpV3 fc = { (v[k[0]].x + v[k[2]].x) * 0.5f - c.x, (v[k[0]].y + v[k[2]].y) * 0.5f - c.y, (v[k[0]].z + v[k[2]].z) * 0.5f - c.z };
      if (nn.x * fc.x + nn.y * fc.y + nn.z * fc.z < 0) nn.x = -nn.x, nn.y = -nn.y, nn.z = -nn.z;
      float l = sqrtf(nn.x * nn.x + nn.y * nn.y + nn.z * nn.z);
      l = l > 1e-12f ? 1.0f / l : 0;
      q->n[f] = (LpV3){ nn.x * l, nn.y * l, nn.z * l };
    }
  }
  // Faces buried inside another block of the same limb: the two move together,
  // so the face stays covered in every pose and is never drawn (fewer triangles,
  // like closing the model's seams instead of drawing over them).
  for (int b = 0; b < n; b++) {
    LpPrep *q = &pp[b];
    q->hide = 0;
    for (int f = 0; f < 6; f++) {
      const uint8_t *k = kLpF[f];
      LpV3 nf = q->n[f], pt[5];
      if (nf.x == 0 && nf.y == 0 && nf.z == 0) continue;
      for (int j = 0; j < 4; j++) pt[j] = q->v[k[j]];
      pt[4] = (LpV3){ (pt[0].x + pt[2].x) * 0.5f, (pt[0].y + pt[2].y) * 0.5f, (pt[0].z + pt[2].z) * 0.5f };
      for (int j = 0; j < 5; j++) pt[j].x += nf.x * 0.01f, pt[j].y += nf.y * 0.01f, pt[j].z += nf.z * 0.01f;   // just outside
      for (int o = 0; o < n && !(q->hide >> f & 1); o++) {
        if (o == b || m[o].limb != m[b].limb) continue;
        const LpPrep *c = &pp[o];
        bool in = true;
        for (int g = 0; g < 6 && in; g++) {
          LpV3 ng = c->n[g], a = c->v[kLpF[g][0]];
          if (ng.x == 0 && ng.y == 0 && ng.z == 0) continue;
          for (int j = 0; j < 5 && in; j++)
            if (ng.x * (pt[j].x - a.x) + ng.y * (pt[j].y - a.y) + ng.z * (pt[j].z - a.z) > -1e-3f) in = false;
        }
        if (in) q->hide |= 1 << f;
      }
    }
  }
  g_lp_cache[slot] = (LpModelCache){ m, n, pp };
  return pp;
}

typedef struct { float m[3][3]; } LpM3;
static inline LpM3 LpMul(const LpM3 *a, const LpM3 *b) {
  LpM3 r;
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++) r.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] + a->m[i][2] * b->m[2][j];
  return r;
}
static inline LpV3 LpApply(const LpM3 *a, LpV3 p) {
  return (LpV3){ a->m[0][0] * p.x + a->m[0][1] * p.y + a->m[0][2] * p.z,
                 a->m[1][0] * p.x + a->m[1][1] * p.y + a->m[1][2] * p.z,
                 a->m[2][0] * p.x + a->m[2][1] * p.y + a->m[2][2] * p.z };
}
static inline LpM3 LpRotZ(float c, float s) { return (LpM3){ { { c, -s, 0 }, { s, c, 0 }, { 0, 0, 1 } } }; }

// Draws a block model with its feet at screen (cx, fy), turned by yaw, back to
// front by block (each block is convex, so its own front faces never overlap).
// Each limb's whole transform is one 3x3 matrix plus an offset. tint != 0 draws
// every block in that colour (hit flash, frozen).
static void LpDrawModel(const LpBlock *m, int n, const LpPose *ps, float cx, float fy, float yaw, int texIdx, uint32_t tint) {
  static LpV3 v[LP_MAXBLOCKS][8];
  float depth[LP_MAXBLOCKS];
  int order[LP_MAXBLOCKS];
  if (n > LP_MAXBLOCKS) n = LP_MAXBLOCKS;
  const LpPrep *pp = LpPrepare(m == kDrakeRed ? kDrakeGreen : m, n);   // the two colours share geometry
  // view = pitch * yaw * scale; roll and limb rotations are about Z
  float cyw = cosf(yaw), syw = sinf(yaw);
  LpM3 Y = { { { cyw, 0, -syw }, { 0, 1, 0 }, { syw, 0, cyw } } };
  LpM3 P = { { { 1, 0, 0 }, { 0, LP_CP, -LP_SP }, { 0, LP_SP, LP_CP } } };
  LpM3 PY = LpMul(&P, &Y);
  LpM3 Sc = { { { ps->scale, 0, 0 }, { 0, ps->scale * ps->scaleY, 0 }, { 0, 0, ps->scale } } };
  LpM3 PYS = LpMul(&PY, &Sc);
  LpM3 R = LpRotZ(cosf(ps->roll), sinf(ps->roll));
  const float la[7] = { 0, ps->leg[0], ps->leg[1], ps->arm[0], ps->arm[1], 0, ps->tail };
  const float lpy[7] = { 0, ps->legY, ps->legY, ps->armY, ps->armY, 0, ps->legY * 0.6f };
  LpM3 M[7], Nm[7];
  LpV3 off[7];
  for (int l = 0; l < 7; l++) {
    LpM3 L = LpRotZ(cosf(la[l]), sinf(la[l]));
    LpM3 RL = LpMul(&R, &L);
    M[l] = LpMul(&PYS, &RL);
    Nm[l] = LpMul(&PY, &RL);
    // offset: PYS * (R * (pl - L pl) + pr - R pr), pl = (0, lpy), pr = (0, rollY)
    LpV3 t1 = { L.m[0][1] * -lpy[l], lpy[l] - L.m[1][1] * lpy[l], 0 };      // pl - L pl
    LpV3 t2 = LpApply(&R, t1);
    t2.x += -R.m[0][1] * ps->rollY; t2.y += ps->rollY - R.m[1][1] * ps->rollY;
    off[l] = LpApply(&PYS, t2);
  }
#ifdef LPPROF
  uint32_t tA = TICKS_READ();
#endif
  bool anyTex = false;
  for (int b = 0; b < n; b++) {
    int l = m[b].limb;
    anyTex |= m[b].mat == 0;
    const LpM3 *A = &M[l];
    const LpPrep *q = &pp[b];
    LpV3 o = off[l];
    // centre and half-axes in view space; v[] gets screen x / y (y down) and view z
    float a00 = A->m[0][0], a01 = A->m[0][1], a02 = A->m[0][2];
    float a10 = A->m[1][0], a11 = A->m[1][1], a12 = A->m[1][2];
    float a20 = A->m[2][0], a21 = A->m[2][1], a22 = A->m[2][2];
    float Cx = cx + (a00 * q->m.x + a01 * q->m.y + a02 * q->m.z + o.x);
    float Cy = fy - (a10 * q->m.x + a11 * q->m.y + a12 * q->m.z + o.y);
    float Cz = a20 * q->m.x + a21 * q->m.y + a22 * q->m.z + o.z;
    float Xx = a00 * q->ux + a01 * q->uy, Xy = -(a10 * q->ux + a11 * q->uy), Xz = a20 * q->ux + a21 * q->uy;
    float Yx = a00 * q->wx + a01 * q->wy, Yy = -(a10 * q->wx + a11 * q->wy), Yz = a20 * q->wx + a21 * q->wy;
    float Zx = a02 * q->hz, Zy = -a12 * q->hz, Zz = a22 * q->hz;
    LpV3 *vb = v[b];
    for (int i = 0; i < 4; i++) {          // i: bit 0 front (+X), bit 1 top (+Y)
      float sx = (i & 2) ? q->taper : 1.0f, sy = (i & 1) ? 1.0f : q->tback, sz = sx * sy;
      if (!(i & 1)) sx = -sx;
      if (!(i & 2)) sy = -sy;
      float bx = Cx + sx * Xx + sy * Yx, by = Cy + sx * Xy + sy * Yy, bz = Cz + sx * Xz + sy * Yz;
      float zx = sz * Zx, zy = sz * Zy, zz = sz * Zz;
      vb[i] = (LpV3){ bx - zx, by - zy, bz - zz };
      vb[i + 4] = (LpV3){ bx + zx, by + zy, bz + zz };
    }
    float dz = a20 * q->c.x + a21 * q->c.y + a22 * q->c.z + o.z;
    depth[b] = dz;
    int k = b - 1;                                                    // insertion sort, far first
    while (k >= 0 && depth[order[k]] > dz) { order[k + 1] = order[k]; k--; }
    order[k + 1] = b;
  }
#ifdef LPPROF
  uint32_t tB = TICKS_READ();
  g_lp_tt[0] += tB - tA;
  uint32_t tri0 = g_lp_tt[2] + g_lp_tt[3];
#endif
  if (anyTex) {
    LpPut(LP_SYNC_PIPE);
    LpPut(LP_TEX_IMAGE(LP_TEX, PhysicalAddr(g_lp_tex[texIdx])));
    LpPut(LP_SET_TILE(7, LP_TEX * 2 / 8, 4));
    LpPut(LP_SYNC_LOAD);
    LpPut(LP_LOAD_TILE(7, LP_TEX, LP_TEX));
    LpPut(LP_SYNC_TILE);
    LpPut(LP_SET_TILE(0, LP_TEX * 2 / 8, 4));
    LpPut(LP_TILE_SIZE(0, LP_TEX, LP_TEX));
  }
  const float lx = kLpLight.x, ly = kLpLight.y, lz = kLpLight.z;   // (already unit length)
  int mode = -1;
  uint32_t last = 0;
  for (int o = 0; o < n; o++) {
    int b = order[o];
    const LpBlock *bk = &m[b];
    const LpM3 *N = &Nm[bk->limb];
    int want = bk->mat == 0;
    uint32_t rgb = tint ? tint : bk->rgb;
    for (int f = 0; f < 6; f++) {
      if (pp[b].hide >> f & 1) continue;                              // buried in a neighbour
      LpV3 nl = pp[b].n[f];
      float nz = N->m[2][0] * nl.x + N->m[2][1] * nl.y + N->m[2][2] * nl.z;
      if (nz <= 1e-3f) continue;                                      // facing away
      if (want != mode) {
        LpPut(LP_SYNC_PIPE);
        LpPut(LP_OTHER_MODES(want ? LP_SOM_TEX : LP_SOM_FLAT));
        LpPut(want ? LP_CC_TEXPRIM : LP_CC_PRIM);
        mode = want, last = 0;
      }
      uint32_t col;
      if (bk->mat != 2 || tint) {
        float nx = N->m[0][0] * nl.x + N->m[0][1] * nl.y + N->m[0][2] * nl.z;
        float ny = N->m[1][0] * nl.x + N->m[1][1] * nl.y + N->m[1][2] * nl.z;
        float d = nx * lx + ny * ly + nz * lz;
        int k = 108 + (d > 0 ? (int)(d * 184.0f) : 0);               // 0.42 + 0.72 d, in 1/256
        int r = (int)(rgb >> 16 & 0xff) * k >> 8, g = (int)(rgb >> 8 & 0xff) * k >> 8, bl = (int)(rgb & 0xff) * k >> 8;
        col = (uint32_t)(r > 255 ? 255 : r) << 24 | (g > 255 ? 255 : g) << 16 | (bl > 255 ? 255 : bl) << 8 | 0xff;
      } else {
        col = rgb << 8 | 0xff;
      }
      if (col != last) { LpPut(LP_PRIM_COLOR(col)); last = col; }
      const uint8_t *q = kLpF[f];
      int ax = f >> 1;                                                // model axis along the face normal
      LpVert t[4];
      for (int j = 0; j < 4; j++) {
        const LpV3 *p = &v[b][q[j]];
        t[j].x = p->x, t[j].y = p->y;     // (already screen coordinates)
        if (want) {                                                   // planar mapping, one texel per 1.25 units
          const LpV3 *mm = &pp[b].v[q[j]];
          float s_, tt;
          if (ax == 0) s_ = mm->z, tt = mm->y;
          else if (ax == 1) s_ = mm->x, tt = mm->z;
          else s_ = mm->x, tt = mm->y;
          t[j].s = s_ * 0.8f, t[j].t = -tt * 0.8f;
        }
      }
      // skip slivers: a face seen almost edge-on covers well under a pixel
      float area = (t[2].x - t[0].x) * (t[3].y - t[1].y) - (t[3].x - t[1].x) * (t[2].y - t[0].y);
      if (area < 3.0f && area > -3.0f) continue;
      LpPutTri(&t[0], &t[1], &t[2], want, false);
      LpPutTri(&t[0], &t[2], &t[3], want, false);
    }
  }
  if (mode != 0) {   // leave flat mode for whatever is drawn next
    LpPut(LP_SYNC_PIPE);
    LpPut(LP_OTHER_MODES(LP_SOM_FLAT));
    LpPut(LP_CC_PRIM);
  }
#ifdef LPPROF
  g_lp_tt[1] += (TICKS_READ() - tB) - (g_lp_tt[2] + g_lp_tt[3] - tri0);
#endif
}

// ---- Opponents -------------------------------------------------------------------
static void LpDrawEnemies(const LpFrame *lf) {
  typedef struct { float z; uint32_t col; LpVert v[3]; } LpTriQ;
  static LpTriQ q[80];
  static LpV3 vv[42];
  for (int i = 0; i < lf->nen; i++) {
    const LpEnemy *e = &lf->en[i];
    if (lf->er1[i] <= lf->er0[i]) continue;
    if (e->model & 3) {   // Burrowdrake: feet on the sprite box's bottom, facing the hero
      float sc = LpDrakeScale(e);
      float ex = SNES_LEFT + (e->x0 + e->x1) * 0.5f;
      bool heroLeft = lf->h.valid && lf->h.x < (e->x0 + e->x1) / 2;
      LpPose ps = { {0, 0}, {0, 0}, 0, 0, 9.0f, 0, 0, sc, 1, false };
      ps.tail = 0.18f * sinf(LpWrap((float)(g_lp_ticks % 629) * 0.1f + i));
      LpPut(LP_SCISSOR(SNES_LEFT, SNES_TOP + lf->er0[i], SNES_LEFT + SNES_W, SNES_TOP + lf->er1[i]));
      uint32_t tint = (e->model & 4) ? (uint32_t)e->r << 16 | e->g << 8 | e->b : 0;
      LpDrawModel((e->model & 3) == 1 ? kDrakeGreen : kDrakeRed, LP_COUNT(kDrakeGreen), &ps, ex,
                  SNES_TOP + e->y1, heroLeft ? 3.14159265f - 0.6f : 0.6f, 1, tint);
      continue;
    }
    float sx, sy, sz;
    LpEnemySize(e, &sx, &sy, &sz);
    uint32_t hsh = e->id * 2654435761u;
    int kind = e->boss ? kLpSpiky : (int)((hsh >> 16) % 5);
    const LpMesh *m = &g_lp_mesh[kind];
    float cx = SNES_LEFT + (e->x0 + e->x1) * 0.5f, cy = SNES_TOP + (e->y0 + e->y1) * 0.5f;
    float spin = LpWrap((float)(g_lp_ticks % 4096) * (0.025f + (hsh >> 8 & 15) * 0.003f) + (hsh & 255) * 0.0245f);
    float cyw = cosf(spin), syw = sinf(spin);
    float tilt = 0.30f + 0.12f * sinf(LpWrap(spin * 0.7f)), ct = cosf(tilt), st = sinf(tilt);
    for (int k = 0; k < m->nv; k++) {
      LpV3 p = m->v[k];
      float x = p.x * cyw - p.z * syw, z = p.x * syw + p.z * cyw;   // spin
      float y = p.y * ct - z * st; z = p.y * st + z * ct;            // tilt
      vv[k] = (LpV3){ x * sx, y * sy, z * sz };
    }
    int nq = 0;
    for (int f = 0; f < m->nt; f++) {
      LpV3 a = vv[m->t[f][0]], b = vv[m->t[f][1]], c = vv[m->t[f][2]];
      LpV3 n = LpCross(a, b, c);
      LpV3 ctr = { a.x + b.x + c.x, a.y + b.y + c.y, a.z + b.z + c.z };
      if (n.x * ctr.x + n.y * ctr.y + n.z * ctr.z < 0) n.x = -n.x, n.y = -n.y, n.z = -n.z;   // outwards
      if (n.z <= 0) continue;                                   // facing away
      LpTriQ *t = &q[nq++];
      t->z = ctr.z;
      t->col = LpShade(e->r, e->g, e->b, LpLight(n));
      const LpV3 *pv[3] = { &a, &b, &c };
      for (int j = 0; j < 3; j++) t->v[j] = (LpVert){ cx + pv[j]->x, cy - pv[j]->y, 0, 0, 0 };
    }
    // back to front (insertion sort; the spiked shape isn't convex)
    for (int a = 1; a < nq; a++) {
      LpTriQ x = q[a];
      int b = a - 1;
      while (b >= 0 && q[b].z > x.z) { q[b + 1] = q[b]; b--; }
      q[b + 1] = x;
    }
    LpPut(LP_SCISSOR(SNES_LEFT, SNES_TOP + lf->er0[i], SNES_LEFT + SNES_W, SNES_TOP + lf->er1[i]));
    uint32_t last = 0;
    for (int a = 0; a < nq; a++) {
      if (q[a].col != last) { LpPut(LP_PRIM_COLOR(q[a].col)); last = q[a].col; }
      LpPutTri(&q[a].v[0], &q[a].v[1], &q[a].v[2], false, false);
    }
  }
}

// ---- Hero ------------------------------------------------------------------------
#define LP_HERO_SCALE 0.86f             // Kestrel is 49 units tall; Samus's standing sprite ~42 px
// The hero's pose for this frame (also advances its animation).
static LpPose LpHeroPose(const LpHero *h) {
  LpPose p = { {0, 0}, {0, 0}, 0, 0, 24.0f, 33.5f, 26.0f, LP_HERO_SCALE, 1, false };
  float target = h->left ? 3.14159265f - 0.6f : 0.6f;
  // (values that decay are snapped to their target: denormal floats make every
  // FPU instruction that touches them trap, thousands of cycles each)
  float dyaw = target - g_lp_yaw;
  g_lp_yaw = fabsf(dyaw) < 1e-3f ? target : g_lp_yaw + dyaw * 0.35f;
  int dx = g_lp_lastwx < 0 ? 0 : h->wx - g_lp_lastwx;
  g_lp_lastwx = h->wx;
  if (dx < -16 || dx > 16) dx = 0;                         // a room change
  p.scaleY = h->radius > 0 ? (2.0f * h->radius) / 42.0f : 1.0f;
  if (p.scaleY > 1.1f) p.scaleY = 1.1f;
  if (p.scaleY < 0.55f) p.scaleY = 0.55f;
  float swing = 0;
  switch (h->move) {
  case 0x04: case 0x08: case 0x11: case 0x12: case 0x13:   // ball
    p.ball = true;
    g_lp_ball = LpWrap(g_lp_ball + dx * (h->left ? 0.14f : -0.14f));
    break;
  case 0x03: case 0x14:                                     // spin jump, wall jump: tucked, rolling
    g_lp_roll = LpWrap(g_lp_roll - 0.42f);
    p.roll = g_lp_roll;
    p.leg[0] = 1.25f, p.leg[1] = 1.0f;
    p.scaleY = 1.0f;
    break;
  case 0x02: case 0x06: case 0x17: case 0x18:               // jumping, falling
    g_lp_roll = 0;
    p.leg[0] = 0.55f, p.leg[1] = -0.35f;
    break;
  default:
    g_lp_roll = 0;
    if (dx) g_lp_phase = LpWrap(g_lp_phase + (dx < 0 ? -dx : dx) * 0.2f);
    else g_lp_phase = fabsf(g_lp_phase) < 1e-3f ? 0 : g_lp_phase * 0.8f;
    swing = sinf(g_lp_phase) * 0.6f;
    p.leg[0] = swing, p.leg[1] = -swing;
    break;
  }
  // near arm (with the blaster) points along the shot direction: hanging down is
  // -90 degrees from forward (0 up ... 2 / 7 forward ... 4 / 5 down)
  static const float kAim[10] = { 1.5708f, 0.7854f, 0, -0.7854f, -1.5708f, -1.5708f, -0.7854f, 0, 0.7854f, 1.5708f };
  float aim = (unsigned)h->aim < 10 ? kAim[h->aim] : 0;
  if (p.roll != 0) aim = -0.6f;
  p.arm[0] = 1.5708f + aim;
  p.arm[1] = -swing * 0.6f;
  return p;
}

// Rows the hero may touch (feet-relative bounds for each kind of pose).
static void LpHeroRows(const LpHero *h, int *y0, int *y1) {
  int mv = h->move;
  bool ball = mv == 0x04 || mv == 0x08 || (mv >= 0x11 && mv <= 0x13), roll = mv == 0x03 || mv == 0x14;
  if (ball) *y0 = h->feetY - 2 * (h->radius > 0 ? h->radius : 7) - 4, *y1 = h->feetY + 4;
  else if (roll) *y0 = h->feetY - 22 - 36, *y1 = h->feetY - 22 + 36;
  else *y0 = h->feetY - 62, *y1 = h->feetY + 8;
}

static void LpDrawHero(const LpFrame *lf) {
  const LpHero *h = &lf->h;
  if (lf->hr1 <= lf->hr0) return;
  LpPose ps = LpHeroPose(h);
  if (!h->shown) return;
  float cx = SNES_LEFT + h->x, fy = SNES_TOP + h->feetY;
  LpPut(LP_SCISSOR(SNES_LEFT, SNES_TOP + lf->hr0, SNES_LEFT + SNES_W, SNES_TOP + lf->hr1));
  if (ps.ball) {   // ball: a faceted green / purple icosahedron with glowing panels, rolling along
    float cyw = cosf(g_lp_yaw), syw = sinf(g_lp_yaw);
    const LpMesh *m = &g_lp_mesh[kLpIco];
    float r = h->radius > 0 ? (float)h->radius : 7.0f;
    float cr = cosf(g_lp_ball), sr = sinf(g_lp_ball);
    LpV3 vv[12];
    for (int k = 0; k < 12; k++) {
      LpV3 p = m->v[k];
      float x = p.x * cr - p.y * sr, y = p.x * sr + p.y * cr, z = p.z;     // roll
      float rx = x * cyw - z * syw, rz = x * syw + z * cyw;
      float ry = y * LP_CP - rz * LP_SP; rz = y * LP_SP + rz * LP_CP;
      vv[k] = (LpV3){ rx * r, ry * r, rz * r };
    }
    for (int f = 0; f < 20; f++) {
      LpV3 a = vv[m->t[f][0]], b = vv[m->t[f][1]], c = vv[m->t[f][2]];
      LpV3 n = LpCross(a, b, c);
      LpV3 ctr = { a.x + b.x + c.x, a.y + b.y + c.y, a.z + b.z + c.z };
      if (n.x * ctr.x + n.y * ctr.y + n.z * ctr.z < 0) n.x = -n.x, n.y = -n.y, n.z = -n.z;
      if (n.z <= 0) continue;
      bool glow = (f % 5) == 0;
      uint32_t rgb = glow ? 0xd8ff3a : (f & 1) ? 0x3fa34d : 0x7a4fb0;
      LpPut(LP_PRIM_COLOR(LpShade(rgb >> 16, rgb >> 8 & 0xff, rgb & 0xff, glow ? 1.0f : LpLight(n))));
      LpVert t[3];
      const LpV3 *pv[3] = { &a, &b, &c };
      for (int j = 0; j < 3; j++) t[j] = (LpVert){ cx + pv[j]->x, fy - r - pv[j]->y, 0, 0, 0 };
      LpPutTri(&t[0], &t[1], &t[2], false, false);
    }
    return;
  }
  LpDrawModel(kKestrel, LP_COUNT(kKestrel), &ps, cx, fy, g_lp_yaw, 0, 0);
}

// ---- Sprites in 3D (the player, the opponents, the bosses) ----------------------------
extern uint8_t g_ram[];                 // the game's RAM (little-endian)
#define LP_BOSS_ID (g_ram[0x179C] | g_ram[0x179D] << 8)

// The 4 bit planes of row y of sprite character ch (plane p in bits 8p .. 8p+7,
// bit 7 of each the leftmost pixel). heroTiles: characters 0x00-0x1F come from
// the player's tiles for this frame (the ROM), the rest from VRAM.
const uint16_t *Snes_N64_Vram(void);    // (n64_glue.c)
int Snes_N64_BgLayout(int layer, uint16_t *tilemap, uint16_t *tiles, int *wtiles, int *htiles, bool *bigTiles);
static inline uint32_t LpTileRow(const uint16_t *vram, int ch, int y, bool heroTiles) {
  if (heroTiles && ch < 32 && (g_lp_hs.tmask >> ch & 1)) {
    const uint8_t *t = g_lp_hs.tile[ch];
    return t[2 * y] | t[2 * y + 1] << 8 | t[16 + 2 * y] << 16 | (uint32_t)t[17 + 2 * y] << 24;
  }
  unsigned base = 0x6000 + (ch & 0xff) * 16 + (ch >> 8) * 0x1000;   // OBSEL 3: characters at word 0x6000
  return vram[(base + y) & 0x7fff] | (uint32_t)vram[(base + 8 + y) & 0x7fff] << 16;
}
// Colour index (0-15) of pixel k (0: left) of a row of planes; hf: flipped.
static inline int LpPix(uint32_t r, int k, bool hf) {
  int sh = hf ? k : 7 - k;
  return (r >> sh & 1) | (r >> (sh + 7) & 2) | (r >> (sh + 14) & 4) | (r >> (sh + 21) & 8);
}

// SNES colour -> RGBA5551 through the colour filter (a few colours per sprite: cached).
static uint16_t LpTexColour(uint16_t c) {
  static uint16_t key[64], val[64];
  static int nk, nxt;
  static uint8_t cfmode = 0xff;
  if (cfmode != (uint8_t)g_ppur_cf_mode) { nk = 0; cfmode = (uint8_t)g_ppur_cf_mode; }   // filter changed
  for (int i = 0; i < nk; i++) if (key[i] == c) return val[i];
  int r = c & 31, g = c >> 5 & 31, b = c >> 10 & 31;
  uint32_t f = CfRGBA((uint32_t)(r << 3 | r >> 2) << 24 | (g << 3 | g >> 2) << 16 | (b << 3 | b >> 2) << 8 | 0xff);
  uint16_t v = (uint16_t)((f >> 27 & 31) << 11 | (f >> 19 & 31) << 6 | (f >> 11 & 31) << 1 | 1);
  int slot = nk < 64 ? nk++ : (nxt++ & 63);
  key[slot] = c, val[slot] = v;
  return v;
}

// Palette p (CGRAM 16p .. 16p + 15: 0-7 the BG palettes, 8-15 the sprite
// palettes) as this frame shows them (palette_buffer), through the colour
// filter: worked out when something first uses it.
static uint16_t g_lp_pal[256];
static unsigned g_lp_paldone;           // palettes worked out this frame (bit per palette)
static const uint16_t *LpPalette(int p) {
  if (!(g_lp_paldone >> p & 1)) {
    g_lp_paldone |= 1u << p;
    g_lp_pal[p * 16] = 0;               // colour 0: transparent
    for (int i = 1; i < 16; i++) {
      int k = 2 * (p * 16 + i);
      g_lp_pal[p * 16 + i] = LpTexColour((g_ram[0xC000 + k] | g_ram[0xC001 + k] << 8) & 0x7fff);
    }
  }
  return &g_lp_pal[p * 16];
}

// Thickness, in place. A texture holds per pixel its colour (palette slot * 16
// + colour, slot 0-3 of the object's own up to 4 palettes; 0: transparent) and,
// in bits 6-7, d - 1, d being the steps to the outline (chessboard distance:
// 1 at the outline ... LP_DCAP). A layer hides the pixels that don't reach it
// by blanking a quarter of the TLUT. LpThickness works d out in the window
// x0 .. x1, y0 .. y1 of a w x h texture (line tw), taking the pixels around
// the window as they are (two passes).
static inline int LpD(const uint8_t *t, int tw, int w, int h, int x, int y) {
  if ((unsigned)x >= (unsigned)w || (unsigned)y >= (unsigned)h) return 0;
  uint8_t v = t[y * tw + x];
  return v ? (v >> 6) + 1 : 0;
}
static void LpThickness(uint8_t *t, int tw, int w, int h, int x0, int y0, int x1, int y1) {
  for (int y = y0; y < y1; y++) {
    uint8_t *row = t + y * tw;
    for (int x = x0; x < x1; x++) {
      if (!row[x]) continue;
      int m = LpD(t, tw, w, h, x - 1, y), k;
      if ((k = LpD(t, tw, w, h, x - 1, y - 1)) < m) m = k;
      if ((k = LpD(t, tw, w, h, x, y - 1)) < m) m = k;
      if ((k = LpD(t, tw, w, h, x + 1, y - 1)) < m) m = k;
      if (m > LP_DCAP - 1) m = LP_DCAP - 1;
      row[x] = (uint8_t)((row[x] & 63) | m << 6);
    }
  }
  for (int y = y1 - 1; y >= y0; y--) {
    uint8_t *row = t + y * tw;
    for (int x = x1 - 1; x >= x0; x--) {
      if (!row[x]) continue;
      int m = row[x] >> 6, k;
      if ((k = LpD(t, tw, w, h, x + 1, y)) < m) m = k;
      if ((k = LpD(t, tw, w, h, x - 1, y + 1)) < m) m = k;
      if ((k = LpD(t, tw, w, h, x, y + 1)) < m) m = k;
      if ((k = LpD(t, tw, w, h, x + 1, y + 1)) < m) m = k;
      row[x] = (uint8_t)((row[x] & 63) | m << 6);
    }
  }
}

// ---- Bosses ------------------------------------------------------------------------
// Kraid, Crocomire, Phantoon and Mother Brain's body are drawn on BG2 (the game
// turns X-ray off for these boss IDs for that reason). Their BG2 pixels are
// kept in a canvas (BG2 tile coordinates, the box of tiles with pixels), built
// once and then only where tiles change (tilemap entry, or a write to the
// tile's VRAM page). Bosses drawn from sprites too big for the opponents'
// budget (Ridley, Draygon...) use the same memory as two frame pools.
#define LP_BOSS_TEX (2 * LP_BIGPOOL)    // bytes (CI8)
#define LP_BOSS_TILES 1024
static struct {
  uint8_t *mem;                         // NULL: no memory for it (bosses stay 2D)
  bool valid;                           // the canvas holds the current boss
  uint16_t tm, ta;                      // BG2 tilemap / tile words it was built from
  int W, H;                             // tilemap size (tiles)
  int bx0, by0, bw, bh, tw;             // canvas: box (tiles), line (pixels)
  uint32_t tmgen;                       // the tilemap's pages' write counts, summed
  uint32_t stamp;                       // counts changes to the canvas
  int np;
  uint8_t pals[LP_MAXPAL];
  int8_t slot[8];
  uint16_t ent[LP_BOSS_TILES];          // per canvas tile: tilemap entry it was built from,
  uint32_t gen[LP_BOSS_TILES];          // its tile page's write count,
  uint8_t tf[LP_BOSS_TILES];            // 1: has pixels, 2: priority
} g_lp_boss;
static bool LpBgBossId(int id) { return id == 3 || id == 6 || id == 7 || id == 10; }
static bool LpBigPoolOk(void) { return g_lp_boss.mem && LP_BOSS_ID && !LpBgBossId(LP_BOSS_ID); }
static uint8_t *LpBigPool(const LpFrame *lf) { return g_lp_boss.mem + (lf - g_lp_frames) * LP_BIGPOOL; }
// The bosses' memory, only if the heap keeps a margin after it (freekb < 0: unknown, the PC test).
#define LP_BOSS_MARGIN_KB 48
static bool LpBossInit(int freekb) {
  if (freekb >= 0 && freekb < LP_BOSS_TEX / 1024 + LP_BOSS_MARGIN_KB) return false;
  g_lp_boss.mem = (uint8_t *)memalign(16, LP_BOSS_TEX);
  return g_lp_boss.mem != NULL;
}

static inline unsigned LpTmAdr(uint16_t tm, int W, int tx, int ty) {
  return (tm + ((ty & 31) << 5) + (tx & 31) + ((tx & 32) ? 0x400 : 0) + ((ty & 32) ? (W == 64 ? 0x800 : 0x400) : 0)) & 0x7fff;
}
static bool LpCharEmpty(const uint16_t *vram, uint16_t ta, int ch) {
  uint16_t o = 0;
  for (int i = 0; i < 16; i++) o |= vram[(ta + ch * 16 + i) & 0x7fff];
  return !o;
}
static uint32_t LpPageGenSum(unsigned adr, int words) {
  uint32_t s = 0;
  for (unsigned p = adr >> 7; p <= (adr + words - 1) >> 7; p++) s += g_ppur_pagegen[p & 255];
  return s;
}
// BG pixel row a line record shows for a layer (tilemap tm, tiles ta, W tiles wide).
static int LpBgY(const PpuLayerRec *L, uint16_t tm, uint16_t ta, int W) {
  unsigned off = (unsigned)(L->tm0 - tm) & 0x7fff, blk = W == 64 ? 0x800 : 0x400;
  int ty = 0;
  if (off >= blk) ty = 32, off -= blk;
  return (ty + (int)((off >> 5) & 31)) * 8 + ((L->ta0 - ta) & 7);
}
static int LpBossSlot(int pal) {
  if (g_lp_boss.slot[pal] >= 0) return g_lp_boss.slot[pal];
  if (g_lp_boss.np >= LP_MAXPAL) return -1;
  g_lp_boss.pals[g_lp_boss.np] = (uint8_t)pal;
  return g_lp_boss.slot[pal] = (int8_t)g_lp_boss.np++;
}
// Canvas tile cx, cy from tilemap entry e. False: a fifth palette.
static bool LpBossTile(const uint16_t *vram, int cx, int cy, uint16_t e) {
  int sl = LpBossSlot(e >> 10 & 7);
  if (sl < 0) return false;
  int tw = g_lp_boss.tw, any = 0;
  uint8_t *dst = g_lp_boss.mem + cy * 8 * tw + cx * 8;
  unsigned base = g_lp_boss.ta + (e & 0x3ff) * 16;
  bool hf = e & 0x4000, vf = e & 0x8000;
  for (int r = 0; r < 8; r++) {
    int sr = vf ? 7 - r : r;
    uint32_t p = vram[(base + sr) & 0x7fff] | (uint32_t)vram[(base + 8 + sr) & 0x7fff] << 16;
    uint8_t *d = dst + r * tw;
    for (int k = 0; k < 8; k++) {
      int ci = LpPix(p, k, hf);
      d[k] = ci ? (uint8_t)(sl * 16 + ci) : 0;
      any |= ci;
    }
  }
  g_lp_boss.tf[cy * g_lp_boss.bw + cx] = (uint8_t)((any ? 1 : 0) | (e & 0x2000 ? 2 : 0));
  return true;
}
// Brings the canvas up to date with BG2. False: no boss there to draw.
static bool LpBossUpdate(const uint16_t *vram) {
  uint16_t tm, ta;
  int W, H;
  bool big;
  if ((Snes_N64_BgLayout(1, &tm, &ta, &W, &H, &big) & 7) != 1 || big) return g_lp_boss.valid = false;
  bool full = !g_lp_boss.valid || tm != g_lp_boss.tm || ta != g_lp_boss.ta || W != g_lp_boss.W || H != g_lp_boss.H;
  uint32_t gsum = LpPageGenSum(tm, W * H);
  if (full || gsum != g_lp_boss.tmgen) {   // the tilemap was written: where are the boss's tiles now?
    int x0 = W, y0 = H, x1 = -1, y1 = -1;
    for (int ty = 0; ty < H; ty++)
      for (int tx = 0; tx < W; tx++) {
        uint16_t e = vram[LpTmAdr(tm, W, tx, ty)];
        if (LpCharEmpty(vram, ta, e & 0x3ff)) continue;
        if (tx < x0) x0 = tx;
        if (tx > x1) x1 = tx;
        if (ty < y0) y0 = ty;
        if (ty > y1) y1 = ty;
      }
    g_lp_boss.tmgen = gsum;
    if (x1 < 0) return g_lp_boss.valid = false;
    int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
    if (bw * 8 > LP_BIG_GW || bw * bh > LP_BOSS_TILES || bw * 8 * bh * 8 > LP_BOSS_TEX) return g_lp_boss.valid = false;
    if (x0 != g_lp_boss.bx0 || y0 != g_lp_boss.by0 || bw != g_lp_boss.bw || bh != g_lp_boss.bh) full = true;
    g_lp_boss.bx0 = x0, g_lp_boss.by0 = y0, g_lp_boss.bw = bw, g_lp_boss.bh = bh;
  }
  g_lp_boss.tm = tm, g_lp_boss.ta = ta, g_lp_boss.W = W, g_lp_boss.H = H;
  int bw = g_lp_boss.bw, bh = g_lp_boss.bh;
  if (full) {
    g_lp_boss.tw = bw * 8;
    g_lp_boss.np = 0;
    memset(g_lp_boss.slot, -1, sizeof(g_lp_boss.slot));
  }
  int dx0 = bw, dy0 = bh, dx1 = -1, dy1 = -1;   // tiles rebuilt
  for (int cy = 0; cy < bh; cy++)
    for (int cx = 0; cx < bw; cx++) {
      int i = cy * bw + cx;
      uint16_t e = vram[LpTmAdr(tm, W, g_lp_boss.bx0 + cx, g_lp_boss.by0 + cy)];
      uint32_t cg = g_ppur_pagegen[((ta + (e & 0x3ff) * 16) >> 7) & 255];
      if (!full && e == g_lp_boss.ent[i] && cg == g_lp_boss.gen[i]) continue;
      if (!LpBossTile(vram, cx, cy, e)) return g_lp_boss.valid = false;
      g_lp_boss.ent[i] = e, g_lp_boss.gen[i] = cg;
      if (cx < dx0) dx0 = cx;
      if (cx > dx1) dx1 = cx;
      if (cy < dy0) dy0 = cy;
      if (cy > dy1) dy1 = cy;
    }
  g_lp_boss.valid = true;
  if (dx1 < 0) return true;             // nothing changed
  // the thickness where it may have changed (up to LP_DCAP pixels around)
  int w = bw * 8, h = bh * 8;
  int x0 = dx0 * 8 - LP_DCAP, y0 = dy0 * 8 - LP_DCAP, x1 = dx1 * 8 + 8 + LP_DCAP, y1 = dy1 * 8 + 8 + LP_DCAP;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > w) x1 = w;
  if (y1 > h) y1 = h;
  LpThickness(g_lp_boss.mem, g_lp_boss.tw, w, h, x0, y0, x1, y1);
  data_cache_hit_writeback(g_lp_boss.mem + y0 * g_lp_boss.tw, (((y1 - y0) * g_lp_boss.tw) + 15) & ~15);
  g_lp_boss.stamp++;                    // (a frame captured before this doesn't draw it: the 2D boss shows)
  return true;
}

// Is any of the boss's pixels under the 8 x 8 square at x, y (game picture),
// on a canvas tile of priority <= maxp (canvas at sx, sy)?
static bool LpBossUnder(int x, int y, int sx, int sy, int maxp) {
  int cx0 = (x - sx) >> 3, cy0 = (y - sy) >> 3, cx1 = (x - sx + 7) >> 3, cy1 = (y - sy + 7) >> 3;
  for (int cy = cy0; cy <= cy1; cy++)
    for (int cx = cx0; cx <= cx1; cx++) {
      if ((unsigned)cx >= (unsigned)g_lp_boss.bw || (unsigned)cy >= (unsigned)g_lp_boss.bh) continue;
      uint8_t f = g_lp_boss.tf[cy * g_lp_boss.bw + cx];
      if ((f & 1) && (f >> 1) <= maxp) return true;
    }
  return false;
}
// A piece (8 x 8, drawn over the boss as it is): CGRAM colour indices into the strip.
static bool LpAddPiece(LpFrame *lf, int x, int y, const uint16_t *vram, unsigned base, int pal, bool hf, bool vf) {
  if (lf->npc >= LP_MAXPC || lf->ntex + 64 > LP_TEXPOOL) return false;
  uint8_t *d = lf->tex + lf->ntex;
  for (int r = 0; r < 8; r++) {
    int sr = vf ? 7 - r : r;
    uint32_t p = vram[(base + sr) & 0x7fff] | (uint32_t)vram[(base + 8 + sr) & 0x7fff] << 16;
    for (int k = 0; k < 8; k++) {
      int ci = LpPix(p, k, hf);
      d[r * 8 + k] = ci ? (uint8_t)(pal * 16 + ci) : 0;
    }
  }
  LpPalette(pal);
  lf->pc[lf->npc].x = (int16_t)x, lf->pc[lf->npc].y = (int16_t)y;
  lf->npc++;
  lf->ntex += 64;
  return true;
}
// What the 2D picture shows in front of the boss, over it: BG1 tiles of at
// least its priority, and sprites of priority 3 (or 2, over its priority-0
// tiles). They are drawn again after the 3D boss.
static void LpBossPieces(LpFrame *lf, const PpuFrame *f, const uint16_t *vram, int sx, int sy) {
  int X0 = sx < 0 ? 0 : sx, Y0 = sy < LP_HUD ? LP_HUD : sy;
  int X1 = sx + g_lp_boss.bw * 8, Y1 = sy + g_lp_boss.bh * 8;
  if (X1 > SNES_W) X1 = SNES_W;
  if (Y1 > SNES_H) Y1 = SNES_H;
  if (X1 <= X0 || Y1 <= Y0) return;
  // BG1, at the scroll of its first line over the boss
  uint16_t tm, ta;
  int W, H;
  bool big;
  Snes_N64_BgLayout(0, &tm, &ta, &W, &H, &big);
  int n = f->numLines < SNES_H ? f->numLines : SNES_H;
  for (int l = Y0; l < Y1 && l < n && !big; l++) {
    const PpuLineRec *r = &f->lines[l];
    if (!(r->flags & kLR_Valid) || (r->flags & (kLR_ForcedBlank | kLR_Mode7 | kLR_CpuDrawn)) || !(r->drawMask & 1)) continue;
    int hs = r->bg[0].hscroll & (W * 8 - 1), vo = LpBgY(&r->bg[0], tm, ta, W) - l;
    for (int ty = (Y0 + vo) >> 3; ty <= (Y1 - 1 + vo) >> 3; ty++)
      for (int tx = (X0 + hs) >> 3; tx <= (X1 - 1 + hs) >> 3; tx++) {
        uint16_t e = vram[LpTmAdr(tm, W, tx & (W - 1), ty & (H - 1))];
        int x = tx * 8 - hs, y = ty * 8 - vo;
        if (!LpBossUnder(x, y, sx, sy, e >> 13 & 1) || LpCharEmpty(vram, ta, e & 0x3ff)) continue;
        if (!LpAddPiece(lf, x, y, vram, ta + (e & 0x3ff) * 16, e >> 10 & 7, e & 0x4000, e & 0x8000)) goto sprites;
      }
    break;
  }
sprites:
  for (int i = 0; i < 128; i++) {       // the sprites still in OAM (the 3D ones were taken out)
    const uint8_t *o = &g_ram[0x370 + 4 * i];
    int ext = (g_ram[0x570 + 2 * (i >> 3)] | g_ram[0x571 + 2 * (i >> 3)] << 8) >> (2 * (i & 7)) & 3;
    int y = o[1], fl = o[3], prio = fl >> 4 & 3;
    if (y >= 0xE0 || prio < 2) continue;
    int x = o[0] | (ext & 1) << 8, sz = ext & 2 ? 2 : 1;
    if (x >= 256) x -= 512;
    if (x + sz * 8 <= X0 || x >= X1 || y + sz * 8 <= Y0 || y >= Y1) continue;
    bool hf = fl & 0x40, vf = fl & 0x80;
    int ch = o[2] | (fl & 1) << 8;
    for (int qy = 0; qy < sz; qy++)
      for (int qx = 0; qx < sz; qx++) {
        int px = x + qx * 8, py = y + qy * 8;
        if (!LpBossUnder(px, py, sx, sy, prio == 3 ? 1 : 0)) continue;
        int sxq = hf ? sz - 1 - qx : qx, syq = vf ? sz - 1 - qy : qy;
        int c = (ch & 0x100) | ((((ch >> 4) + syq) & 15) << 4) | ((ch + sxq) & 15);
        unsigned base = 0x6000 + (c & 0xff) * 16 + (c >> 8) * 0x1000;
        if (!LpAddPiece(lf, px, py, vram, base, 8 + (fl >> 1 & 7), hf, vf)) return;
      }
  }
}
// After the last logic step: the BG2 boss, if there is one to draw.
static void LpBossCapture(LpFrame *lf, const PpuFrame *f) {
  lf->boss.on = false;
  if (!g_lp_boss.mem || !f || !LpBgBossId(LP_BOSS_ID)) return;
  const uint16_t *vram = Snes_N64_Vram();
  if (!LpBossUpdate(vram)) return;
  // Where BG2 is: one scroll on every line it shows on (else, during a wave
  // effect, the boss stays 2D)
  int Wpx = g_lp_boss.W * 8, Hpx = g_lp_boss.H * 8, hs = -1, vo = 0;
  int n = f->numLines < SNES_H ? f->numLines : SNES_H;
  for (int l = LP_HUD; l < n; l++) {
    const PpuLineRec *r = &f->lines[l];
    if (!(r->flags & kLR_Valid) || (r->flags & (kLR_ForcedBlank | kLR_Mode7 | kLR_CpuDrawn)) || !(r->drawMask & 2)) continue;
    int h = r->bg[1].hscroll & (Wpx - 1), v = (LpBgY(&r->bg[1], g_lp_boss.tm, g_lp_boss.ta, g_lp_boss.W) - l) & (Hpx - 1);
    if (hs < 0) hs = h, vo = v;
    else if (h != hs || v != vo) return;
  }
  if (hs < 0) return;
  int sx = (g_lp_boss.bx0 * 8 - hs) & (Wpx - 1), sy = (g_lp_boss.by0 * 8 - vo) & (Hpx - 1);
  if (sx >= SNES_W) sx -= Wpx;
  if (sy >= SNES_H) sy -= Hpx;
  int gw = g_lp_boss.bw * 8, gh = g_lp_boss.bh * 8;
  if (sx + gw <= 0 || sx >= SNES_W) return;
  if (!LpRows(lf, sy - 4, sy + gh + 4, &lf->boss.row0, &lf->boss.row1)) return;
  memset(lf->bossct, 0, sizeof(lf->bossct));
  for (int k = 0; k < g_lp_boss.np; k++) {
    const uint16_t *pc = LpPalette(g_lp_boss.pals[k]);
    for (int j = 1; j < 16; j++) lf->bossct[k * 16 + j] = pc[j];
  }
  data_cache_hit_writeback(lf->bossct, sizeof(lf->bossct));
  lf->boss.on = true, lf->boss.x = (int16_t)sx, lf->boss.y = (int16_t)sy, lf->boss.stamp = g_lp_boss.stamp;
  LpBossPieces(lf, f, vram, sx, sy);
}

// What the 2D picture shows in front of the player, drawn again over the 3D
// player: BG1 / BG2 tiles that win over her sprites' priority (in mode 1,
// priority-1 tiles are in front of priority-2 sprites, and every BG1 / BG2
// tile is in front of priority 0-1 sprites), and the sprites written to OAM
// before hers (lower OAM entries are in front: the ship when she gets in or
// out of it, ...).
static void LpHeroPieces(LpFrame *lf, const PpuFrame *f, const uint16_t *vram) {
  const LpObj *o = &lf->obj[0];
  int prio = 0;
  for (int i = 0; i < g_lp_hs.n; i++) {
    int p = g_lp_hs.s[i].attr >> 4 & 3;
    if (p > prio) prio = p;
  }
  int X0 = o->gx - 3, Y0 = o->gy - 3, X1 = o->gx + o->gw + 3, Y1 = o->gy + o->gh + 3;   // (+ the layers' shift)
  if (X0 < 0) X0 = 0;
  if (Y0 < LP_HUD) Y0 = LP_HUD;
  if (X1 > SNES_W) X1 = SNES_W;
  if (Y1 > SNES_H) Y1 = SNES_H;
  if (X1 <= X0 || Y1 <= Y0) return;
  int n = f->numLines < SNES_H ? f->numLines : SNES_H;
  for (int layer = 0; layer < 2 && prio < 3; layer++) {
    uint16_t tm, ta;
    int W, H;
    bool big;
    if ((Snes_N64_BgLayout(layer, &tm, &ta, &W, &H, &big) & 7) != 1 || big) break;
    for (int l = Y0; l < Y1 && l < n; l++) {   // at the scroll of its first line there
      const PpuLineRec *r = &f->lines[l];
      if (!(r->flags & kLR_Valid) || (r->flags & (kLR_ForcedBlank | kLR_Mode7 | kLR_CpuDrawn)) || !(r->drawMask & (1 << layer))) continue;
      int hs = r->bg[layer].hscroll & (W * 8 - 1), vo = LpBgY(&r->bg[layer], tm, ta, W) - l;
      for (int ty = (Y0 + vo) >> 3; ty <= (Y1 - 1 + vo) >> 3; ty++)
        for (int tx = (X0 + hs) >> 3; tx <= (X1 - 1 + hs) >> 3; tx++) {
          uint16_t e = vram[LpTmAdr(tm, W, tx & (W - 1), ty & (H - 1))];
          if ((prio == 2 && !(e & 0x2000)) || LpCharEmpty(vram, ta, e & 0x3ff)) continue;
          if (!LpAddPiece(lf, tx * 8 - hs, ty * 8 - vo, vram, ta + (e & 0x3ff) * 16, e >> 10 & 7, e & 0x4000, e & 0x8000)) return;
        }
      break;
    }
  }
  for (int i = 0; i < g_lp_hs.first && i < 128; i++) {   // the sprites before hers
    const uint8_t *e = &g_ram[0x370 + 4 * i];
    int ext = (g_ram[0x570 + 2 * (i >> 3)] | g_ram[0x571 + 2 * (i >> 3)] << 8) >> (2 * (i & 7)) & 3;
    int y = e[1], fl = e[3];
    if (y >= 0xE0) continue;
    int x = e[0] | (ext & 1) << 8, sz = ext & 2 ? 2 : 1;
    if (x >= 256) x -= 512;
    if (x + sz * 8 <= X0 || x >= X1 || y + sz * 8 <= Y0 || y >= Y1) continue;
    bool hf = fl & 0x40, vf = fl & 0x80;
    int ch = e[2] | (fl & 1) << 8;
    for (int qy = 0; qy < sz; qy++)
      for (int qx = 0; qx < sz; qx++) {
        int px = x + qx * 8, py = y + qy * 8;
        if (px + 8 <= X0 || px >= X1 || py + 8 <= Y0 || py >= Y1) continue;
        int sxq = hf ? sz - 1 - qx : qx, syq = vf ? sz - 1 - qy : qy;
        int c = (ch & 0x100) | ((((ch >> 4) + syq) & 15) << 4) | ((ch + sxq) & 15);
        if (!LpAddPiece(lf, px, py, vram, 0x6000 + (c & 0xff) * 16 + (c >> 8) * 0x1000, 8 + (fl >> 1 & 7), hf, vf)) return;
      }
  }
}

// After the last logic step: one sprite group (OAM entries s[0 .. n)) as an
// object, added to the frame. maxW / maxH clip the grid. big: in the big pool
// (bosses from sprites). False: no room.
static bool LpBuildSprite(LpFrame *lf, const LpSpr *s, int n, int maxW, int maxH, bool heroTiles, bool big) {
  if (!n || lf->nobj >= LP_MAXOBJ) return false;
  int b[4];
  LpSprBox(s, n, b);
  int x0 = b[0], y0 = b[1];
  int gw = b[2] - x0, gh = b[3] - y0;
  if (gw > maxW) gw = maxW;
  if (gh > maxH) gh = maxH;
  if (gw <= 0 || gh <= 0) return false;
  int tw = (gw + 7) & ~7, bh = LP_TEXELS / tw;   // (CI8 lines are whole 64-bit words)
  if (bh > gh) bh = gh;
  int *used = big ? &lf->ntexb : &lf->ntex;
  if (*used + tw * gh > (big ? LP_BIGPOOL : LP_TEXPOOL)) return false;
  uint8_t *tex = (big ? LpBigPool(lf) : lf->tex) + *used;
  memset(tex, 0, tw * gh);
  // Its palettes: slots 0-3 (a fifth one shares the last slot; opponents with
  // more than four were already left in 2D by LowPoly_EndEnemy).
  int8_t slot[8];
  uint8_t pals[LP_MAXPAL];
  int np = 0;
  memset(slot, -1, sizeof(slot));
  for (int i = 0; i < n; i++) {
    int p = s[i].attr >> 1 & 7;
    if (slot[p] < 0) {
      if (np < LP_MAXPAL) pals[np] = (uint8_t)p, slot[p] = (int8_t)np++;
      else slot[p] = LP_MAXPAL - 1;
    }
  }
  // The pixels, straight into the texture, a tile row (8 pixels) at a time.
  // Lower OAM entries are in front: the list is drawn backwards.
  const uint16_t *vram = Snes_N64_Vram();
  for (int i = n - 1; i >= 0; i--) {
    const LpSpr *e = &s[i];
    int sz = e->big ? 16 : 8, base = slot[e->attr >> 1 & 7] * 16;
    bool hf = e->attr & 0x40, vf = e->attr & 0x80;
    for (int ty = 0; ty < sz; ty++) {
      int gy = e->y + ty - y0;
      if (gy < 0 || gy >= gh) continue;
      int sy = vf ? sz - 1 - ty : ty;
      uint8_t *row = tex + gy * tw;
      for (int cx = 0; cx < sz; cx += 8) {   // source tile column
        int ch = (e->ch & 0x100) | ((((e->ch >> 4) + (sy >> 3)) & 15) << 4) | ((e->ch + (cx >> 3)) & 15);
        uint32_t r = LpTileRow(vram, ch, sy & 7, heroTiles);
        if (!r) continue;
        for (int k = 0; k < 8; k++) {
          int ci = LpPix(r, k, false);
          if (!ci) continue;
          int sx = cx + k, gx = e->x + (hf ? sz - 1 - sx : sx) - x0;
          if ((unsigned)gx < (unsigned)gw) row[gx] = (uint8_t)(base + ci);
        }
      }
    }
  }
  LpThickness(tex, tw, gw, gh, 0, 0, gw, gh);
  // Its colours: slot k is TLUT entries 16k .. 16k + 15 of each class.
  uint16_t *ct = lf->ctab[lf->nobj];
  memset(ct, 0, 64 * 2);
  for (int k = 0; k < np; k++) {
    const uint16_t *pc = LpPalette(8 + pals[k]);
    for (int j = 1; j < 16; j++) ct[k * 16 + j] = pc[j];
  }
  data_cache_hit_writeback(ct, 64 * 2);
  data_cache_hit_writeback(tex, ((tw * gh) + 15) & ~15);
  LpObj *o = &lf->obj[lf->nobj++];
  *o = (LpObj){ (int16_t)x0, (int16_t)y0, (int16_t)gw, (int16_t)gh, (int16_t)tw, (int16_t)bh, (uint16_t)*used, 0, 0, big };
  *used += tw * gh;
  if (x0 + gw > -8 && x0 < SNES_W + 8)
    LpRows(lf, y0 - 4, y0 + gh + 4, &o->row0, &o->row1);    // + the layers' shift
  return true;
}

// The layers, back to front: depth z along the view (+ towards the viewer;
// the outline's pixels span -1.6 .. 1.6, the innermost reach 5.2), the grey
// the texture is multiplied by (0: the side shade, these mostly show as the
// figure's sides and top), and how many thickness levels are hidden (the
// layer at z shows the pixels whose thickness reaches it).
typedef struct { float z, k; int8_t hide; } LpLayer;
static const LpLayer kLpLayers[] = {
  { -1.6f, 0, 0 }, { 0.0f, 0, 0 },      // back half of the outline
  { 1.6f, 0.86f, 0 },                   // front: the outline (d >= 1)
  { 2.8f, 0.90f, 1 },                   // d >= 2
  { 4.0f, 0.95f, 2 },                   // d >= 3
  { 5.2f, 1.00f, 3 },                   // d >= 4
};
static inline uint64_t LpGrey(float k) { return 0x3Aull << 56 | (uint32_t)((int)(255.0f * k) * 0x01010100u) | 0xff; }

// n TLUT entries from src (RDRAM) to TLUT entries idx ..
static void LpTlut(int idx, const uint16_t *src, int n) {
  LpPut(LP_TEX_IMAGE(256, PhysicalAddr(src)));
  LpPut(LP_SET_TILE_TLUT(7, idx));
  LpPut(LP_SYNC_LOAD);
  LpPut(LP_LOAD_TLUT(7, n));
  LpPut(LP_SYNC_TILE);
#ifdef LP_HOST_TEST
  LpHostTlut(idx, src, n);
#endif
}
// Rows 0 .. rows of a CI8 texture (line tw texels) into TMEM, as tile 0.
static void LpLoadTex(const uint8_t *tex, int tw, int rows) {
  LpPut(LP_SYNC_PIPE);
  LpPut(LP_TEX_IMAGE_CI8(tw, PhysicalAddr(tex)));
  LpPut(LP_SET_TILE_CI8(7, tw / 8));
  LpPut(LP_SYNC_LOAD);
  LpPut(LP_LOAD_TILE(7, tw, rows));
  LpPut(LP_SYNC_TILE);
  LpPut(LP_SET_TILE_CI8(0, tw / 8));
  LpPut(LP_TILE_SIZE(0, tw, rows));
#ifdef LP_HOST_TEST
  LpHostTex(tex, tw, rows);
#endif
}
// Texture rectangle with tile 0, 1:1, its top left texel at s, t (10.5);
// 10.2 screen coordinates.
static void LpTexRect(int x0, int y0, int x1, int y1, int s, int t) {
  if (x0 < 0) s += -x0 * 8, x0 = 0;     // (10.2 -> 10.5)
  if (y0 < 0) t += -y0 * 8, y0 = 0;
  if (x1 <= x0 || y1 <= y0) return;
#ifndef LP_HOST_TEST
  if (g_lp_wend - g_lp_w < 2) LpSubmit(true);   // (never split across the buffers)
#endif
  LpPut(LP_TEXRECT0(x0, y0, x1, y1, 0));
  LpPut(LP_TEXRECT1(s, t));
  g_lp_ntri++;
#ifdef LP_HOST_TEST
  LpHostRect(x0, y0, x1, y1, s, t);
#endif
}

// A gw x gh texture (line tw, bands of bh rows) at x0, y0 on the game picture,
// as layers, turned by yaw (> 0: the right side shows), scissored to rows
// row0 .. row1. za: the depth that stays in place (0: the middle; the outline's
// front for a boss whose 2D picture is under it). Each band: the texture, its
// colours (ct, 64 entries) four times, then the layers, each one texture
// rectangle. The bands go bottom to top: a band's nearer layers reach down
// over the band below, never up.
static void LpDrawLayers(const uint8_t *tex, int tw, int bh, int gw, int gh, int x0, int y0,
                         const uint16_t *ct, float yaw, float za, int row0, int row1) {
  if (row1 <= row0) return;
  float dxz = -sinf(yaw), dyz = LP_SP * cosf(yaw);   // screen shift per unit of depth
  float side = dxz < 0 ? 0.62f : 0.72f;              // the right side is in the shade
  LpPut(LP_SCISSOR(SNES_LEFT, SNES_TOP + row0, SNES_LEFT + SNES_W, SNES_TOP + row1));
  int nb = (gh + bh - 1) / bh;
  for (int b = nb - 1; b >= 0; b--) {
    int t0 = b * bh, rows = gh - t0 < bh ? gh - t0 : bh;
    if (y0 + t0 + rows + 2 <= row0 || y0 + t0 - 2 >= row1) continue;   // (shifted at most a pixel or two)
    LpLoadTex(tex + t0 * tw, tw, rows);
    for (int k = 0; k < 4; k++) LpTlut(64 * k, ct, 64);   // every thickness shows
    int hidden = 0;
    for (int l = 0; l < LP_COUNT(kLpLayers); l++) {
      const LpLayer *L = &kLpLayers[l];
      while (hidden < L->hide) { hidden++; LpTlut(64 * (hidden - 1), g_lp_zero, 64); }   // d = hidden: gone
      LpPut(LpGrey(L->k > 0 ? L->k : side * (l == 0 ? 0.85f : 1.0f)));
      float z = L->z - za;
      int sx = SNES_LEFT + x0 + LpFloorI(dxz * z + 0.5f), sy = SNES_TOP + y0 + t0 + LpFloorI(dyz * z + 0.5f);
      LpTexRect(sx * 4, sy * 4, (sx + gw) * 4, (sy + rows) * 4, 0, 0);
    }
  }
}
static void LpDrawSprite(const LpFrame *lf, int i, float yaw) {
  const LpObj *ob = &lf->obj[i];
  const uint8_t *tex = (ob->big ? LpBigPool(lf) : lf->tex) + ob->tex;
  LpDrawLayers(tex, ob->tw, ob->bh, ob->gw, ob->gh, ob->gx, ob->gy, lf->ctab[i], yaw, 0, ob->row0, ob->row1);
}
static inline float LpYawAt(float cx) {  // turned by where it is on the screen: the side towards the middle shows
  float k = (SNES_W * 0.5f - cx) * (0.35f / 128);
  return k > 0.35f ? 0.35f : k < -0.35f ? -0.35f : k;
}

// Pieces from .. to of the 2D picture, as they are (8 x 8 each), scissored to rows row0 .. row1.
#ifndef LP_HOST_TEST
// The pieces' pixels, taken from the finished picture (before any 3D is drawn
// on it) where the piece's tile has pixels: exactly what the 2D picture shows
// there, colour math (fog, rain, water...) included; transparent elsewhere.
// RGBA5551, 8 x 8 each, for this draw only.
static uint16_t g_lp_pcpix[LP_MAXPC * 64] __attribute__((aligned(16)));
static void LpPiecesFromPicture(const LpFrame *lf, const surface_t *fb) {
  const uint16_t *px = (const uint16_t *)fb->buffer;   // (uncached: the RSP drew it)
  int stride = fb->stride / 2;
  for (int i = 0; i < lf->npc; i++) {
    const uint8_t *m = lf->tex + lf->pctex + i * 64;
    uint16_t *d = &g_lp_pcpix[i * 64];
    int x0 = SNES_LEFT + lf->pc[i].x, y0 = SNES_TOP + lf->pc[i].y;
    for (int y = 0; y < 8; y++)
      for (int x = 0; x < 8; x++) {
        int sx = x0 + x, sy = y0 + y;
        bool in = sx >= SNES_LEFT && sx < SNES_LEFT + SNES_W && sy >= SNES_TOP && sy < SNES_TOP + SNES_H;
        d[y * 8 + x] = (m[y * 8 + x] && in) ? (uint16_t)(px[sy * stride + sx] | 1) : 0;
      }
  }
  data_cache_hit_writeback(g_lp_pcpix, ((lf->npc * 128) + 15) & ~15);
}
#endif

// Pieces from .. to of the 2D picture, as they are (8 x 8 each), scissored to rows row0 .. row1.
static void LpDrawPieces(const LpFrame *lf, int from, int to, int row0, int row1) {
  if (to <= from || row1 <= row0) return;
  LpPut(LP_SCISSOR(SNES_LEFT, SNES_TOP + row0, SNES_LEFT + SNES_W, SNES_TOP + row1));
  LpPut(LpGrey(1.0f));
#ifndef LP_HOST_TEST
  // RGBA16 copies of the picture (LpPiecesFromPicture): no TLUT, alpha compare
  LpPut(LP_SYNC_PIPE);
  LpPut(LP_OTHER_MODES(LP_SOM_FLAT | 1ull));
  for (int b = from; b < to; b += LP_TEXELS / 64) {
    int n = to - b < LP_TEXELS / 64 ? to - b : LP_TEXELS / 64;
    LpPut(LP_SYNC_PIPE);
    LpPut(LP_TEX_IMAGE(8, PhysicalAddr(g_lp_pcpix + b * 64)));
    LpPut(LP_SET_TILE(7, 2, 0));
    LpPut(LP_SYNC_LOAD);
    LpPut(LP_LOAD_TILE(7, 8, n * 8));
    LpPut(LP_SYNC_TILE);
    LpPut(LP_SET_TILE(0, 2, 0));
    LpPut(LP_TILE_SIZE(0, 8, n * 8));
    for (int j = 0; j < n; j++) {
      int x = SNES_LEFT + lf->pc[b + j].x, y = SNES_TOP + lf->pc[b + j].y;
      LpTexRect(x * 4, y * 4, (x + 8) * 4, (y + 8) * 4, 0, j * 8 * 32);
    }
  }
  LpPut(LP_SYNC_PIPE);                  // back to the layers' mode
  LpPut(LP_OTHER_MODES(LP_SOM_LAYER));
#else
  // PC test: the pieces' own colours through the TLUT (it has no picture to copy from)
  LpTlut(0, lf->fullpal, 256);
  for (int b = from; b < to; b += LP_TEXELS / 64) {
    int n = to - b < LP_TEXELS / 64 ? to - b : LP_TEXELS / 64;
    LpLoadTex(lf->tex + lf->pctex + b * 64, 8, n * 8);
    for (int j = 0; j < n; j++) {
      int x = SNES_LEFT + lf->pc[b + j].x, y = SNES_TOP + lf->pc[b + j].y;
      LpTexRect(x * 4, y * 4, (x + 8) * 4, (y + 8) * 4, 0, j * 8 * 32);
    }
  }
#endif
}

// The BG2 boss (its 2D picture is under it, exactly covered by its outline's
// front), then what is in front of it in the 2D picture, again.
static void LpDrawBoss(const LpFrame *lf) {
  if (!lf->boss.on || !g_lp_boss.valid || lf->boss.stamp != g_lp_boss.stamp) return;
  int gw = g_lp_boss.bw * 8, gh = g_lp_boss.bh * 8, bh = LP_TEXELS / g_lp_boss.tw;
  LpDrawLayers(g_lp_boss.mem, g_lp_boss.tw, bh, gw, gh, lf->boss.x, lf->boss.y, lf->bossct,
               LpYawAt(lf->boss.x + gw * 0.5f), kLpLayers[2].z, lf->boss.row0, lf->boss.row1);
  LpDrawPieces(lf, 0, lf->nbpc, lf->boss.row0, lf->boss.row1);
}

// The player: turned a little, showing the side it faces.
static float g_lp_syaw;
static void LpDrawHeroSprite(const LpFrame *lf) {
  float target = lf->h.valid && lf->h.left ? 0.32f : -0.32f;
  float dyw = target - g_lp_syaw;
  g_lp_syaw = fabsf(dyw) < 1e-3f ? target : g_lp_syaw + dyw * 0.3f;
  if (!lf->hero) return;
  LpDrawSprite(lf, 0, g_lp_syaw);
  LpDrawPieces(lf, lf->nbpc, lf->npc, lf->obj[0].row0, lf->obj[0].row1);   // what is in front of her
}

// The opponents, the front-most one (lowest OAM entries) last.
static void LpDrawEnemySprites(const LpFrame *lf) {
  for (int i = lf->nobj - 1; i >= (lf->hero ? 1 : 0); i--) {
    const LpObj *o = &lf->obj[i];
    LpDrawSprite(lf, i, LpYawAt(o->gx + o->gw * 0.5f));
  }
}

// After the last logic step: the boss, the player's and the opponents' sprites.
static void LpBuildSprites(LpFrame *lf, const PpuFrame *f) {
  lf->ntex = lf->ntexb = lf->nobj = 0;
  lf->hero = false;
  g_lp_paldone = 0;
#ifndef LP_HOST_TEST
  while (LP_DP[3] & (0x400 | 0x20 | 0x40 | 0x100)) {}   // the RDP may still read this frame buffer's last textures
#endif
#if !LP_HERO_KESTREL
  lf->hero = LpBuildSprite(lf, g_lp_hs.s, g_lp_hs.n, LP_HERO_GW, LP_HERO_GH, true, false);
#endif
#if !LP_ENEMY_SHAPES
  // Their textures always fit: the game side already counted them (LowPoly_EndEnemy).
  for (int i = 0; i < g_lp_es.ng; i++) {
    const LpGroup *g = &g_lp_es.g[i];
    if (g->big) {
      if (!LpBigPoolOk()) continue;     // (can't happen: EndEnemy checked it)
      g_lp_boss.valid = false;          // the canvas memory holds these now
      LpBuildSprite(lf, &g_lp_es.s[g->first], g->n, LP_BIG_GW, LP_BIG_GH, false, true);
    } else {
      LpBuildSprite(lf, &g_lp_es.s[g->first], g->n, LP_GW, LP_GH, false, false);
    }
  }
#endif
  // the pieces of the 2D picture in front of the boss and the player (the
  // texture pool's rest, after the sprites)
  lf->npc = lf->nbpc = 0;
  lf->pctex = (uint16_t)lf->ntex;
  LpBossCapture(lf, f);
  lf->nbpc = lf->npc;
  if (lf->hero && f) LpHeroPieces(lf, f, Snes_N64_Vram());
  if (lf->npc) {
    memset(lf->fullpal, 0, sizeof(lf->fullpal));
    for (int p = 0; p < 16; p++)
      if (g_lp_paldone >> p & 1) memcpy(&lf->fullpal[p * 16], &g_lp_pal[p * 16], 32);
    data_cache_hit_writeback(lf->fullpal, sizeof(lf->fullpal));
    data_cache_hit_writeback(lf->tex + lf->pctex, ((lf->npc * 64) + 15) & ~15);
  }
}

// ---- Per frame --------------------------------------------------------------------
// After a frame's last logic step: what to draw over its picture, and the rows.
static void LowPoly_Capture(int idx, bool gameplay, const PpuFrame *f) {
  LpFrame *lf = &g_lp_frames[idx];
  memset(lf->rows, 0, sizeof(lf->rows));
  lf->on = g_lowpoly_on && !g_lp_nomem && gameplay;
  lf->nobj = 0, lf->hero = false, lf->boss.on = false, lf->npc = lf->nbpc = 0;
  if (!lf->on) { g_lp_lastwx = -1; lf->nen = 0; lf->h.valid = false; return; }
  lf->h = g_lp_step.h;
  lf->nen = g_lp_step.nen;
  memcpy(lf->en, g_lp_step.en, sizeof(LpEnemy) * lf->nen);
#ifdef LP_TEST_FAKE
  // test builds: show both Burrowdrakes and two shapes next to the hero (the
  // autoplay route has no regular enemies)
  if (lf->h.valid && lf->nen + 4 <= LP_MAXEN) {
    int fy = lf->h.feetY, x = lf->h.x;
    LpEnemy t[4] = {
      { (int16_t)(x - 70), (int16_t)(fy - 34), (int16_t)(x - 50), (int16_t)fy, 0xD0FF, 0, 0, 0, 0, 1 },
      { (int16_t)(x + 50), (int16_t)(fy - 34), (int16_t)(x + 70), (int16_t)fy, 0xD0FF, 0, 0, 0, 0, 2 },
      { (int16_t)(x - 30), (int16_t)(fy - 70), (int16_t)(x - 14), (int16_t)(fy - 54), 0x1234, 0xd8, 0x40, 0x3c, 0, 0 },
      { (int16_t)(x + 14), (int16_t)(fy - 70), (int16_t)(x + 30), (int16_t)(fy - 54), 0x9876, 0x2e, 0xc4, 0xb4, 0, 0 } };
    for (int k = 0; k < 4; k++) lf->en[lf->nen++] = t[k];
  }
#endif
  for (int i = 0; i < lf->nen; i++) {
    const LpEnemy *e = &lf->en[i];
    float sx, sy, sz;
    LpEnemySize(e, &sx, &sy, &sz);
    float r = (sx > sy ? sx : sy) * (e->boss ? 1.05f : 1.25f);   // reach of the turning shape
    float cy = (e->y0 + e->y1) * 0.5f, cx = (e->x0 + e->x1) * 0.5f;
    lf->er0[i] = lf->er1[i] = 0;
    if (e->x1 - e->x0 < 4 || e->y1 - e->y0 < 4 || cx + r < 0 || cx - r >= SNES_W) continue;
    if (e->model & 3) {   // Burrowdrake: up to 38 units tall (horns), feet on the box bottom
      float sc = LpDrakeScale(e);
      if (cx + 30 * sc < 0 || cx - 30 * sc >= SNES_W) continue;
      LpRows(lf, e->y1 - LpFloorI(40 * sc) - 2, e->y1 + 3, &lf->er0[i], &lf->er1[i]);
      continue;
    }
    LpRows(lf, LpFloorI(cy - r) - 1, LpFloorI(cy + r) + 2, &lf->er0[i], &lf->er1[i]);
  }
  lf->hr0 = lf->hr1 = 0;
#if LP_HERO_KESTREL
  if (lf->h.valid && lf->h.shown && lf->h.x > -40 && lf->h.x < SNES_W + 40) {
    int y0, y1;
    LpHeroRows(&lf->h, &y0, &y1);
    LpRows(lf, y0, y1, &lf->hr0, &lf->hr1);
  }
#endif
  LpBuildSprites(lf, f);                // the player's (unless Kestrel), the opponents' and the bosses' own pictures in 3D
}

// Lines drawn over in the previous frame's picture: the RSP must not copy them.
static int LowPoly_NoCopy(PpuFrame *f, int prevIdx) {
  const LpFrame *lf = &g_lp_frames[prevIdx];
  int n = f->numLines < SNES_H ? f->numLines : SNES_H, un = 0;
  for (int l = 0; l < n; l++)
    if ((lf->rows[l >> 5] >> (l & 31) & 1) && (f->lines[l].flags & kLR_Copy)) { f->lines[l].flags &= ~kLR_Copy; un++; }
  return un;
}

// The picture the RDP is drawing models into: shown by the DP interrupt
// (SYNC_FULL done), so the CPU goes on with the next frame meanwhile.
static surface_t *volatile g_lp_show;
static void LpDpInterrupt(void) {
  surface_t *fb = g_lp_show;
  g_lp_show = NULL;
  if (fb) display_show(fb);
}
static void LpFlushShow(void) {         // the RDP is idle: make sure its picture went out
  disable_interrupts();
  surface_t *fb = g_lp_show;
  g_lp_show = NULL;
  enable_interrupts();
  if (fb) display_show(fb);
}

// Waits until the RDP has finished (and shown) the last picture LowPoly_Draw had.
static void LowPoly_Sync(void) {
  while (LP_DP[3] & (0x400 | 0x20 | 0x40 | 0x100)) {}
  LpFlushShow();
}

// Draws frame idx's models into its finished picture fb and shows it (now, or
// from the DP interrupt once the RDP is done).
static void LowPoly_Draw(int idx, surface_t *fb) {
  LpFrame *lf = &g_lp_frames[idx];
  static bool dp_irq;
  if (!dp_irq) { register_DP_handler(LpDpInterrupt); set_DP_interrupt(1); dp_irq = true; }
  uint64_t t0 = get_ticks_us();
  while (LP_DP[3] & (0x400 | 0x20 | 0x40 | 0x100)) {}   // idle (nothing else uses the RDP)
  LpFlushShow();
  if (!lf->on) { display_show(fb); return; }
#ifndef LP_HOST_TEST
  if (lf->npc) LpPiecesFromPicture(lf, fb);   // (before the RDP draws anything on it)
#endif
  LP_DP[3] = 0x1 | 0x4 | 0x10;          // commands from RDRAM, unfreeze, unflush
  g_lp_w = g_lp_wstart = g_lp_cmds;
  g_lp_wend = g_lp_cmds + LP_CHUNK;
  LpPut(LP_SYNC_PIPE);
  LpPut(LP_COLOR_IMAGE(SCREEN_W, PhysicalAddr(fb->buffer)));
  LpPut(LP_OTHER_MODES(LP_SOM_FLAT));
  LpPut(LP_CC_PRIM);
  LpDrawEnemies(lf);                    // opponents first (shapes, or their sprites in 3D), the hero on top
  if (lf->nobj || lf->boss.on) {        // sprites in 3D: CI8 layers through a TLUT, transparent texels not drawn
    LpPut(LP_SYNC_PIPE);
    LpPut(LP_OTHER_MODES(LP_SOM_LAYER));
    LpPut(LP_CC_TEXPRIM_TA);
    LpPut(LP_BLEND_COLOR(0x00000080));  // alpha compare threshold
    LpDrawBoss(lf);                     // the BG2 boss behind everything
    LpDrawEnemySprites(lf);
#if !LP_HERO_KESTREL
    LpDrawHeroSprite(lf);
#endif
    LpPut(LP_SYNC_PIPE);                // back to flat colours
    LpPut(LP_OTHER_MODES(LP_SOM_FLAT));
    LpPut(LP_CC_PRIM);
  }
#if LP_HERO_KESTREL
  if (lf->h.valid) LpDrawHero(lf);
  else g_lp_lastwx = -1;
#endif
  LpPut(LP_SYNC_FULL);
  g_lp_show = fb;                       // (before the RDP can finish)
  LpSubmit(false);
  g_lp_us += get_ticks_us() - t0;
}
