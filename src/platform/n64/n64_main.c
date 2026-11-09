// N64 platform layer for snesrev/sm (Super Metroid C port).
// Replaces the PC main.c (SDL2 window/audio/input) with libdragon.
//
// Video : each frame's SNES PPU state is captured per scanline (snes/ppu_rsp.c)
//         and drawn by the RSP (rsp_ppu.S) into a 320x240 RGBA5551 framebuffer,
//         256x224 picture centred (32 px side borders, 8 px top/bottom).
//         The RSP draws frame N while the CPU runs frame N+1 (3 framebuffers).
//         Mode-7 lines are still drawn by the CPU.
//         2.5D look (removed from the build: FX25D 0 below): rsp_fx.S bevelled
//         the level blocks and added drop shadows behind blocks and sprites.
// Input : controller 1, see kButtonMap below.
// Audio : the C reimplementation of the SPC sound driver + DSP, 32 kHz stereo.
// Saves : cartridge SRAM (256 Kbit). Requires the ROM header save type, set in the Makefile.
// Timing: game logic is stepped at 60 Hz on a fixed clock, so a PAL (50 Hz)
//         console runs the game at the correct speed by doing 1-2 steps per
//         displayed frame (same scheme as the earlier milestone-1 main.c).

#include <libdragon.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

// ---- Minimal declarations from the game sources ---------------------------
// (Including the full game headers next to libdragon.h clashes on type names,
//  so only what this file needs is declared here.)
typedef struct Snes Snes;
typedef struct Ppu Ppu;
typedef struct SpcPlayer SpcPlayer;
#include "../../config.h"  // Config struct (SDL parts are compiled out on N64)
#include "../../snes/ppu_rsp.h"
#include "../../snes/dsp.h"
#include "../../spc_player.h"

Snes *SnesInit(const char *filename);
bool RtlRunFrame(int inputs);
void RtlReadSram(void);
void RtlRenderAudio(int16_t *audio_buffer, int samples, int channels);
void PpuBeginDrawing(Ppu *ppu, uint8_t *pixels, size_t pitch, uint32_t render_flags);
SpcPlayer *SpcPlayer_Create(void);
void SpcPlayer_Initialize(SpcPlayer *p);
void Snes_N64_GetPpus(Ppu **a, Ppu **b);   // small accessors added in n64_glue.c
void Snes_N64_SetDisableRender(bool v);
Ppu *Snes_N64_ActivePpu(void);

DEFINE_RSP_UCODE(rsp_ppu);
DEFINE_RSP_UCODE(rsp_dsp);
DEFINE_RSP_UCODE(rsp_fx);
DEFINE_RSP_UCODE(rsp_m7);

// Job block at DMEM 0, same layout as JOB_* in rsp_dsp.S.
typedef struct __attribute__((aligned(8))) RspDspJob {
  uint32_t aram, state, log;
  uint16_t nlog, nsamp;
  uint32_t out;
  uint32_t pad[3];
} RspDspJob;

// Job block at DMEM 0, same layout as JOB_* in rsp_ppu.S.
typedef struct __attribute__((aligned(8))) RspPpuJob {
  uint32_t vram, lines, cgrams, slivers, fb, stride;
  uint16_t nlines, dbgflags;
  uint32_t dbg, blocks;
  uint32_t m7rows;
  int32_t prevd;                  // previous frame's framebuffer - this one's (for kLR_Copy lines)
  uint32_t mask;                  // 0, or where the RSP stores drawn lines' main z-buffers (for rsp_fx)
} RspPpuJob;

// Job block at DMEM 0, same layout as JOB_* in rsp_m7.S.
typedef struct __attribute__((aligned(16))) RspM7Job {
  uint32_t map, pix, out, list;
  uint16_t n, pad;
  uint32_t pad2[3];
} RspM7Job;

// Job block at DMEM 0, same layout as JOB_* in rsp_fx.S.
typedef struct __attribute__((aligned(16))) RspFxJob {
  uint32_t fb, stride, mask;
  uint16_t nlines;
  uint8_t sh[2];                  // stride = (1 << sh[0]) + (1 << sh[1])
  uint32_t pad2[4];
  uint32_t fxbits[8];             // bit (y & 31) of word y >> 5: apply the effect to line y
  uint32_t validbits[8];          // line y's mask row is valid
} RspFxJob;

static PpuFrame g_frames[2] __attribute__((aligned(16)));

// ---- Globals the PC main.c used to provide -------------------------------
bool g_debug_flag;
bool g_is_turbo;
bool g_want_dump_memmap_flags;
bool g_new_ppu = true;
bool g_other_image;
SpcPlayer *g_spc_player;
int g_got_mismatch_count;
Config g_config;

// ---- Settings --------------------------------------------------------------
#define ENABLE_AUDIO     1        // set to 0 to measure CPU cost without sound
#ifndef DSP_ON_RSP
#define DSP_ON_RSP       1        // 1: SNES sound DSP runs on the RSP (rsp_dsp.S); 0: on the CPU (snes/dsp.c)
#endif
#define LOGIC_STEP_US    16639    // SNES NTSC frame: 1 / 60.0988 Hz
#define MAX_STEPS        2        // max logic steps per displayed frame. More keeps game speed when the CPU is
                                  // overloaded but makes the picture very choppy (6 gave ~5-7 fps); 2 still covers PAL 50 Hz.
#define AUDIO_FREQ       32000
#define SAMPLES_PER_STEP 534      // what the DSP produces per SNES frame
#define SCREEN_W 320
#define SCREEN_H 240
#define SNES_W   256
#define SNES_H   224
#define SNES_LEFT ((SCREEN_W - SNES_W) / 2)   // game picture position in the framebuffer
#define SNES_TOP  ((SCREEN_H - SNES_H) / 2)
#define kPpuRenderFlags_NewRenderer 1
// ---- Colour filters (L + C-up cycles) --------------------------------------------
// The game shows at most 256 colours at a time, so a filter is applied to the
// palette (and colour-math fixed colours), not to every pixel, and a colour is
// only worked out again when it changes (PpuRsp_FilterPalette): it costs
// nothing measurable and needs no big tables (memory is nearly full).
enum { kCF_Off, kCF_Brighter, kCF_Darker, kCF_Vivid, kCF_Warm, kCF_Sepia, kCF_GameBoy, kCF_Grey, kCF_Contrast, kCF_Count };
static const char *const kCfName[kCF_Count] = {
  "off", "brighter", "darker", "vivid colours", "warm CRT tint", "sepia", "Game Boy green", "greyscale", "high contrast" };
static int g_cf;                         // current filter
static int g_cf_fm;                      // filter used for fixed (colour-math) colours
static uint64_t g_cf_msg_until;          // show its name until (us)
static uint8_t g_cf_gam[32];             // 5-bit channel -> 8-bit, with the filter's gamma

static int CfClamp(float v) { return v < 0 ? 0 : v > 255 ? 255 : (int)(v + 0.5f); }
// One colour through filter m (r, g, b: 5-bit channels; out: 8-bit channels).
static void CfPixel(int m, int r5, int g5, int b5, int o[3]) {
  int r = r5 << 3 | r5 >> 2, g = g5 << 3 | g5 >> 2, b = b5 << 3 | b5 >> 2;
  float L = 0.299f * r + 0.587f * g + 0.114f * b;
  switch (m) {
  case kCF_Brighter: case kCF_Darker:
    o[0] = g_cf_gam[r5]; o[1] = g_cf_gam[g5]; o[2] = g_cf_gam[b5]; return;
  case kCF_Vivid:
    o[0] = CfClamp(L + (r - L) * 1.5f); o[1] = CfClamp(L + (g - L) * 1.5f); o[2] = CfClamp(L + (b - L) * 1.5f); return;
  case kCF_Warm:   // a little redder and less blue, slightly lifted, like an old warm-set CRT
    o[0] = CfClamp(g_cf_gam[r5] * 1.06f + 6); o[1] = CfClamp(g_cf_gam[g5] * 0.99f + 2); o[2] = CfClamp(g_cf_gam[b5] * 0.82f); return;
  case kCF_Sepia:
    o[0] = CfClamp(0.393f * r + 0.769f * g + 0.189f * b);
    o[1] = CfClamp(0.349f * r + 0.686f * g + 0.168f * b);
    o[2] = CfClamp(0.272f * r + 0.534f * g + 0.131f * b); return;
  case kCF_GameBoy: {   // the original Game Boy's four greens (the darkest at half: rooms that add
                        // two layers together double it, which then gives the real darkest green)
    static const uint8_t k[4][3] = { { 8, 28, 8 }, { 48, 98, 48 }, { 139, 172, 15 }, { 155, 188, 15 } };
    int i = L < 50 ? 0 : L < 105 ? 1 : L < 170 ? 2 : 3;
    o[0] = k[i][0]; o[1] = k[i][1]; o[2] = k[i][2]; return;
  }
  case kCF_Grey:
    o[0] = o[1] = o[2] = CfClamp(L); return;
  case kCF_Contrast: {
    float rr = L + (r - L) * 1.2f, gg = L + (g - L) * 1.2f, bb = L + (b - L) * 1.2f;
    o[0] = CfClamp((rr - 128) * 1.45f + 128); o[1] = CfClamp((gg - 128) * 1.45f + 128); o[2] = CfClamp((bb - 128) * 1.45f + 128); return;
  }
  default:
    o[0] = r; o[1] = g; o[2] = b;
  }
}
static uint16_t CfTo15(const int o[3]) {
  int r = (o[0] + 4) >> 3, g = (o[1] + 4) >> 3, b = (o[2] + 4) >> 3;
  return (uint16_t)((r > 31 ? 31 : r) | (g > 31 ? 31 : g) << 5 | (b > 31 ? 31 : b) << 10);
}
static uint16_t CfColor(uint16_t v) { int o[3]; CfPixel(g_cf, v & 31, v >> 5 & 31, v >> 10 & 31, o); return CfTo15(o); }
static uint16_t CfFixed(uint16_t v) { int o[3]; CfPixel(g_cf_fm, v & 31, v >> 5 & 31, v >> 10 & 31, o); return CfTo15(o); }

static void ColorFilter_Set(int m) {
#ifdef CF_CYCLE
  debugf("CFSET %d\n", m);
#endif
  g_ppur_cf_mode = 0;
  g_cf = m;
  float e = m == kCF_Brighter ? 0.68f : m == kCF_Darker ? 1.45f : m == kCF_Warm ? 0.92f : 1.0f;
  for (int i = 0; i < 32; i++) g_cf_gam[i] = (uint8_t)CfClamp(255.0f * powf((i << 3 | i >> 2) / 255.0f, e));
  // fixed colours are amounts added / subtracted by colour math: brightness and contrast
  // filters leave them alone, the Game Boy look turns them grey (0 stays 0)
  g_cf_fm = (m == kCF_Brighter || m == kCF_Darker || m == kCF_Contrast) ? kCF_Off : m == kCF_GameBoy ? kCF_Grey : m;
  g_ppur_cf_color = CfColor;
  g_ppur_cf_fixed = CfFixed;
  g_ppur_cf_gen++;
  g_ppur_cf_mode = m;
}
// RDP colours (0xRRGGBBAA) of the 3D models through the same filter.
static inline uint32_t CfRGBA(uint32_t c) {
  if (!g_ppur_cf_mode) return c;
  int o[3];
  CfPixel(g_cf, c >> 27 & 31, c >> 19 & 31, c >> 11 & 31, o);
  return (uint32_t)o[0] << 24 | (uint32_t)o[1] << 16 | (uint32_t)o[2] << 8 | (c & 0xff);
}

#ifndef LOWPOLY
#define LOWPOLY 1                 // low-poly look: RDP-drawn models in place of the hero's and opponents' sprites
#endif
#if LOWPOLY
#include "lowpoly.c"
#else
bool g_lowpoly_on = false;        // (referenced by the game's draw hooks)
bool LowPoly_Active(void) { return false; }
void LowPoly_BeginStep(void) {}
void LowPoly_NoteHero(int, int, int, int, int, int, int, bool) {}
bool LowPoly_NoteEnemy(int, int, int, int, unsigned, uint32_t, bool, int) { return false; }
void LowPoly_NoteHeroOam(int, int, int, int, bool) {}
void LowPoly_NoteHeroOamFirst(int) {}
void LowPoly_NoteHeroTiles(int, const uint8_t *, int) {}
bool LowPoly_EnemySprites(void) { return false; }
bool LowPoly_BeginEnemy(void) { return false; }
bool LowPoly_NoteEnemyOam(int, int, int, int, bool) { return false; }
bool LowPoly_EndEnemy(void) { return false; }
#endif

static void ShowErrorAndHalt(const char *msg);

void NORETURN Die(const char *error) {
  debugf("Error: %s\n", error);
  ShowErrorAndHalt(error);
}

void Warning(const char *error) {
  debugf("Warning: %s\n", error);
}

// Audio is generated on the main thread between frames, so no lock is needed.
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}

// ---- ROM / SRAM -------------------------------------------------------------
// Loads the ROM from the cartridge filesystem into a buffer rounded up to the
// next power of two, because the loader mirrors the ROM in place to that size
// (3 MB -> 4 MB). The buffer is kept as the emulated cartridge ROM.
uint8_t *N64_ReadRom(const char *name, size_t *length) {
  FILE *f = fopen(name, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  size_t size = ftell(f);
  fseek(f, 0, SEEK_SET);
  size_t cap = 0x8000;
  while (cap < size) cap *= 2;
  uint8_t *buf = malloc(cap);
  if (!buf) { fclose(f); Die("Out of memory loading ROM (is the Expansion Pak inserted?)"); }
  if (fread(buf, 1, size, f) != size) { fclose(f); free(buf); return NULL; }
  fclose(f);
  *length = size;
  return buf;
}

static bool g_has_sram;

void N64_ReadSram(uint8_t *sram, size_t size) {
  if (g_has_sram) {
    sram_read(sram, 0, size);
    // A blank cart SRAM is all 0xFF; the game treats that as no save files, same as zeros.
  }
}

void N64_WriteSram(const uint8_t *sram, size_t size) {
  if (g_has_sram)
    sram_write(sram, 0, size);
}

// ---- Input ------------------------------------------------------------------
// SNES joypad bit layout used by RtlRunFrame:
enum {
  kSnes_B = 1 << 0, kSnes_Y = 1 << 1, kSnes_Select = 1 << 2, kSnes_Start = 1 << 3,
  kSnes_Up = 1 << 4, kSnes_Down = 1 << 5, kSnes_Left = 1 << 6, kSnes_Right = 1 << 7,
  kSnes_A = 1 << 8, kSnes_X = 1 << 9, kSnes_L = 1 << 10, kSnes_R = 1 << 11,
};

// Super Metroid default controls: A jump, X shoot, B dash, Y item cancel,
// Select item select, L/R aim.
static int ReadInputs(void) {
  joypad_poll();
  joypad_inputs_t in = joypad_get_inputs(JOYPAD_PORT_1);
  int r = 0;
  if (in.btn.a)       r |= kSnes_A;       // jump
  if (in.btn.b)       r |= kSnes_X;       // shoot
  if (in.btn.c_down)  r |= kSnes_B;       // dash
  if (in.btn.c_left)  r |= kSnes_Y;       // item cancel
  if (in.btn.z)       r |= kSnes_Select;  // item select
  if (in.btn.c_right) r |= kSnes_Select;
  if (in.btn.start)   r |= kSnes_Start;
  if (in.btn.l)       r |= kSnes_L;
  if (in.btn.r)       r |= kSnes_R;
  if (in.btn.d_up    || in.stick_y >  40) r |= kSnes_Up;
  if (in.btn.d_down  || in.stick_y < -40) r |= kSnes_Down;
  if (in.btn.d_left  || in.stick_x < -40) r |= kSnes_Left;
  if (in.btn.d_right || in.stick_x >  40) r |= kSnes_Right;
  return r;
}

#ifdef AUTOPLAY
// Test builds: replay the be_rig's gameplay script (new game, Ceres, Zebes...)
// by logic step, so a run in an emulator follows the same path at any speed.
typedef struct { int frame, len, buttons; } AutoEv;
static const AutoEv kAutoPlay[] = {
#ifdef AUTOPLAY_FILE
#include "autoplay_file.h"      // load save slot A, then walk about (performance tests)
#else
#include "autoplay_script.h"
#endif
};
static int AutoInput(int f) {
  int r = 0;
  for (size_t i = 0; i < sizeof(kAutoPlay) / sizeof(kAutoPlay[0]); i++)
    if (f >= kAutoPlay[i].frame && f < kAutoPlay[i].frame + kAutoPlay[i].len) r |= kAutoPlay[i].buttons;
  return r;
}
static int g_auto_step;
#endif

// ---- RSP scheduling ----------------------------------------------------------
// Each shown frame the RSP runs two jobs back to back: the sound for the
// frame's logic steps (rsp_dsp), then the picture (rsp_ppu). The CPU starts
// the first; the SP interrupt starts the second, so the CPU doesn't wait.
#define DSP_LOG_MAX 1024                 // register writes per frame (~240 per step seen)
static uint8_t g_dsp_state[2048] __attribute__((aligned(16)));          // rsp_dsp.S persistent state
static uint8_t g_dsp_log_ents[2][DSP_LOG_MAX * 4 + 128] __attribute__((aligned(16)));
static int16_t g_dsp_out[MAX_STEPS * SAMPLES_PER_STEP * 2 + 8] __attribute__((aligned(16)));
static DspLog g_dlog;
static RspDspJob g_dsp_job;
static RspPpuJob g_ppu_job;
static unsigned g_lines_copied, g_lines_total;
static uint64_t g_t_mark;   // time in PpuRsp_MarkCopies (debug readout)   // lines the RSP copied from the previous frame (debug readout)
static volatile int g_rsp_stage;         // 0 idle, 4 mode 7 rows, 1 sound, 2 picture, 3 2.5D pass (in that order)
static RspM7Job g_m7_job;
static RspFxJob g_fx_job;
static volatile bool g_fx_run;           // run rsp_fx after rsp_ppu this frame
static volatile uint64_t g_rsp_tfx;      // rsp_fx time (us, debug readout)

static volatile uint64_t g_rsp_t0, g_rsp_t1, g_rsp_tsound, g_rsp_tpic;   // RSP busy time (us)
static volatile uint64_t g_rsp_tgov;     // RSP busy time for the 2.5D governor (us)
static void StartDsp(void);
static void RspInterrupt(void) {
  if (g_rsp_stage) {
    uint64_t now = get_ticks_us();
    if (g_rsp_stage == 1) g_rsp_tsound += now - g_rsp_t0; else g_rsp_tpic += now - g_rsp_t1;
    g_rsp_tgov += now - (g_rsp_stage == 1 ? g_rsp_t0 : g_rsp_t1);
    if (g_rsp_stage == 3) g_rsp_tfx += now - g_rsp_t1;
    g_rsp_t1 = now;
  }
  if (g_rsp_stage == 4) {
    g_ppur_m7_busy = false;
    StartDsp();
  } else if (g_rsp_stage == 1) {
    rsp_load(&rsp_ppu);
    rsp_load_data(&g_ppu_job, sizeof(g_ppu_job), 0);
    rsp_run_async();
    g_rsp_stage = 2;
  } else if (g_rsp_stage == 2 && g_fx_run) {
    rsp_load(&rsp_fx);
    rsp_load_data(&g_fx_job, sizeof(g_fx_job), 0);
    rsp_run_async();
    g_rsp_stage = 3;
  } else {
    g_rsp_stage = 0;
  }
}

// The sound job (or the picture, if sound isn't on the RSP).
static void StartDsp(void) {
#if DSP_ON_RSP
  g_rsp_stage = 1;
  g_rsp_t0 = get_ticks_us();
  rsp_load(&rsp_dsp);
  rsp_load_data(&g_dsp_job, sizeof(g_dsp_job), 0);
  rsp_run_async();
#else
  g_rsp_stage = 2;
  rsp_load(&rsp_ppu);
  rsp_load_data(&g_ppu_job, sizeof(g_ppu_job), 0);
  rsp_run_async();
#endif
}

// ---- 2.5D look -------------------------------------------------------------------
// rsp_fx.S needs the z values of rows y-3 .. y+1 (written by rsp_ppu.S to
// g_fx_mask while the effect is on). A line copied from the previous frame
// (kLR_Copy) gets that frame's finished pixels, effect included, so it may
// only be copied when those neighbour rows are unchanged too.
#define FX_TOP 32                        // rows above are the HUD
static uint8_t *g_fx_mask;               // SNES_H lines x 512 bytes, only the RSP touches it
#ifndef FX25D
#define FX25D 0                          // 2.5D look (bevels and shadows): 0 = not in the game at all (it caused
                                         // glitches when the screen changes, e.g. doors); 1 = back (on, automatic per room)
#endif
static bool g_fx_on = FX25D;
static RspFxJob g_fx_next;               // job for the frame being prepared
static bool g_fx_next_run;
static bool g_fx_prev;                   // the previous frame got the effect
static int g_fx_prev_n;

static bool FxGameplay(void) {
  extern uint8_t g_ram[];
  unsigned gs = g_ram[0x998] | g_ram[0x999] << 8;   // game_state (the game's RAM is little-endian)
  return (gs >= 7 && gs <= 0xb) || gs == 0x1b || gs == 0x2a;   // gameplay, doors, reserve tanks, demos
}

// Automatic 2.5D: the effect costs RSP time on every line that isn't copied
// from the previous frame (3-6 ms in busy rooms). In a room where the RSP
// can't finish a picture per TV frame with it, it is switched off for that
// room (remembered for the rest of the session), so the frame rate stays up.
// L + C-up still switches it on and off by hand (by hand "on" also overrides
// the automatic choice for the current room).
#define FX_HEAVY_MAX 64
static uint16_t g_fx_heavy[FX_HEAVY_MAX];   // rooms (room_ptr) too busy for the effect
static int g_fx_nheavy;
static bool g_fx_auto_off;               // off for the current room
static bool g_fx_forced;                 // switched on by hand in the current room
static uint16_t g_fx_room;
static int g_gov_frames;
static uint64_t g_gov_t0;

static bool FxRoomHeavy(uint16_t room) {
  for (int i = 0; i < g_fx_nheavy; i++) if (g_fx_heavy[i] == room) return true;
  return false;
}

// Once per shown frame. ran_fx: the effect ran on the frame the RSP just finished.
static void FxGovernor(bool ran_fx) {
  extern uint8_t g_ram[];
  uint16_t room = g_ram[0x79b] | g_ram[0x79c] << 8;   // room_ptr
  uint64_t now = get_ticks_us();
  if (room != g_fx_room) {
    g_fx_room = room;
    g_fx_forced = false;
    g_fx_auto_off = FxRoomHeavy(room);
    g_gov_frames = 0;
  }
  if (!ran_fx || g_fx_auto_off || g_fx_forced) { g_gov_frames = 0; return; }
  if (g_gov_frames == 0) { g_gov_t0 = now; g_rsp_tgov = 0; }
  if (++g_gov_frames < 31) return;
  // 30 frames with the effect: was the RSP busy for (almost) the whole frame time,
  // and did pictures come slower than the TV's rate?
  float frame_us = 1000000.0f / vi_get_refresh_rate();
  uint64_t span = now - g_gov_t0;
  bool rsp_full = g_rsp_tgov > (uint64_t)(0.93f * (float)span);
  bool slow = span > (uint64_t)(30 * frame_us * 1.04f);
  if (rsp_full && slow) {
    g_fx_auto_off = true;
    if (g_fx_nheavy < FX_HEAVY_MAX) g_fx_heavy[g_fx_nheavy++] = room;
    debugf("2.5D look off in room %04x (RSP busy %u%% of the time)\n", room, (unsigned)(g_rsp_tgov * 100 / (span ? span : 1)));
  }
  g_gov_frames = 0;
}

// Sets g_fx_next's line bits and g_fx_next_run (used once the RSP is free); may clear kLR_Copy. Returns lines un-copied.
static int FxPrepare(PpuFrame *f, int n) {
  bool on = g_fx_on && !g_fx_auto_off && g_fx_mask && FxGameplay();
  int uncopied = 0;
  memset(g_fx_next.fxbits, 0, sizeof(g_fx_next.fxbits));
  memset(g_fx_next.validbits, 0, sizeof(g_fx_next.validbits));
  const uint8_t bad = kLR_CpuDrawn | kLR_ForcedBlank | kLR_Mode7;
  if (on != g_fx_prev || (on && n != g_fx_prev_n)) {
    // copies would bring the other look along
    for (int l = FX_TOP; l < n; l++)
      if (f->lines[l].flags & kLR_Copy) { f->lines[l].flags &= ~kLR_Copy; uncopied++; }
  }
  g_fx_prev = on;
  g_fx_prev_n = n;
  g_fx_next_run = on;
  if (!on) return uncopied;
  uint8_t stable[SNES_H + 1];            // row y's mask row is the same as last frame
  for (int l = 0; l < n; l++) {
    uint8_t fl = f->lines[l].flags;
    bool valid = (fl & kLR_Valid) && !(fl & bad) && l >= FX_TOP;
    stable[l] = l < FX_TOP || (fl & kLR_Copy);
    if (valid) g_fx_next.validbits[l >> 5] |= 1u << (l & 31);
  }
  stable[n] = 1;                         // past the picture: "outside" both frames
  for (int l = FX_TOP; l < n; l++) {
    if (!(g_fx_next.validbits[l >> 5] >> (l & 31) & 1)) continue;
    uint8_t *fl = &f->lines[l].flags;
    if ((*fl & kLR_Copy) && !(stable[l - 3] && stable[l - 2] && stable[l - 1] && stable[l + 1])) { *fl &= ~kLR_Copy; uncopied++; }
    if (!(*fl & kLR_Copy)) g_fx_next.fxbits[l >> 5] |= 1u << (l & 31);
  }
  g_fx_next.nlines = n;
  return uncopied;
}

// ---- Audio --------------------------------------------------------------------
#if ENABLE_AUDIO
// Ring buffer between the game (534 stereo samples per logic step) and the
// libdragon audio buffers (fixed length chosen by libdragon).
#define RING_FRAMES 4096
static int16_t g_ring[RING_FRAMES * 2];
static int g_ring_rd, g_ring_count;

static void AudioPush(const int16_t *samples, int n) {
  if (n > RING_FRAMES - g_ring_count) n = RING_FRAMES - g_ring_count;   // overflow: drop
  while (n > 0) {   // block copies up to the wrap point (4 bytes per stereo frame)
    int wr = (g_ring_rd + g_ring_count) & (RING_FRAMES - 1);
    int k = RING_FRAMES - wr < n ? RING_FRAMES - wr : n;
    memcpy(&g_ring[wr * 2], samples, k * 4);
    samples += k * 2, n -= k, g_ring_count += k;
  }
}

// Runs the SPC sound driver for one logic step. With DSP_ON_RSP the DSP part
// only records register writes; the RSP turns them into samples at frame end.
static void AudioProduceStep(void) {
  static int16_t tmp[SAMPLES_PER_STEP * 2];
  RtlRenderAudio(tmp, SAMPLES_PER_STEP, 2);
#if !DSP_ON_RSP
  AudioPush(tmp, SAMPLES_PER_STEP);
#endif
}

static void AudioPump(void) {
  int len = audio_get_buffer_length();
  while (audio_can_write() && g_ring_count >= len) {
    int16_t *out = audio_write_begin();
    for (int done = 0; done < len;) {
      int k = RING_FRAMES - g_ring_rd < len - done ? RING_FRAMES - g_ring_rd : len - done;
      memcpy(out + done * 2, &g_ring[g_ring_rd * 2], k * 4);
      g_ring_rd = (g_ring_rd + k) & (RING_FRAMES - 1);
      done += k;
    }
    g_ring_count -= len;
    audio_write_end();
  }
}
#endif

// ---- Error screen -------------------------------------------------------------
static void ShowErrorAndHalt(const char *msg) {
  for (int i = 0; i < 3; i++) {   // fill every buffer so the message stays up
    surface_t *fb = display_get();
    graphics_fill_screen(fb, graphics_make_color(0, 0, 80, 255));
    graphics_draw_text(fb, 16, 16, "Super Metroid N64 port");
    graphics_draw_text(fb, 16, 40, msg);
    display_show(fb);
  }
  for (;;) {}
}

#if defined(RSP_SELFCHECK) || defined(M7CHECK)
// Test builds: the rows rsp_m7.S built vs the same window of the plane worked out here.
static void M7Check(const PpuFrame *f) {
  static unsigned m7rows, m7bad;
  for (int e = 0; e < f->numM7Jobs; e++) {
    const uint16_t *j = f->m7job[e];
    if (f->m7key[j[0]][3] >> 2 != PpuRsp_M7Gen()) continue;   // the data changed since (the next frame's capture)
    const uint8_t *row = (const uint8_t *)UncachedAddr(f->m7rows[j[0]]);
    for (int x = 0; x < 256; x++) {
      unsigned px = (j[2] + x) & 1023;
      uint8_t want = g_m7pix[g_m7map[(j[1] >> 3) * 128 + (px >> 3)] * 64 + (j[1] & 7) * 8 + (px & 7)];
      if (row[x] != want) {
        if (m7bad++ < 40) debugf("M7BAD idx %u Y %u X0 %u x %d rsp %02x want %02x (job %d of %d)\n", j[0], j[1], j[2], x, row[x], want, e, f->numM7Jobs);
        break;
      }
    }
    m7rows++;
  }
  if (f->numM7Jobs) debugf("M7CHECK rows %u bad %u\n", m7rows, m7bad);
}
#endif

#ifdef RSP_SELFCHECK
// Test build only: compares what the RSP drew with the C reference renderer
// for the same captured frame and prints the result via debugf.
static void RspSelfCheck(const PpuFrame *f, surface_t *fb) {
  static uint16_t ref[PPUR_MAX_LINES * SNES_W];
  static unsigned frame_no;
  int n = f->numLines < SNES_H ? f->numLines : SNES_H;
  PpuRsp_CgramsToSnes((PpuFrame *)f);   // (converted for the RSP; this frame is done with)
  PpuRsp_RenderFrameC(f, ref, SNES_W);
  const uint16_t *pix = (const uint16_t *)UncachedAddr(fb->buffer);
  int stride = fb->stride / 2;
  unsigned bad_lines = 0, checked = 0, first_bad = ~0u;
  uint32_t hash = 2166136261u;
  for (int y = 0; y < n; y++) {
    const PpuLineRec *r = &f->lines[y];
    if (!(r->flags & kLR_Valid) || (r->flags & kLR_CpuDrawn)) continue;
    checked++;
    const uint16_t *row = pix + (y + (SCREEN_H - SNES_H) / 2) * stride + (SCREEN_W - SNES_W) / 2;
    bool bad = false;
    for (int x = 0; x < SNES_W; x++) {
      if (row[x] != ref[y * SNES_W + x]) bad = true;
      hash = (hash ^ row[x]) * 16777619u;
    }
    if (bad) {
      bad_lines++;
      if (first_bad == ~0u) {
        first_bad = y;
        int x = 0; while (row[x] == ref[y * SNES_W + x]) x++;
        int nx = 0; for (int k = 0; k < SNES_W; k++) nx += row[k] != ref[y * SNES_W + k];
        debugf("  line %d x %d rsp %04x ref %04x ndiff %d flags %02x cg %d blk %04x %04x %04x se %02x %02x mf %02x math %02x sl %d\n", y, x, row[x],
               ref[y * SNES_W + x], nx, r->flags, r->cgramIdx, r->block[0], r->block[1], r->block[2], r->screenEnabled[0], r->screenEnabled[1],
               r->mathFlags, r->mathEnabled, r->sliverCount);
      }
    }
  }
  M7Check(f);
  debugf("SELFCHECK frame %u lines %u checked %u bad %u first_bad %d hash %08lx\n", frame_no++, n, checked, bad_lines,
         (int)first_bad, (unsigned long)hash);
}
#endif

// ---- 50 / 60 Hz ------------------------------------------------------------------
// A PAL console normally shows 50 pictures a second. libdragon's PAL60 timing
// makes it send a 60 Hz signal instead, which most CRTs and many (not all)
// TVs and upscalers accept. R + C-up switches; a switch to 60 Hz must be
// confirmed with A within 10 seconds or it goes back to 50 Hz (in case the
// TV shows nothing). The game itself always runs at its own 60 steps per second.
static bool g_pal60;
static uint64_t g_hz_deadline;           // 60 Hz on trial until this time (0: not on trial)

static void ToggleRefresh(void) {
  if (get_tv_type() != TV_PAL) return;   // NTSC / M-PAL consoles are 60 Hz already
  g_pal60 = !g_pal60;
  vi_set_timing_preset(g_pal60 ? &VI_TIMING_PAL60 : &VI_TIMING_PAL);
  g_hz_deadline = g_pal60 ? get_ticks_us() + 10000000 : 0;
  debugf("refresh: %s (%.2f Hz)\n", g_pal60 ? "PAL60" : "PAL", vi_get_refresh_rate());
}

// ---- Main ---------------------------------------------------------------------
int main(void) {
  debug_init_usblog();     // EverDrive: read with UNFLoader -d
  debug_init_isviewer();   // emulators (ares) show this in their console
#ifdef RSP_SELFCHECK
  display_init(RESOLUTION_320x240, DEPTH_16_BPP, 3, GAMMA_NONE, FILTERS_RESAMPLE);   // test build: memory for the reference renderer
#else
  display_init(RESOLUTION_320x240, DEPTH_16_BPP, 4, GAMMA_NONE, FILTERS_RESAMPLE);
#endif
  joypad_init();
#ifdef FORCE_PAL60
  ToggleRefresh();                       // test builds: 60 Hz from the start, no confirmation
  g_hz_deadline = 0;
#endif
  dfs_init(DFS_DEFAULT_LOCATION);
  sram_init();
  g_has_sram = sram_detect();

  debugf("TV type %d (0=PAL 1=NTSC 2=MPAL), RAM %d bytes, SRAM %s\n",
         (int)get_tv_type(), get_memory_size(), g_has_sram ? "yes" : "no");

  if (!is_memory_expanded())
    ShowErrorAndHalt("Needs the Expansion Pak (8 MB RAM).");

  // Mask image for the 2.5D pass (only the RSP reads and writes it): 112 KB in
  // one piece, so it is taken before the ROM and the other buffers.
#if FX25D
  g_fx_mask = (uint8_t *)malloc_uncached_aligned(16, SNES_H * 512);
  if (!g_fx_mask) debugf("2.5D look: no memory for its mask image, off\n");
#endif

  // Clear all framebuffers once; the PPU only writes the centre 256x224.
  for (int i = 0; i < 3; i++) {
    surface_t *fb = display_get();
    graphics_fill_screen(fb, graphics_make_color(0, 0, 0, 255));
    display_show(fb);
  }

  g_config.new_renderer = true;
  g_ppur_m7_rsp = true;              // unrotated, unscaled mode 7 rows are built by rsp_m7.S
  g_config.audio_freq = AUDIO_FREQ;
  g_config.audio_channels = 2;

  Snes *snes = SnesInit("rom:/sm.smc");
  if (!snes)
    ShowErrorAndHalt("Could not load rom:/sm.smc - see README.");

#if DSP_ON_RSP
  // From here on DSP register writes are recorded for rsp_dsp.S instead of being run.
  g_dlog.ents = g_dsp_log_ents[0];
  g_dlog.max = DSP_LOG_MAX;
  g_dsp_log = &g_dlog;
  g_dsp_deferred = true;
#endif
  g_spc_player = SpcPlayer_Create();
  SpcPlayer_Initialize(g_spc_player);
  assertf((PhysicalAddr(g_spc_player->ram) & 7) == 0, "SPC RAM must be 8-byte aligned for the RSP");

#if ENABLE_AUDIO
  audio_init(AUDIO_FREQ, AUDIO_DEFAULT_LATENCY);
#endif

  RtlReadSram();

  Ppu *ppu_a, *ppu_b;
  Snes_N64_GetPpus(&ppu_a, &ppu_b);

#if defined(RSP_SELFCHECK) || defined(FX_OFF)
  g_fx_on = false;                       // test builds: the picture must equal the C reference / speed without it
#endif
#if LOWPOLY
  LowPoly_Init();
#if defined(RSP_SELFCHECK) || defined(LOWPOLY_OFF)
  g_lowpoly_on = false;                  // the self-check compares the RSP's pixels only
#endif
#endif

  {
    heap_stats_t hs;
    sys_get_heap_stats(&hs);
    debugf("heap: %d KB total, %d KB used\n", hs.total / 1024, hs.used / 1024);
  }

  // rsp_init() without its interrupt handler: libdragon's handler reads RSP
  // IMEM after every job to look for an RSP assert, which this ROM's
  // standalone microcodes never use (3 jobs per frame).
  *SP_PC = 0x1000;
  *SP_STATUS = SP_WSTATUS_SET_HALT;
  set_SP_interrupt(1);
  register_SP_handler(RspInterrupt);
  // Buffers only the RSP writes: drop any dirty cache lines (e.g. from clearing .bss)
  // so they can never be evicted over RSP data later.
  data_cache_hit_writeback_invalidate(g_dsp_state, sizeof(g_dsp_state));
  data_cache_hit_writeback_invalidate(g_dsp_out, sizeof(g_dsp_out));

  uint64_t last = get_ticks_us();
  int64_t accum = LOGIC_STEP_US;
  int cur = 0;                      // capture buffer for this frame
  surface_t *pending_fb = NULL;     // frame the RSP is drawing
  int dsp_buf = 0;                  // log buffer being filled
  int dsp_pending_samples = 0;      // samples the last sound job produces
  RspPpuJob &job = g_ppu_job;

  // Performance counters, reported every second on screen (C-up toggles) and via debugf.
  bool show_perf = true;
  bool prev_cup = false;
  uint64_t t_logic = 0, t_draw = 0, t_audio = 0, t_wait = 0, t_window_start = get_ticks_us();
  int n_logic = 0, n_draw = 0, n_shown = 0;
  char perf_text[48] = "";

  for (;;) {
    surface_t *fb = display_get();   // 4 buffers: shown, queued to show, being drawn by the RSP, this one
    uint8_t *pixels = (uint8_t *)CachedAddr(fb->buffer);
    uint8_t *origin = pixels + ((SCREEN_H - SNES_H) / 2) * fb->stride + ((SCREEN_W - SNES_W) / 2) * 2;

    uint64_t now = get_ticks_us();
    accum += (int64_t)(now - last);
    last = now;

    // How many 60 Hz logic steps are due? Always at least one per shown frame.
    // If drawing is slow, several steps run per shown frame (frame skipping),
    // so the game itself keeps its normal speed.
    int steps = (int)(accum / LOGIC_STEP_US);
    if (steps < 1) steps = 1;
    if (steps > MAX_STEPS) { steps = MAX_STEPS; accum = 0; }
    else accum -= (int64_t)steps * LOGIC_STEP_US;
    if (accum < -LOGIC_STEP_US) accum = 0;

    int inputs = ReadInputs();
    {
      joypad_buttons_t held = joypad_get_buttons_held(JOYPAD_PORT_1);
      if (held.c_up && !prev_cup) {
        if (held.l && held.r) g_lowpoly_on = !g_lowpoly_on;   // L + R + C-up: low-poly look on / off
        else if (held.l) {                 // L + C-up: next colour filter
          ColorFilter_Set((g_cf + 1) % kCF_Count);
          g_cf_msg_until = get_ticks_us() + 3000000;
        }
        else if (held.r) ToggleRefresh();  // R + C-up: 50 / 60 Hz (PAL consoles)
        else show_perf = !show_perf;
      }
      if (g_hz_deadline) {               // 60 Hz on trial: A keeps it, otherwise back to 50 Hz
        if (held.a) g_hz_deadline = 0;
        else if (get_ticks_us() > g_hz_deadline) ToggleRefresh();
      }
      prev_cup = held.c_up;
    }
    PpuFrame *frame = &g_frames[cur];
    for (int s = 0; s < steps; s++) {
      bool last_step = (s == steps - 1);
      g_dlog.base = s * SAMPLES_PER_STEP;
      // Only the final step of this display frame is drawn.
#ifdef PROF_LOGIC
      Snes_N64_SetDisableRender(true);   // profiling build: logic steps only
#else
      Snes_N64_SetDisableRender(!last_step);
#endif
      if (last_step) {
        // Lines the RSP can't draw (mode 7) are drawn by the CPU straight into fb.
        PpuBeginDrawing(ppu_a, origin, fb->stride, kPpuRenderFlags_NewRenderer);
        PpuBeginDrawing(ppu_b, origin, fb->stride, kPpuRenderFlags_NewRenderer);
        PpuRsp_BeginFrame(Snes_N64_ActivePpu(), frame);
      }
      uint64_t t0 = get_ticks_us();
#ifdef AUTOPLAY
      inputs = AutoInput(g_auto_step++);
#ifdef CF_CYCLE
      if (g_auto_step >= 6000 && g_auto_step % CF_CYCLE == 0) {   // test builds: step through the colour filters
        ColorFilter_Set((g_auto_step - 6000) / CF_CYCLE % kCF_Count);
        g_cf_msg_until = get_ticks_us() + 3000000;
      }
#endif
      if (g_auto_step % 600 == 0) debugf("STEP %d\n", g_auto_step);
#endif
      LowPoly_BeginStep();
      RtlRunFrame(inputs);
      if (last_step) PpuRsp_EndFrame(Snes_N64_ActivePpu());
      uint64_t t1 = get_ticks_us();
      if (last_step) { t_draw += t1 - t0; n_draw++; }   // logic + capture
      else { t_logic += t1 - t0; n_logic++; }           // logic only
#if ENABLE_AUDIO
      AudioProduceStep();
      AudioPump();
      t_audio += get_ticks_us() - t1;
#endif
    }

#if LOWPOLY
    LowPoly_Capture(cur, FxGameplay(), frame);  // models to draw over this frame's picture
#endif
    // Lines whose pixels equal the previous frame's (same record, palette,
    // sprites and BG data): the RSP copies them from the previous framebuffer.
    {
      uint64_t tc = get_ticks_us();
      PpuRsp_ConvertCgrams(frame);     // palettes as the RSP takes them (RGBA5551)
      g_lines_copied += PpuRsp_MarkCopies(frame, pending_fb ? &g_frames[cur ^ 1] : NULL);
#if LOWPOLY
      if (pending_fb) g_lines_copied -= LowPoly_NoCopy(frame, cur ^ 1);   // models were drawn over those
#endif
      g_lines_copied -= FxPrepare(frame, frame->numLines < SNES_H ? frame->numLines : SNES_H);
      tc = get_ticks_us() - tc;
      t_draw += tc;                    // counted as capture time (C)
      g_t_mark += tc;
    }
    g_lines_total += frame->numLines;

    // Make the capture and any CPU-drawn lines visible to the RSP / video interface.
    data_cache_hit_writeback(frame->lines, sizeof(PpuLineRec) * frame->numLines);
    data_cache_hit_writeback(frame->cgram, 512 * frame->numCgrams);
    data_cache_hit_writeback(frame->slivers, sizeof(PpuSliver) * frame->numSlivers);
#if PPUR_REFERENCE
    data_cache_hit_writeback(frame->vram, sizeof(frame->vram));
#endif
    for (int i = 0; i < frame->numBlocks; i++)       // only blocks changed this frame
      if (frame->bwritten[i]) { data_cache_hit_writeback(&frame->blocks[i], sizeof(PpuTileBlock)); frame->bwritten[i] = 0; }
    // mode 7 rows: the CPU's are written back, and no cached copies are kept
    // (rsp_m7.S writes the others straight to RDRAM)
    data_cache_hit_writeback_invalidate(frame->m7rows, 256 * frame->numM7);
    if (frame->numM7Jobs) {
      data_cache_hit_writeback(frame->m7job, 8 * frame->numM7Jobs);
      if (g_ppur_m7_dirty) {
        g_ppur_m7_dirty = false;
        data_cache_hit_writeback(g_m7map, sizeof(g_m7map));
        data_cache_hit_writeback(g_m7pix, sizeof(g_m7pix));
      }
    }
    {   // lines the CPU drew went through the cache: write them out, and drop the lines
        // so they can't be written back over the RSP's pixels later
      extern bool g_ppu_cpu_drew;
      if (g_ppu_cpu_drew) data_cache_hit_writeback_invalidate(pixels, fb->stride * SCREEN_H);
      g_ppu_cpu_drew = false;
    }

    // Top border (8 px above the game picture): performance readout. The RSP never writes there.
    graphics_draw_box(fb, 0, 0, SCREEN_W, 8, graphics_make_color(0, 0, 0, 255));
    if (g_hz_deadline) {
      char t[48];
      unsigned left = (unsigned)((g_hz_deadline - get_ticks_us()) / 1000000) + 1;
      snprintf(t, sizeof(t), "60 Hz: press A to keep it (%u)", left > 10 ? 10 : left);
      graphics_draw_text(fb, 4, 0, t);
    } else if (show_perf) graphics_draw_text(fb, 4, 0, perf_text);
    // Bottom border (8 px below the picture): the colour filter's name for 3 s after a change.
    graphics_draw_box(fb, 0, SCREEN_H - 8, SCREEN_W, 8, graphics_make_color(0, 0, 0, 255));
    if (get_ticks_us() < g_cf_msg_until) {
      char t[64];
      snprintf(t, sizeof(t), "Colour filter: %s  (%d/%d)", kCfName[g_cf], g_cf + 1, kCF_Count);
      graphics_draw_text(fb, 4, SCREEN_H - 8, t);
    }
#if LOWPOLY
    else if (get_ticks_us() < 12000000) {   // the first seconds: the low-poly look's memory
      char t[64];
      if (g_lp_nomem) snprintf(t, sizeof(t), "Low-poly 3D off: not enough memory");
      else snprintf(t, sizeof(t), "Low-poly 3D on, %d KB RAM free%s", g_lp_freekb, g_lp_bossmem ? "" : ", bosses 2D");
      graphics_draw_text(fb, 4, SCREEN_H - 8, t);
    }
#endif

    // Sound log and SPC RAM (sample data, uploads) must be in RDRAM for the RSP.
#if DSP_ON_RSP
    data_cache_hit_writeback(g_spc_player->ram, 65536);
    data_cache_hit_writeback(g_dlog.ents, g_dlog.count * 4);
#endif

    // Finish and show the previous frame.
    {
      uint64_t tw = get_ticks_us();
      while (g_rsp_stage) {}
      t_wait += get_ticks_us() - tw;
    }
#if DSP_ON_RSP && ENABLE_AUDIO
    if (dsp_pending_samples) {     // last frame's sound
      data_cache_hit_invalidate(g_dsp_out, (dsp_pending_samples * 4 + 15) & ~15);   // buffer has 16 bytes slack
      AudioPush(g_dsp_out, dsp_pending_samples);
#ifdef RSP_SELFCHECK
      {
        static int peak, nz, n;
        for (int i = 0; i < dsp_pending_samples * 2; i++) { int v = abs(g_dsp_out[i]); if (v > peak) peak = v; nz += v != 0; }
        if (++n == 30) { debugf("AUDIOCHECK peak %d nonzero %d\n", peak, nz); peak = nz = n = 0; }
      }
#endif
      AudioPump();
    }
#endif
    if (pending_fb) {
#ifdef RSP_SELFCHECK
      RspSelfCheck(&g_frames[cur ^ 1], pending_fb);
#elif defined(M7CHECK)
      M7Check(&g_frames[cur ^ 1]);
#endif
#if !LOWPOLY
      display_show(pending_fb);
#endif
      n_shown++;
    }
    if (pending_fb) FxGovernor(g_fx_run);   // g_fx_run: the effect ran on that picture
    surface_t *show_fb = pending_fb;     // finished picture (shown below with LOWPOLY)
    // Start the RSP on this frame.
    job.vram = 0;   // (unused by the microcode)
    assertf((PhysicalAddr(frame->lines) & 7) == 0 &&
            (PhysicalAddr(frame->blocks) & 7) == 0 && (PhysicalAddr(frame->slivers) & 7) == 0 &&
            (PhysicalAddr(frame->cgram) & 7) == 0, "RSP job data must be 8-byte aligned");
    job.lines = PhysicalAddr(frame->lines);
    job.cgrams = PhysicalAddr(frame->cgram);
    job.slivers = PhysicalAddr(frame->slivers);
    job.blocks = PhysicalAddr(frame->blocks);
    job.m7rows = PhysicalAddr(frame->m7rows);
    job.fb = PhysicalAddr(fb->buffer) + ((SCREEN_H - SNES_H) / 2) * fb->stride + ((SCREEN_W - SNES_W) / 2) * 2;
    job.stride = fb->stride;
    job.prevd = pending_fb ? (int32_t)(PhysicalAddr(pending_fb->buffer) - PhysicalAddr(fb->buffer)) : 0;
    job.nlines = frame->numLines < SNES_H ? frame->numLines : SNES_H;   // never past the 224 picture rows
    job.dbgflags = 0;
    g_fx_job = g_fx_next;                // the RSP is idle here
    g_fx_run = g_fx_next_run;
    job.mask = g_fx_run ? PhysicalAddr(g_fx_mask) : 0;   // z rows only while the effect is on
    g_fx_job.fb = job.fb;
    g_fx_job.stride = job.stride;
    g_fx_job.mask = job.mask;
    g_fx_job.sh[0] = 31 - __builtin_clz(job.stride);
    g_fx_job.sh[1] = __builtin_ctz(job.stride);
    if (g_fx_job.sh[0] == g_fx_job.sh[1]) g_fx_job.sh[0] = g_fx_job.sh[1] = g_fx_job.sh[0] - 1;   // power of two: half + half
    assertf(job.stride == (1u << g_fx_job.sh[0]) + (1u << g_fx_job.sh[1]), "rsp_fx: stride %lu", (unsigned long)job.stride);
    data_cache_hit_writeback(&g_fx_job, sizeof(g_fx_job));
    job.dbg = 0;
    data_cache_hit_writeback(&job, sizeof(job));
#if DSP_ON_RSP
    if (g_dlog.overflow) { debugf("DSP log overflow (%d)\n", g_dlog.overflow); g_dlog.overflow = 0; }
    g_dsp_job.aram = PhysicalAddr(g_spc_player->ram);
    g_dsp_job.state = PhysicalAddr(g_dsp_state);
    g_dsp_job.log = PhysicalAddr(g_dlog.ents);
    g_dsp_job.nlog = g_dlog.count;
    g_dsp_job.nsamp = steps * SAMPLES_PER_STEP;
    g_dsp_job.out = PhysicalAddr(g_dsp_out);
    data_cache_hit_writeback(&g_dsp_job, sizeof(g_dsp_job));
    dsp_pending_samples = steps * SAMPLES_PER_STEP;
    dsp_buf ^= 1;                  // the CPU logs the next frame into the other buffer
    g_dlog.ents = g_dsp_log_ents[dsp_buf];
    g_dlog.count = 0;
#endif
    if (frame->numM7Jobs) {        // mode 7 rows first (see g_ppur_m7_busy)
      g_m7_job.map = PhysicalAddr(g_m7map);
      g_m7_job.pix = PhysicalAddr(g_m7pix);
      g_m7_job.out = PhysicalAddr(frame->m7rows);
      g_m7_job.list = PhysicalAddr(frame->m7job);
      g_m7_job.n = frame->numM7Jobs;
      data_cache_hit_writeback(&g_m7_job, sizeof(g_m7_job));
      g_ppur_m7_busy = true;
      g_rsp_stage = 4;
      g_rsp_t1 = get_ticks_us();
      rsp_load(&rsp_m7);
      rsp_load_data(&g_m7_job, sizeof(g_m7_job), 0);
      rsp_run_async();
    } else {
      StartDsp();
    }
#if LOWPOLY
    // The models go on top of the finished picture while the RSP already works
    // on this frame (it never copies the rows they cover).
    if (show_fb) LowPoly_Draw(cur ^ 1, show_fb);   // shows it once the RDP is done
#endif
    pending_fb = fb;
    cur ^= 1;

    uint64_t now2 = get_ticks_us();
    if (now2 - t_window_start >= 1000000) {
      unsigned logic_us = n_logic ? (unsigned)(t_logic / n_logic) : 0;
      unsigned draw_total_us = n_draw ? (unsigned)(t_draw / n_draw) : 0;
      unsigned draw_us = draw_total_us > logic_us && n_logic ? draw_total_us - logic_us : draw_total_us;
      unsigned audio_us = (n_logic + n_draw) ? (unsigned)(t_audio / (n_logic + n_draw)) : 0;
      unsigned wait_us = n_shown ? (unsigned)(t_wait / n_shown) : 0;
      unsigned secs_x10 = (unsigned)((now2 - t_window_start) / 100000);
      unsigned fps = n_shown * 10 / (secs_x10 ? secs_x10 : 1);
      unsigned sps = (n_logic + n_draw) * 10 / (secs_x10 ? secs_x10 : 1);
      unsigned rs_us = n_shown ? (unsigned)(g_rsp_tsound / n_shown) : 0, rp_us = n_shown ? (unsigned)(g_rsp_tpic / n_shown) : 0;
      g_rsp_tsound = g_rsp_tpic = 0;
      snprintf(perf_text, sizeof(perf_text), "FPS%u GAME%u L%u.%u C%u.%u W%u.%u S%u.%u P%u.%u",
               fps, sps, logic_us / 1000, logic_us / 100 % 10, draw_us / 1000, draw_us / 100 % 10,
               wait_us / 1000, wait_us / 100 % 10, rs_us / 1000, rs_us / 100 % 10, rp_us / 1000, rp_us / 100 % 10);
      {
        extern uint64_t g_t_game_us, g_t_lines_us;
        unsigned st = n_logic + n_draw ? n_logic + n_draw : 1;
#ifdef CAPSTAT
        { extern long g_cs[8]; unsigned d = n_shown ? n_shown : 1;
          debugf("CS calls%ld srch%ld inval%ld chk%ld fill%ld pages%ld lines%ld sliv%ld ", g_cs[0]/d, g_cs[1]/d, g_cs[2]/d, g_cs[3]/d, g_cs[4]/d, g_cs[5]/d, g_cs[6]/d, g_cs[7]/d);
          extern unsigned long g_ct[10];
          debugf("CT tile%lu spr%lu cap%lu hdma%lu sig%lu (us/frame) ", g_ct[0]/47/d, g_ct[1]/47/d, g_ct[2]/47/d, g_ct[3]/47/d, g_ct[4]/47/d);
          debugf("TB search%lu first%lu todo%lu fill%lu ", g_ct[5]/47/d, g_ct[6]/47/d, g_ct[7]/47/d, g_ct[8]/47/d);
          memset(g_ct, 0, sizeof(g_ct));
          memset(g_cs, 0, sizeof(g_cs)); }
#endif
        debugf("MK%u FX%u ", n_shown ? (unsigned)(g_t_mark / n_shown) : 0, n_shown ? (unsigned)(g_rsp_tfx / n_shown) : 0); g_t_mark = 0; g_rsp_tfx = 0;
#if LOWPOLY
        debugf("LP%u T%u ", n_shown ? (unsigned)(g_lp_us / n_shown) : 0, n_shown ? (unsigned)(g_lp_ntri / n_shown) : 0); g_lp_us = 0; g_lp_ntri = 0;
#ifdef LPPROF
        debugf("LPPROF xf%u faces%u tri%u sub%u ", (unsigned)(g_lp_tt[0] / 47 / (n_shown ? n_shown : 1)), (unsigned)(g_lp_tt[1] / 47 / (n_shown ? n_shown : 1)),
               (unsigned)(g_lp_tt[2] / 47 / (n_shown ? n_shown : 1)), (unsigned)(g_lp_tt[3] / 47 / (n_shown ? n_shown : 1)));
        memset(g_lp_tt, 0, sizeof(g_lp_tt));
#endif
#endif
        debugf("%s A%u.%u G%u LN%u CP%u%% (ms: logic, capture per step; W CPU waits for RSP, S/P RSP sound/picture per frame; A audio; G/LN us per step game code / scanline loop; CP lines copied from the previous frame)\n",
               perf_text, audio_us / 1000, audio_us / 100 % 10, (unsigned)(g_t_game_us / st), (unsigned)(g_t_lines_us / st),
               g_lines_total ? g_lines_copied * 100 / g_lines_total : 0);
        g_lines_copied = g_lines_total = 0;
        g_t_game_us = g_t_lines_us = 0;
      }
      t_logic = t_draw = t_audio = t_wait = 0; n_logic = n_draw = n_shown = 0; t_window_start = now2;
    }

#if ENABLE_AUDIO
    AudioPump();
#endif
  }
}
