// Checks rsp_fx.S against a C reference on random masks / pixels (cxd4).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsph.h"
enum { N = 224, FB = 0x100000, STRIDE = 640, MASK = 0x200000 };
static uint16_t z[N][256], pix[N][256], ref[N][256];
static uint32_t fxb[8], vb[8];
static uint16_t cls(uint16_t v) { int t = (v >> 8) & 7; return t == 0 ? 0x3802 : (t & 5) == 4 ? 0x3800 : 1; }
static uint16_t W(int r, int x) {
  if (r < 0 || r >= N || !(vb[r >> 5] >> (r & 31) & 1) || x < 0 || x >= 256) return 2;
  return cls(z[r][x]);
}
static uint16_t Fx(int y, int x, uint16_t p) {
  uint16_t A = W(y-2,x-2), B = W(y-1,x-1), X = W(y-3,x-3), U = W(y-1,x), L = W(y,x-1), C = W(y,x), R = W(y,x+1), D = W(y+1,x);
  uint16_t UL = U & L, DR = D & R, C2 = C & 2;
  int edge = (C2 & ~(UL & DR)) != 0, light = (C2 & DR & ~UL) != 0;
  uint32_t taps = (A & 0x3800) + (B & 0x3800) + (X & 0x3800);
  uint32_t d = ((C & 1) ? taps : 0) | (edge ? 0x8000 : 0);
  uint16_t sub = (((p & 0xf800) * d >> 16) & 0xf800) | (((p & 0x07c0) * d >> 16) & 0x07c0) | (((p & 0x3e) * d >> 16) & 0x3e);
  return (uint16_t)(p - sub + (light ? 0x6318 : 0));
}
static uint16_t rz(int c) {   // a z value of class c (0 bg, 1 obj, 5 bg1)
  static const int t0[] = {1, 2, 5, 3, 7}, t1[] = {4, 6};
  int t = c == 5 ? 0 : c == 1 ? t1[rand() & 1] : t0[rand() % 5];
  return (uint16_t)(((rand() & 0xf8) | t) << 8 | (rand() & 0xff));
}
int main(int argc, char **argv) {
  int seeds = argc > 1 ? atoi(argv[1]) : 20, bad = 0;
  for (int seed = 0; seed < seeds; seed++) {
    srand(seed);
    int cell = 4 + rand() % 12;
    for (int y = 0; y < N; y++) for (int x = 0; x < 256; x++) {
      unsigned h = (x / cell) * 7919u + (y / cell) * 104729u + seed * 31u; h = (h ^ (h >> 7)) * 2654435761u >> 28;
      int c = h < 6 ? 5 : h < 8 ? 1 : 0;
      if (rand() % 20 == 0) c = (int[]){0, 1, 5}[rand() % 3];
      if (seed % 5 == 4) z[y][x] = rand(); else z[y][x] = rz(c);
      pix[y][x] = rand() | 1;
    }
    for (int i = 0; i < 8; i++) { vb[i] = rand() ^ rand() << 16; fxb[i] = vb[i] & (rand() ^ rand() << 16); }
    if (seed & 1) for (int i = 0; i < 8; i++) vb[i] = fxb[i] = ~0u;
    rsph_init(); rsph_load_file("rsp_fx.text.bin", "rsp_fx.data.bin");
    for (int y = 0; y < N; y++) for (int x = 0; x < 256; x++) {
      uint8_t b[2] = {pix[y][x] >> 8, pix[y][x]}; rsph_write(rsph_rdram, FB + y * STRIDE + 64 + 2 * x, b, 2);
      uint8_t c[2] = {z[y][x] >> 8, z[y][x]}; rsph_write(rsph_rdram, MASK + y * 512 + 2 * x, c, 2);
      ref[y][x] = (fxb[y >> 5] >> (y & 31) & 1) ? Fx(y, x, pix[y][x]) : pix[y][x];
    }
    rsph_w32(rsph_dmem, 0, FB + 64); rsph_w32(rsph_dmem, 4, STRIDE); rsph_w32(rsph_dmem, 8, MASK); rsph_w32(rsph_dmem, 12, N << 16 | 9 << 8 | 7);
    for (int i = 0; i < 8; i++) { rsph_w32(rsph_dmem, 32 + 4 * i, fxb[i]); rsph_w32(rsph_dmem, 64 + 4 * i, vb[i]); }
    if (rsph_run()) { printf("seed %d: no halt\n", seed); return 1; }
    int nb = 0, changed = 0;
    for (int y = 0; y < N; y++) for (int x = 0; x < 256; x++) {
      uint8_t b[2]; rsph_read(rsph_rdram, FB + y * STRIDE + 64 + 2 * x, b, 2);
      uint16_t v = b[0] << 8 | b[1];
      changed += ref[y][x] != pix[y][x];
      if (v != ref[y][x]) { if (nb++ < 5) printf("seed %d y %d x %d rsp %04x ref %04x pix %04x\n", seed, y, x, v, ref[y][x], pix[y][x]); }
    }
    // border pixels untouched?
    uint8_t b[2]; rsph_read(rsph_rdram, FB + 62, b, 2); if (b[0] | b[1]) nb++;
    printf("seed %d: %d bad, %d changed\n", seed, nb, changed);
    bad += nb;
  }
  printf(bad ? "FAIL\n" : "OK\n");
  return bad != 0;
}
