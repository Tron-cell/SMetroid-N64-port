// Runs rsp_ppu microcode (cxd4) on captured frames and compares with the C reference.
// usage: ppu_test <ucode prefix> <frames.bin>... [-dbg] [-max N]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "rsph.h"
#include "ppu_rsp.h"

void PpuRsp_RenderLineDebug(const PpuFrame *f, int idx, uint16_t *dst, void *dbg);
typedef struct { uint16_t obj[256], main[256], sub[256]; bool rendered_sub; } Dbg;
extern unsigned long long g_rsp_instr, g_rsp_dma_count, g_rsp_dma_bytes, g_rsp_pc_hist[1024];
extern void (*g_rsp_trace)(unsigned, unsigned); extern void (*g_rsp_dma_hook)(unsigned, int);
void rspcyc_reset(void); void rspcyc_inst(unsigned, unsigned); void rspcyc_dma(unsigned, int); extern unsigned long long g_rspcyc_cycles;

enum { A_VRAM = 0x100000, A_LINES = 0x110000, A_CGRAM = 0x120000, A_SLIV = 0x130000, A_FB = 0x200000, A_DBG = 0x300000, A_BLOCKS = 0x400000, A_M7 = 0x600000, A_MASK = 0x700000 };

static void w16(uint8_t *mem, uint32_t a, uint16_t v) { uint8_t b[2] = { v >> 8, v }; rsph_write(mem, a, b, 2); }
static uint16_t r16(uint8_t *mem, uint32_t a) { uint8_t b[2]; rsph_read(mem, a, b, 2); return b[0] << 8 | b[1]; }

static void upload(const PpuFrame *f) {
  for (int i = 0; i < 0x8000; i++) w16(rsph_rdram, A_VRAM + i * 2, f->vram[i]);
  for (int c = 0; c < f->numCgrams; c++) for (int i = 0; i < 256; i++) w16(rsph_rdram, A_CGRAM + c * 512 + i * 2, PpuRsp_Cg5551(f->cgram[c][i]));   // (as PpuRsp_ConvertCgrams)
  for (int l = 0; l < f->numLines; l++) {
    const PpuLineRec *r = &f->lines[l];
    uint32_t a = A_LINES + l * 64;
    rsph_write(rsph_rdram, a, r, 16);   // byte fields 0..15
    w16(rsph_rdram, a + 16, r->fixedColor);
    w16(rsph_rdram, a + 18, r->sliverStart);
    rsph_w32(rsph_rdram, a + 20, r->windowsel);
    for (int k = 0; k < 3; k++) {
      const PpuLayerRec *L = &r->bg[k];
      uint32_t b = a + 24 + k * 10;
      w16(rsph_rdram, b, L->tm0); w16(rsph_rdram, b + 2, L->tm1); w16(rsph_rdram, b + 4, L->ta0);
      w16(rsph_rdram, b + 6, L->ta1); w16(rsph_rdram, b + 8, L->hscroll);
    }
    { static int nb = -1; if (nb < 0) nb = getenv("NOBLOCK") ? atoi(getenv("NOBLOCK")) : 0;
      for (int k = 0; k < 3; k++) w16(rsph_rdram, a + 54 + k * 2, (nb >> k & 1) ? 0xffff : r->block[k]); }
    w16(rsph_rdram, a + 60, r->drawMask << 8 | r->composeBits);
  }
  for (int i = 0; i < f->numBlocks; i++) {
    const PpuTileBlock *b = &f->blocks[i];
    uint32_t a = A_BLOCKS + i * sizeof(PpuTileBlock);
    for (int t = 0; t < 100; t++) w16(rsph_rdram, a + t * 2, b->z[t]);
    rsph_write(rsph_rdram, a + 200, b->tiles, sizeof(b->tiles));   // already big-endian bytes
  }
  rsph_write(rsph_rdram, A_M7, f->m7rows, f->numM7 * 256);
  for (int i = 0; i < f->numSlivers; i++) {
    const PpuSliver *s = &f->slivers[i];
    w16(rsph_rdram, A_SLIV + i * 8, s->x); w16(rsph_rdram, A_SLIV + i * 8 + 2, s->z);
    rsph_w32(rsph_rdram, A_SLIV + i * 8 + 4, s->planes);
  }
}


static void save_ppm(const char *fn) {
  FILE *o = fopen(fn, "wb"); fprintf(o, "P6\n256 224\n255\n");
  for (int y = 0; y < 224; y++) for (int x = 0; x < 256; x++) {
    uint16_t v = r16(rsph_rdram, A_FB + y * 512 + x * 2);
    uint8_t c[3] = { (v >> 11 & 31) * 255 / 31, (v >> 6 & 31) * 255 / 31, (v >> 1 & 31) * 255 / 31 };
    fwrite(c, 3, 1, o);
  }
  fclose(o);
}
// Runs rsp_fx on the frame just drawn (all valid lines >= 32, as with no copies). Returns cycles.
static unsigned long long run_fx(const PpuFrame *f, const char *fxp, const char *ppup, int fno) {
  char tb[256], db[256];
  uint32_t fxb[8] = {0};
  int n = f->numLines < 224 ? f->numLines : 224, any = 0;
  for (int l = 32; l < n; l++) {
    uint8_t fl = f->lines[l].flags;
    if ((fl & kLR_Valid) && !(fl & (kLR_CpuDrawn | kLR_ForcedBlank | kLR_Mode7))) { fxb[l >> 5] |= 1u << (l & 31); any = 1; }
  }
  if (!any) return 0;
  const char *imgdir = getenv("FXIMG");
  if (imgdir) { snprintf(tb, sizeof tb, "%s/pre_%05d.ppm", imgdir, fno); save_ppm(tb); }
  if (imgdir) { snprintf(tb, sizeof tb, "%s/mask_%05d.bin", imgdir, fno); FILE *o = fopen(tb, "wb"); for (int i = 0; i < 224 * 256; i++) { uint16_t v = r16(rsph_rdram, A_MASK + i * 2); fputc(v >> 8, o); fputc(v, o); } fclose(o); }
  snprintf(tb, sizeof tb, "%s.text.bin", fxp); snprintf(db, sizeof db, "%s.data.bin", fxp);
  rsph_load_file(tb, db);
  rsph_w32(rsph_dmem, 0, A_FB); rsph_w32(rsph_dmem, 4, 512); rsph_w32(rsph_dmem, 8, A_MASK); rsph_w32(rsph_dmem, 12, n << 16 | 8 << 8 | 8);
  for (int i = 0; i < 8; i++) { rsph_w32(rsph_dmem, 32 + 4 * i, fxb[i]); rsph_w32(rsph_dmem, 64 + 4 * i, fxb[i]); }
  extern unsigned long long g_rspcyc_pc[1024];
  static unsigned long long save[1024]; memcpy(save, g_rspcyc_pc, sizeof save); memset(g_rspcyc_pc, 0, sizeof save);
  rspcyc_reset();
  if (rsph_run()) { printf("fx hang\n"); exit(1); }
  { static int dumped; if (!dumped++ && getenv("FXHIST")) { FILE *h = fopen(getenv("FXHIST"), "w"); for (int i = 0; i < 1024; i++) if (g_rspcyc_pc[i]) fprintf(h, "%x %llu\n", i * 4, g_rspcyc_pc[i]); fclose(h); } }
  memcpy(g_rspcyc_pc, save, sizeof save);
  unsigned long long c = g_rspcyc_cycles;
  if (imgdir) { snprintf(tb, sizeof tb, "%s/post_%05d.ppm", imgdir, fno); save_ppm(tb); }
  snprintf(tb, sizeof tb, "%s.text.bin", ppup); snprintf(db, sizeof db, "%s.data.bin", ppup);
  rsph_load_file(tb, db);
  return c;
}
uint32_t g_ppu_regGen;   // (ppu.c, not linked here)
static long g_maskbad;
typedef struct { uint16_t obj[256], main[256], sub[256]; bool rendered_sub; } DbgT;
static uint16_t eff(const void *dv, int x) { const DbgT *d = (const DbgT *)dv; uint16_t m = d->main[x];
  if (d->rendered_sub && (((m >> 8) & 7) == 2 || m == 0x0500)) { m = d->sub[x]; if (((m >> 8) & 7) == 2) ; }
  return m; }
static unsigned long long g_fxtot, g_fxmax, g_fxn;
static int cls(uint16_t v) { int t = (v >> 8) & 7; return t == 0 ? 5 : (t & 5) == 4 ? 1 : 0; }
int main(int argc, char **argv) {
  const char *prefix = argv[1];
  bool dbg = false; int maxf = 1 << 30, onlyf = -1;
  char tb[256], db[256]; snprintf(tb, sizeof tb, "%s.text.bin", prefix); snprintf(db, sizeof db, "%s.data.bin", prefix);
  rsph_init();
  g_rsp_trace = rspcyc_inst; g_rsp_dma_hook = rspcyc_dma;
  unsigned long long tot_cyc = 0, max_cyc = 0;
  rsph_load_file(tb, db);   // once, like the N64: DMEM state must not leak between frames
  static PpuFrame f;
  static uint16_t ref[256];
  long frames = 0, lines = 0, badlines = 0, badfirst = 0;
  unsigned long long tot_instr = 0, tot_dma = 0, tot_dmab = 0, max_instr = 0;
  for (int ai = 2; ai < argc; ai++) {
    if (!strcmp(argv[ai], "-dbg")) { dbg = true; continue; }
    if (!strcmp(argv[ai], "-max")) { maxf = atoi(argv[++ai]); continue; }
    if (!strcmp(argv[ai], "-only")) { onlyf = atoi(argv[++ai]); continue; }
    FILE *fp = fopen(argv[ai], "rb"); if (!fp) { perror(argv[ai]); return 1; }
    int32_t hdr[2];
    while (frames < maxf && fread(hdr, 4, 2, fp) == 2) {
      // dumps made before PpuFrame got 16-byte alignment are a prefix of the current layout
      if (hdr[1] > (int)sizeof(PpuFrame) || hdr[1] < (int)sizeof(PpuFrame) - 15) { fprintf(stderr, "frame size mismatch %d vs %zu\n", hdr[1], sizeof(PpuFrame)); return 1; }
      memset(&f, 0, sizeof f);
      if (fread(&f, hdr[1], 1, fp) != 1) break;
      if (onlyf >= 0 && hdr[0] != onlyf) continue;
      upload(&f);
            rsph_w32(rsph_dmem, 0, A_VRAM); rsph_w32(rsph_dmem, 4, A_LINES); rsph_w32(rsph_dmem, 8, A_CGRAM);
      rsph_w32(rsph_dmem, 12, A_SLIV); rsph_w32(rsph_dmem, 16, A_FB); rsph_w32(rsph_dmem, 20, 512);
      w16(rsph_dmem, 24, f.numLines); w16(rsph_dmem, 26, dbg ? 1 : 0); rsph_w32(rsph_dmem, 28, A_DBG); rsph_w32(rsph_dmem, 32, A_BLOCKS); rsph_w32(rsph_dmem, 36, A_M7); rsph_w32(rsph_dmem, 44, getenv("MASKCHK") ? A_MASK : 0);
      if (getenv("MASKCHK")) for (int i = 0; i < 224 * 256; i++) w16(rsph_rdram, A_MASK + i * 2, 0xbeef);
      // poison the framebuffer so untouched lines show up
      for (int i = 0; i < 240 * 256; i++) w16(rsph_rdram, A_FB + i * 2, 0xdead);
      unsigned long long i0 = g_rsp_instr, d0 = g_rsp_dma_count, b0 = g_rsp_dma_bytes;
      rspcyc_reset();
      if (rsph_run()) { printf("frame %d: RSP hang\n", hdr[0]); return 1; }
      unsigned long long di = g_rsp_instr - i0;
      tot_instr += di; if (di > max_instr) max_instr = di;
      tot_cyc += g_rspcyc_cycles; if (g_rspcyc_cycles > max_cyc) max_cyc = g_rspcyc_cycles;
      if (getenv("CYCLIST")) printf("CYC frame %d %llu\n", hdr[0], g_rspcyc_cycles);
      tot_dma += g_rsp_dma_count - d0; tot_dmab += g_rsp_dma_bytes - b0;
      frames++;
      for (int l = 0; l < f.numLines; l++) {
        const PpuLineRec *r = &f.lines[l];
        if (!(r->flags & kLR_Valid) || (r->flags & kLR_CpuDrawn)) continue;
        lines++;
        Dbg d; memset(&d, 0, sizeof d);
        PpuRsp_RenderLineDebug(&f, l, ref, &d);
        int bad = -1;
        if (getenv("SUBDUMP") && hdr[0] == atoi(getenv("SUBDUMP")) && l % 16 == 8) {
          char mc[257], sc[257]; const char *cn = "BbFObOxO";
          for (int x = 0; x < 256; x++) { mc[x] = "1235o5?o"[(d.main[x] >> 8) & 7]; sc[x] = d.rendered_sub ? "1235o5?o"[(d.sub[x] >> 8) & 7] : '-'; }
          mc[256] = sc[256] = 0; (void)cn;
          printf("L%3d main %.128s\n     sub  %.128s\n", l, mc, sc);
        }
        if (getenv("MASKCHK") && !(r->flags & (kLR_ForcedBlank | kLR_Copy)))
          for (int x = 0; x < 256; x++) if (cls(r16(rsph_rdram, A_MASK + l * 512 + x * 2)) != cls(eff(&d, x) ^ 0x8000)) { static int nm; if (nm++ < 5) printf("mask frame %d line %d x %d: %04x vs %04x\n", hdr[0], l, x, r16(rsph_rdram, A_MASK + l * 512 + x * 2) ^ 0x8000, d.main[x]); g_maskbad++; break; }
        for (int x = 0; x < 256; x++) if (r16(rsph_rdram, A_FB + l * 512 + x * 2) != ref[x]) { bad = x; break; }
        if (bad >= 0) {
          badlines++;
          if (badfirst++ < 6) {
            printf("frame %d line %d: first bad x=%d rsp=%04x ref=%04x  (flags %02x math %02x mf %02x pm %d clip %d sprites %d)\n", hdr[0], l, bad,
                   r16(rsph_rdram, A_FB + l * 512 + bad * 2), ref[bad], r->flags, r->mathEnabled, r->mathFlags, r->preventMathMode, r->clipMode, r->sliverCount);
            if (dbg && !(r->flags & kLR_ForcedBlank)) {
              const char *nm[3] = { "obj", "main", "sub" };
              uint16_t *rb[3] = { d.obj, d.main, d.sub };
              for (int k = 0; k < 3; k++) {
                if (k == 2 && !d.rendered_sub) continue;
                for (int x = 0; x < 256; x++) {
                  uint16_t rv = r16(rsph_rdram, A_DBG + l * 0x700 + k * 0x220 + (x + 8) * 2) ^ 0x8000;
                  if (rv != rb[k][x]) { printf("   %s differs first at x=%d: rsp=%04x ref=%04x\n", nm[k], x, rv, rb[k][x]); break; }
                }
              }
              printf("   rendered_sub rsp=%d ref=%d\n", rsph_r32(rsph_rdram, A_DBG + l * 0x700 + 0x660), d.rendered_sub);
              printf("   at x=%d: main rsp=%04x ref=%04x sub rsp=%04x ref=%04x fixed=%04x bright=%d cg[main]=%04x cg[sub]=%04x\n", bad,
                     r16(rsph_rdram, A_DBG + l * 0x700 + 0x220 + (bad + 8) * 2) ^ 0x8000, d.main[bad], r16(rsph_rdram, A_DBG + l * 0x700 + 0x440 + (bad + 8) * 2) ^ 0x8000, d.sub[bad],
                     r->fixedColor, r->brightness, f.cgram[r->cgramIdx][d.main[bad] & 0xff], f.cgram[r->cgramIdx][d.sub[bad] & 0xff]);
            }
          }
        }
      }
      if (getenv("FXUCODE")) {
        static unsigned long long fxtot, fxmax, fxn;
        unsigned long long c = run_fx(&f, getenv("FXUCODE"), prefix, hdr[0]);
        if (c) { fxtot += c; fxn++; if (c > fxmax) fxmax = c; }
        if (getenv("CYCLIST") && c) printf("FXCYC frame %d %llu\n", hdr[0], c);
        g_fxtot = fxtot; g_fxmax = fxmax; g_fxn = fxn;
      }
    }
    fclose(fp);
  }
  printf("model: RSP cycles/frame avg %.0fK max %.0fK (= %.1f ms avg, %.1f ms max at 62.5 MHz)\n", frames ? tot_cyc / 1e3 / frames : 0, max_cyc / 1e3,
         frames ? tot_cyc / 62.5e3 / frames : 0, max_cyc / 62.5e3);
  printf("frames=%ld lines=%ld BAD_LINES=%ld | RSP instr/frame avg %.0fK max %.0fK | DMAs/frame %.0f (%.0f KB)\n",
         frames, lines, badlines, frames ? tot_instr / 1e3 / frames : 0, max_instr / 1e3, frames ? (double)tot_dma / frames : 0,
         frames ? tot_dmab / 1024.0 / frames : 0);
  { extern unsigned long long g_rspcyc_pc[1024]; FILE *cf = fopen("cyc_hist.txt", "w"); for (int i = 0; i < 1024; i++) if (g_rspcyc_pc[i]) fprintf(cf, "%x %llu\n", 0x1000 + i * 4, g_rspcyc_pc[i]); fclose(cf); }
  { FILE *hf = fopen("pc_hist.txt", "w"); for (int i = 0; i < 1024; i++) if (g_rsp_pc_hist[i]) fprintf(hf, "%x %llu\n", 0x1000 + i * 4, g_rsp_pc_hist[i]); fclose(hf); }
  if (g_fxn) printf("FX: %llu frames, avg %.2f ms max %.2f ms\n", g_fxn, g_fxtot / 62.5e3 / g_fxn, g_fxmax / 62.5e3);
  if (getenv("MASKCHK")) printf("MASK_BAD_LINES=%ld\n", g_maskbad);
  return badlines != 0 || g_maskbad;
}
