// Lockstep test harness: identical source runs on x86 (reference, little-endian)
// and under Unicorn as big-endian MIPS. Prints per-frame fingerprints of all
// game-visible state so the two runs can be diffed frame by frame.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "types.h"
#include "config.h"
#include "snes/snes.h"
#include "snes/ppu.h"
#include "snes/dsp.h"
#ifdef RIG_PPUCHECK
#include "snes/ppu_rsp.h"
extern bool g_ppu_rsp_also_draw;
static PpuFrame g_pframes[2];   // alternate like the N64 (exercises the VRAM snapshot bookkeeping)
#define g_pframe g_pframes[f & 1]
static uint16_t g_fb16[240 * 256];
static long st_frames, st_lines_rsp, st_lines_cpu, st_lines_blank, st_mismatch_lines, st_multi_cgram, st_max_sliv, st_max_cg;
static int st_first_bad = -1;
#endif
#ifdef RIG_DSPCHECK   // RSP sound microcode (cxd4) vs dsp.c, every step
#include "rsph.h"
#include "spc_player.h"
extern SpcPlayer *g_spc_player;
extern unsigned long long g_rsp_instr, g_rsp_dma_count;
extern void (*g_rsp_trace)(unsigned, unsigned); extern void (*g_rsp_dma_hook)(unsigned, int);
void rspcyc_reset(void); void rspcyc_inst(unsigned, unsigned); void rspcyc_dma(unsigned, int); extern unsigned long long g_rspcyc_cycles;
static unsigned long long d_cyc_tot, d_cyc_max;
static DspLog g_dlog; static uint8_t g_dlog_ents[4096 * 4];
enum { D_ARAM = 0x100000, D_STATE = 0x120000, D_LOG = 0x130000, D_OUT = 0x140000 };
static long d_nonzero, d_steps, d_bad_steps, d_bad_samples, d_bad_echo, d_logmax; static unsigned long long d_instr_max, d_instr_tot;
static const char *d_ucode = "rsp_dsp";
static void DspCheckStep(int f, const int16_t *ref) {
  char tb[256], db[256]; snprintf(tb, sizeof tb, "%s.text.bin", d_ucode); snprintf(db, sizeof db, "%s.data.bin", d_ucode);
  rsph_load_file(tb, db);
  rsph_write(rsph_rdram, D_LOG, g_dlog.ents, g_dlog.count * 4);
  rsph_w32(rsph_dmem, 0, D_ARAM); rsph_w32(rsph_dmem, 4, D_STATE); rsph_w32(rsph_dmem, 8, D_LOG);
  uint8_t hb[4] = { g_dlog.count >> 8, g_dlog.count, 534 >> 8, 534 & 0xff }; rsph_write(rsph_dmem, 12, hb, 4);
  rsph_w32(rsph_dmem, 16, D_OUT);
  if (g_dlog.count > d_logmax) d_logmax = g_dlog.count;
  unsigned long long i0 = g_rsp_instr;
  rspcyc_reset();
  if (rsph_run()) { printf("DSPCHECK frame %d: RSP hang\n", f); exit(1); }
  d_cyc_tot += g_rspcyc_cycles; if (g_rspcyc_cycles > d_cyc_max) d_cyc_max = g_rspcyc_cycles;
  unsigned long long di = g_rsp_instr - i0; d_instr_tot += di; if (di > d_instr_max) d_instr_max = di;
  d_steps++;
  static uint8_t out[534 * 4];
  rsph_read(rsph_rdram, D_OUT, out, sizeof out);
  int bad = 0;
  for (int i = 0; i < 534 * 2; i++) {
    int16_t v = (int16_t)(out[i * 2] << 8 | out[i * 2 + 1]);
    if (ref[i]) d_nonzero++;
    if (v != ref[i]) { if (bad++ < 1 && d_bad_steps < 8) printf("DSPBAD frame %d sample %d %c rsp %d ref %d (log %d)\n", f, i / 2, i & 1 ? 'R' : 'L', v, ref[i], g_dlog.count); }
  }
  if (bad) { d_bad_steps++; d_bad_samples += bad; }
  // echo region written by the RSP must match what dsp.c wrote
  Dsp *dsp = g_spc_player->dsp;
  if (dsp->echoWrites) {
    static uint8_t aram[65536 + 16];
    rsph_read(rsph_rdram, D_ARAM, aram, 65536);
    int n = dsp->echoDelay * 4, a = dsp->echoBufferAdr, ebad = 0;
    for (int i = 0; i < n; i++) if (aram[(a + i) & 0xffff] != g_spc_player->ram[(a + i) & 0xffff]) ebad++;
    if (ebad) { if (d_bad_echo++ < 4) printf("DSPECHO frame %d: %d echo bytes differ\n", f, ebad); }
  }
  g_dlog.count = 0;
}
#endif
#ifdef RIG_CAPTURE_ONLY   // N64 configuration: the CPU only captures, the RSP draws
#include "snes/ppu_rsp.h"
static PpuFrame g_cframe;
#endif

Snes *SnesInit(const char *filename); bool RtlRunFrame(int inputs); void RtlReadSram(void);
void RtlRenderAudio(int16_t *b, int s, int c);
struct SpcPlayer *SpcPlayer_Create(void); void SpcPlayer_Initialize(struct SpcPlayer *p);
extern Snes *g_snes; extern uint8 g_ram[0x20000]; extern uint8 *g_sram;
bool g_debug_flag, g_is_turbo, g_want_dump_memmap_flags, g_new_ppu = true, g_other_image;
struct SpcPlayer *g_spc_player; int g_got_mismatch_count; Config g_config;
void Die(const char *e) { printf("DIE %s\n", e); exit(1); }
void Warning(const char *e) { printf("WARN %s\n", e); }
void RtlApuLock(void) {} void RtlApuUnlock(void) {}
void N64_ReadSram(uint8_t *s, size_t n) { const char *p = getenv("RIG_SRAM"); FILE *f = p ? fopen(p, "rb") : NULL; if (f) { if (fread(s, 1, n, f) != n) puts("short SRAM file"); fclose(f); } } void N64_WriteSram(const uint8_t *s, size_t n) {}

// Logical (SNES-order) byte views. The byte-order fix may change how memory is
// stored on big-endian hosts; these hooks let the harness always hash SNES order.
#ifndef RAM_LOGICAL_BYTE
#define RAM_LOGICAL_BYTE(base, i) ((base)[i])
#endif

#ifdef RIG_BE
#ifdef __cplusplus
extern "C"
#endif
long hostcall(long code, long a1, long a2, long a3);
uint8_t *N64_ReadRom(const char *name, size_t *length) {
  uint8_t *b = malloc(0x400000);
  long n = hostcall(1, (long)b, 0x400000, 0);
  if (n <= 0) return NULL;
  *length = n; return b;
}
static void dump_block(const void *p, size_t n) { hostcall(4, (long)p, n, 0); }
#else
static const char *g_rom_path; static FILE *g_dump;
uint8_t *N64_ReadRom(const char *name, size_t *length) {
  FILE *f = fopen(g_rom_path, "rb"); if (!f) return NULL;
  uint8_t *b = malloc(0x400000); size_t n = fread(b, 1, 0x400000, f); fclose(f);
  *length = n; return b;
}
static void dump_block(const void *p, size_t n) { if (g_dump) fwrite(p, 1, n, g_dump); }
#endif

static uint32_t fnv_bytes(uint32_t h, const uint8_t *base, size_t n) {
  for (size_t i = 0; i < n; i++) { h ^= RAM_LOGICAL_BYTE(base, i); h *= 16777619u; }
  return h;
}
static uint32_t fnv_u16(uint32_t h, const uint16_t *p, size_t n) {
  for (size_t i = 0; i < n; i++) { h ^= p[i] & 0xff; h *= 16777619u; h ^= p[i] >> 8; h *= 16777619u; }
  return h;
}
static uint32_t fnv_u32(uint32_t h, const uint32_t *p, size_t n) {
  for (size_t i = 0; i < n; i++) for (int s = 0; s < 32; s += 8) { h ^= (p[i] >> s) & 0xff; h *= 16777619u; }
  return h;
}

// Deterministic controller script (SNES bit layout).
enum { B_ = 1, Y_ = 2, SEL = 4, START = 8, UP = 16, DOWN = 32, LEFT = 64, RIGHT = 128, A_ = 256, X_ = 512, L_ = 1024, R_ = 2048 };
static int g_script;
static int ScriptInput0(int f);
static int ScriptInput1(int f) { return 0; }  // idle: title -> attract-mode gameplay demos
// Script 2: start a new game and play. Built from timed button events.
typedef struct { int frame, len, buttons; } Ev;
static const Ev kPlay[] = {
#include "play_script.h"
};
static int ScriptInput2(int f) {
  int r = 0;
  for (size_t i = 0; i < sizeof(kPlay) / sizeof(kPlay[0]); i++)
    if (f >= kPlay[i].frame && f < kPlay[i].frame + kPlay[i].len) r |= kPlay[i].buttons;
  return r;
}
// Script 3: load save slot A (RIG_SRAM=file), then walk about (same events as the N64 AUTOPLAY_FILE build).
static const Ev kPlay3[] = {
#include "../../src/platform/n64/autoplay_file.h"
};
static int ScriptInput3(int f) {
  int r = 0;
  for (size_t i = 0; i < sizeof(kPlay3) / sizeof(kPlay3[0]); i++)
    if (f >= kPlay3[i].frame && f < kPlay3[i].frame + kPlay3[i].len) r |= kPlay3[i].buttons;
  return r;
}
static int ScriptInput(int f) { return g_script == 3 ? ScriptInput3(f) : g_script == 1 ? ScriptInput1(f) : g_script == 2 ? ScriptInput2(f) : ScriptInput0(f); }
static int ScriptInput0(int f) {
  if (f < 400) return 0;
  if (f < 1100) return (f % 60) < 5 ? START : 0;
  if (f < 2600) return (f % 40) < 5 ? A_ : 0;
  int phase = (f / 240) % 6, r = 0;
  switch (phase) {
  case 0: r = RIGHT; break;
  case 1: r = LEFT; break;
  case 2: r = RIGHT | ((f % 50) < 20 ? A_ : 0); break;
  case 3: r = LEFT | ((f % 30) < 3 ? X_ : 0); break;
  case 4: r = ((f % 30) < 3 ? X_ : 0) | UP; break;
  case 5: r = RIGHT | B_ | ((f % 70) < 30 ? A_ : 0); break;
  }
  if ((f % 997) < 4) r |= START;   // occasionally pause menu
  return r;
}

static uint32_t g_fb[240 * 320];

int main(int argc, char **argv) {
  int frames = 3000, dump_frame = -1;
#ifndef RIG_BE
  g_rom_path = argv[1];
  if (argc > 2) frames = atoi(argv[2]);
  if (argc > 4) { dump_frame = atoi(argv[3]); g_dump = fopen(argv[4], "wb"); }
  if (argc > 5) g_script = atoi(argv[5]);
  int snap_every = getenv("SNAP_EVERY") ? atoi(getenv("SNAP_EVERY")) : 0;
#else
  g_script = RIG_SCRIPT; int snap_every = 0;
  frames = RIG_FRAMES;
  dump_frame = RIG_DUMP_FRAME;
#endif
#ifdef RIG_DSP_DEFERRED   // N64 configuration: DSP writes only logged (the RSP makes the sound)
  { static DspLog l; static uint8_t ents[4096 * 4]; l.ents = ents; l.max = 4096; g_dsp_log = &l; g_dsp_deferred = true; }
#endif
#ifdef RIG_DSPCHECK
  rsph_init(); if (getenv("DSP_UCODE")) d_ucode = getenv("DSP_UCODE");
  g_rsp_trace = rspcyc_inst; g_rsp_dma_hook = rspcyc_dma;
  g_dlog.ents = g_dlog_ents; g_dlog.max = 4096; g_dsp_log = &g_dlog;
#endif
  if (!SnesInit("rom:/sm.smc")) { puts("SnesInit failed"); return 1; }
  g_spc_player = SpcPlayer_Create(); SpcPlayer_Initialize(g_spc_player); RtlReadSram();
  uint8_t *origin = (uint8_t *)g_fb + 8 * 1280 + 32 * 4;
  PpuBeginDrawing(g_snes->snes_ppu, origin, 1280, 1);
  PpuBeginDrawing(g_snes->my_ppu, origin, 1280, 1);
  static int16_t audio[534 * 2];
  for (int f = 0; f < frames; f++) {
    g_snes->disableRender = false;
#ifdef RIG_BE
#ifdef RIG_PROF_FROM
    if (f == RIG_PROF_FROM) hostcall(6, 1, 0, 0);
#endif
    hostcall(5, 0, 0, 0);
#endif
#ifdef RIG_PPUCHECK
    g_ppu_rsp_also_draw = true;
    PpuRsp_BeginFrame(g_snes->ppu, &g_pframe);
#endif
#ifdef RIG_CAPTURE_ONLY
    PpuRsp_BeginFrame(g_snes->ppu, &g_cframe);
#endif
    RtlRunFrame(ScriptInput(f));
#ifdef RIG_CAPTURE_ONLY
    PpuRsp_EndFrame(g_snes->ppu);
#endif
#ifdef RIG_PPUCHECK
    PpuRsp_EndFrame(g_snes->ppu);
    {
      PpuFrame *pf = &g_pframe;
      PpuRsp_RenderFrameC(pf, g_fb16, 256);
      // Dump sample frames for the RSP test harness (env DUMP_FRAMES=file, every DUMP_EVERY frames).
      static FILE *dumpf; static int dump_every;
      if (!dumpf && getenv("DUMP_FRAMES")) { dumpf = fopen(getenv("DUMP_FRAMES"), "wb"); dump_every = getenv("DUMP_EVERY") ? atoi(getenv("DUMP_EVERY")) : 50; }
      if (dumpf && f % dump_every == 0 && f >= (getenv("DUMP_FROM") ? atoi(getenv("DUMP_FROM")) : 0)) {
        int nrsp = 0; for (int l = 0; l < pf->numLines; l++) if ((pf->lines[l].flags & kLR_Valid) && !(pf->lines[l].flags & kLR_CpuDrawn)) nrsp++;
        if (nrsp) { int32_t hdr[2] = { f, (int32_t)sizeof(PpuFrame) }; fwrite(hdr, 4, 2, dumpf); fwrite(pf, sizeof(PpuFrame), 1, dumpf); }
      }
      {   // kLR_Copy decisions: a flagged line must render exactly like the previous frame's line
        static uint16_t prevout[240 * 256]; static bool have_prev; static long copies, cand, bad_copies;
        int nc = PpuRsp_MarkCopies(pf, have_prev ? &g_pframes[(f & 1) ^ 1] : NULL);
        copies += nc;
        for (int l = 0; l < pf->numLines; l++) {
          PpuLineRec *r = &pf->lines[l];
          if (!(r->flags & kLR_Valid) || (r->flags & kLR_CpuDrawn)) continue;
          cand++;
          if ((r->flags & kLR_Copy) && memcmp(prevout + l * 256, g_fb16 + l * 256, 512)) {
            if (bad_copies++ < 5) printf("BADCOPY frame %d line %d\n", f, l);
          }
        }
        memcpy(prevout, g_fb16, sizeof(prevout)); have_prev = true;
        if (f % 2000 == 1999) { extern long g_ppur_cwhy[8]; printf("COPY f=%d copied %ld of %ld lines, bad %ld  why flags %ld rec %ld cg %ld sliv %ld noblk %ld tmap %ld tiles %ld early %ld\n", f, copies, cand, bad_copies, g_ppur_cwhy[0], g_ppur_cwhy[1], g_ppur_cwhy[2], g_ppur_cwhy[3], g_ppur_cwhy[4], g_ppur_cwhy[5], g_ppur_cwhy[6], g_ppur_cwhy[7]); memset(g_ppur_cwhy, 0, sizeof(g_ppur_cwhy)); copies = cand = 0; }
      }
      if (getenv("SAMESTAT")) {   // lines whose output equals the previous frame's same line
        static uint16_t prevfb[240 * 256]; static long same_hud, same_rest, tot_hud, tot_rest;
        for (int l = 0; l < pf->numLines; l++) {
          PpuLineRec *r = &pf->lines[l];
          if (!(r->flags & kLR_Valid) || (r->flags & kLR_CpuDrawn)) continue;
          bool same = !memcmp(prevfb + l * 256, g_fb16 + l * 256, 512);
          if (l < 32) { tot_hud++; same_hud += same; } else { tot_rest++; same_rest += same; }
        }
        memcpy(prevfb, g_fb16, sizeof(prevfb));
        if (f % 1000 == 999) { printf("SAME f=%d hud %ld/%ld rest %ld/%ld\n", f, same_hud, tot_hud, same_rest, tot_rest); same_hud = same_rest = tot_hud = tot_rest = 0; }
      }
      st_frames++;
      if (pf->numCgrams > 1) st_multi_cgram++;
      if (pf->numCgrams > st_max_cg) st_max_cg = pf->numCgrams;
      if (pf->numSlivers > st_max_sliv) st_max_sliv = pf->numSlivers;
      for (int l = 0; l < pf->numLines; l++) {
        PpuLineRec *r = &pf->lines[l];
        if (!(r->flags & kLR_Valid)) continue;
        if (r->flags & kLR_CpuDrawn) { st_lines_cpu++; continue; }
        if (r->flags & kLR_ForcedBlank) st_lines_blank++; else st_lines_rsp++;
        uint32_t *ref = g_fb + (8 + l) * 320 + 32;
        int bad = -1;
        for (int x = 0; x < 256; x++) {
          uint32_t p = ref[x];
          uint16_t want = PPUR_PIX16(p >> 24, (p >> 16) & 0xff, (p >> 8) & 0xff);
          if (g_fb16[l * 256 + x] != want) { bad = x; break; }
        }
        if (bad >= 0) { st_mismatch_lines++; if (st_first_bad < 0 || st_mismatch_lines < 4) { if (st_first_bad < 0) st_first_bad = f; printf("BADLINE frame %d line %d x %d\n", f, l, bad); } }
      }
    }
#endif
#ifdef RIG_BE
    long n_game = hostcall(5, 0, 0, 0);
#endif
#ifdef RIG_DSPCHECK
    rsph_write(rsph_rdram, D_ARAM, g_spc_player->ram, 65536);
#endif
    RtlRenderAudio(audio, 534, 2);
#ifdef RIG_DSPCHECK
    DspCheckStep(f, audio);
#endif
#ifdef RIG_DSP_DEFERRED
    g_dsp_log->count = 0;
#endif
#ifdef RIG_BE
    long n_audio = hostcall(5, 0, 0, 0);
    printf("I%05d game=%ld audio=%ld\n", f, n_game, n_audio);
#endif
    Ppu *ppu = g_snes->ppu;
    uint32_t h_ram = fnv_bytes(2166136261u, g_ram, 0x20000);
    uint32_t h_sram = fnv_bytes(2166136261u, g_sram, 0x2000);
    uint32_t h_vram = fnv_u16(2166136261u, ppu->vram, 0x8000);
    uint32_t h_cg = fnv_u16(fnv_u16(2166136261u, ppu->cgram, 0x100), ppu->oam, 0x100);
    uint32_t h_pix = fnv_u32(2166136261u, g_fb, 240 * 320);
    uint32_t h_aud = fnv_u16(2166136261u, (uint16_t *)audio, 534 * 2);
    { extern long g_ppur_bstat[4]; static long pb; if (getenv("BSTAT")) printf("B%05d %ld\n", f, g_ppur_bstat[1] - pb); pb = g_ppur_bstat[1]; }
    printf("F%05d ram=%08lx sram=%08lx vram=%08lx cgoam=%08lx pix=%08lx aud=%08lx\n", f,
           (unsigned long)h_ram, (unsigned long)h_sram, (unsigned long)h_vram,
           (unsigned long)h_cg, (unsigned long)h_pix, (unsigned long)h_aud);
#ifndef RIG_BE
    if (snap_every && f % snap_every == 0) {
      char fn[64]; sprintf(fn, "snaps/s%05d.ppm", f); FILE *sf = fopen(fn, "wb");
      fprintf(sf, "P6 320 240 255\n");
      for (int i = 0; i < 320 * 240; i++) { uint8_t c[3] = { (uint8_t)(g_fb[i] >> 24), (uint8_t)(g_fb[i] >> 16), (uint8_t)(g_fb[i] >> 8) }; fwrite(c, 1, 3, sf); }
      fclose(sf);
    }
#endif
    if (f == dump_frame) {
      static uint8_t logical[0x20000];
      for (int i = 0; i < 0x20000; i++) logical[i] = RAM_LOGICAL_BYTE(g_ram, i);
      dump_block(logical, 0x20000);
      dump_block(g_fb, sizeof(g_fb));
    }
  }
#ifdef RIG_PPUCHECK
  { extern long g_ppur_m7stat[4]; printf("M7 lines %ld norot %ld scale1 %ld large %ld\n", g_ppur_m7stat[0], g_ppur_m7stat[1], g_ppur_m7stat[2], g_ppur_m7stat[3]); }
  { extern long g_ppur_bstat[4]; printf("BLOCKS hits %ld builds %ld column-only %ld\n", g_ppur_bstat[0], g_ppur_bstat[1], g_ppur_bstat[2]); }
  { extern long g_ppur_modes[16]; printf("MODES"); for (int i = 0; i < 16; i++) printf(" %d:%ld", i, g_ppur_modes[i]); printf("\n"); }
  { extern long g_ppur_why[6]; printf("WHY mode=%ld mosaic=%ld extra=%ld vram_mid=%ld overflow=%ld\n", g_ppur_why[0], g_ppur_why[1], g_ppur_why[2], g_ppur_why[3], g_ppur_why[4]); }
  printf("PPUCHECK frames=%ld rsp_lines=%ld blank_lines=%ld cpu_lines=%ld MISMATCH_LINES=%ld multi_cgram_frames=%ld max_cgrams=%ld max_slivers=%ld first_bad=%d\n",
         st_frames, st_lines_rsp, st_lines_blank, st_lines_cpu, st_mismatch_lines, st_multi_cgram, st_max_cg, st_max_sliv, st_first_bad);
#endif
#ifdef RIG_DSPCHECK
  printf("DSPCHECK nonzero_ref=%ld steps=%ld bad_steps=%ld bad_samples=%ld bad_echo_steps=%ld max_log=%ld | RSP instr/step avg %.0fK max %.0fK\n", d_nonzero, d_steps, d_bad_steps, d_bad_samples,
         d_bad_echo, d_logmax, d_steps ? d_instr_tot / 1e3 / d_steps : 0, d_instr_max / 1e3);
#endif
#ifdef RIG_DSPCHECK
  printf("DSP model: RSP cycles/step avg %.0fK max %.0fK (%.1f ms avg)\n", d_steps ? d_cyc_tot / 1e3 / d_steps : 0, d_cyc_max / 1e3, d_steps ? d_cyc_tot / 62.5e3 / d_steps : 0);
  { extern unsigned long long g_rspcyc_pc[1024]; FILE *cf = fopen("cyc_hist.txt", "w"); for (int i = 0; i < 1024; i++) if (g_rspcyc_pc[i]) fprintf(cf, "%x %llu\n", 0x1000 + i * 4, g_rspcyc_pc[i]); fclose(cf); }
  { extern unsigned long long g_rsp_pc_hist[1024]; FILE *hf = fopen("pc_hist.txt", "w"); for (int i = 0; i < 1024; i++) if (g_rsp_pc_hist[i]) fprintf(hf, "%x %llu\n", 0x1000 + i * 4, g_rsp_pc_hist[i]); fclose(hf); }
#endif
#ifdef RIG_DSPSTATS
  { DspStats *d = &g_dsp_stats; printf("DSPSTATS samples=%ld pmon=%ld non=%ld edl0=%ld reset=%ld high=%ld edl0_writes=%ld echo_samples=%ld pmod_samples=%ld\n",
      d->samples, d->pmon, d->non, d->edl0, d->reset, d->high, d->edl0_writes, d->echo_samples, d->pmod_samples); }
#endif
  puts("DONE");
  return 0;
}
