// Checks rsp_m7.S against the C model of Mode7Row's scale-1 path (cxd4).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsph.h"
extern void (*g_rsp_trace)(unsigned, unsigned); extern void (*g_rsp_dma_hook)(unsigned, int);
void rspcyc_reset(void); void rspcyc_inst(unsigned, unsigned); void rspcyc_dma(unsigned, int); extern unsigned long long g_rspcyc_cycles;
enum { MAP = 0x100000, PIX = 0x110000, OUT = 0x200000, LIST = 0x300000 };
static uint8_t map[0x4000], pix[0x4000], ref[240][256];
int main(int argc, char **argv) {
  int seeds = argc > 1 ? atoi(argv[1]) : 10, bad = 0;
  for (int seed = 0; seed < seeds; seed++) {
    for (int i = 0; i < 0x4000; i++) { map[i] = rand(); pix[i] = rand(); }
    rsph_init(); rsph_load_file("rsp_m7.text.bin", "rsp_m7.data.bin");
    srand(seed * 7919 + 1);
    rsph_write(rsph_rdram, MAP, map, sizeof map); rsph_write(rsph_rdram, PIX, pix, sizeof pix);
    int n = 1 + rand() % 224, Y = rand() & 1023, X0 = rand() & 1023;
    memset(ref, 0, sizeof ref);
    int idxs[240]; int used[240] = {0};
    for (int e = 0; e < n; e++) {
      // mostly consecutive lines with a fixed X0 (like scrolling), sometimes jumps
      if (rand() % 16 == 0) Y = rand() & 1023; else Y = (Y + 1) & 1023;
      if (rand() % 32 == 0) X0 = rand() & 1023;
      int idx; do idx = rand() % 239; while (used[idx]); used[idx] = 1; idxs[e] = idx;
      uint8_t ent[8] = { idx >> 8, idx, Y >> 8, Y, X0 >> 8, X0, 0, 0 };
      rsph_write(rsph_rdram, LIST + e * 8, ent, 8);
      for (int x = 0; x < 256; x++) {
        int px = (X0 + x) & 1023;
        ref[idx][x] = pix[map[(Y >> 3) * 128 + (px >> 3)] * 64 + (Y & 7) * 8 + (px & 7)];
      }
    }
    rsph_w32(rsph_dmem, 0, MAP); rsph_w32(rsph_dmem, 4, PIX); rsph_w32(rsph_dmem, 8, OUT); rsph_w32(rsph_dmem, 12, LIST);
    rsph_w32(rsph_dmem, 16, n << 16);
    g_rsp_trace = rspcyc_inst; g_rsp_dma_hook = rspcyc_dma; rspcyc_reset();
    if (rsph_run()) { printf("seed %d: no halt\n", seed); return 1; }
    int nb = 0;
    for (int e = 0; e < n; e++) {
      uint8_t row[256]; rsph_read(rsph_rdram, OUT + idxs[e] * 256, row, 256);
      if (memcmp(row, ref[idxs[e]], 256)) { if (nb++ < 3) { int x = 0; while (row[x] == ref[idxs[e]][x]) x++; printf("seed %d entry %d idx %d x %d rsp %02x ref %02x\n", seed, e, idxs[e], x, row[x], ref[idxs[e]][x]); } }
    }
    printf("seed %d: %d entries, %d bad, %llu cycles (%.0f per row)\n", seed, n, nb, g_rspcyc_cycles, (double)g_rspcyc_cycles / n);
    bad += nb;
  }
  { extern unsigned long long g_rspcyc_pc[1024]; FILE *cf = fopen("cyc_hist.txt", "w"); for (int i = 0; i < 1024; i++) if (g_rspcyc_pc[i]) fprintf(cf, "%x %llu\n", i * 4, g_rspcyc_pc[i]); fclose(cf); }
  printf(bad ? "FAIL\n" : "OK\n");
  return bad != 0;
}
