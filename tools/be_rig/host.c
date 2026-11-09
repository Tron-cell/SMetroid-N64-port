// Runs a big-endian MIPS ELF (built with the N64 toolchain) under Unicorn.
#include <unicorn/unicorn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <elf.h>
#define MEM_BASE 0x80000000ull
#define MEM_SIZE (16u << 20)
static uc_engine *uc;
static uint8_t *mem;
static int exit_code = -1;
static const char *rom_path;
static FILE *dump_f;
static long n_unaligned;
static uint64_t last_block[8]; static int lb_pos;
static uint64_t g_instr, g_instr_mark;
static int g_prof, g_prof_on = 1; static uint64_t *g_prof_hist;   // instructions per 4-byte code word, guest code < 4 MB
static void hook_block(uc_engine *uc, uint64_t addr, uint32_t size, void *ud) {
  last_block[lb_pos++ & 7] = addr; g_instr += size >> 2;
  if (g_prof && g_prof_on) { uint32_t o = ((uint32_t)addr - 0x80000000u) >> 2; if (o < (4u << 20) / 4) g_prof_hist[o] += size >> 2; }
}

static void hook_mem(uc_engine *uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void *ud) {
  if (size > 1 && (addr & (size - 1))) {
    uint64_t pc; uc_reg_read(uc, UC_MIPS_REG_PC, &pc);
    printf("\n*** UNALIGNED %s size %d addr=%08x pc=%08x\n", type == UC_MEM_WRITE ? "write" : "read", size, (uint32_t)addr, (uint32_t)pc);
    if (++n_unaligned >= 1) { exit_code = 97; uc_emu_stop(uc); }
  }
}
static uint32_t be32(const void *p) { const uint8_t *b = p; return b[0]<<24|b[1]<<16|b[2]<<8|b[3]; }
static uint16_t be16(const void *p) { const uint8_t *b = p; return b[0]<<8|b[1]; }
static uint8_t *G(uint64_t va) { return mem + (uint32_t)(va - MEM_BASE); }
static void hook_intr(uc_engine *uc, uint32_t intno, void *ud) {
  uint64_t v0, a1, a2, a3, pc;
  uc_reg_read(uc, UC_MIPS_REG_V0, &v0); uc_reg_read(uc, UC_MIPS_REG_A1, &a1);
  uc_reg_read(uc, UC_MIPS_REG_A2, &a2); uc_reg_read(uc, UC_MIPS_REG_A3, &a3);
  uc_reg_read(uc, UC_MIPS_REG_PC, &pc);
  if (getenv("DBG")) fprintf(stderr, "intr %u v0=%lld a1=%llx\n", intno, (long long)v0, (unsigned long long)a1);
  if (intno != 17) {  // not EXCP_SYSCALL: a CPU exception (address error, etc.)
    uint64_t ra, sp, epc, bad, st; uc_reg_read(uc, UC_MIPS_REG_RA, &ra); uc_reg_read(uc, UC_MIPS_REG_SP, &sp); uc_reg_read(uc, UC_MIPS_REG_CP0_EPC, &epc); uc_reg_read(uc, UC_MIPS_REG_CP0_BADVADDR, &bad); uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &st);
    printf("\n*** CPU EXCEPTION %u at pc=%08x epc=%08x badvaddr=%016llx status=%08x ra=%016llx sp=%016llx\n", intno, (uint32_t)pc, (uint32_t)epc, (unsigned long long)bad, (uint32_t)st, (unsigned long long)ra, (unsigned long long)sp);
    printf("recent blocks:"); for (int i = 1; i <= 8; i++) printf(" %08x", (uint32_t)last_block[(lb_pos - i) & 7]); printf("\n");
    exit_code = 96; uc_emu_stop(uc); return;
  }
  int64_t ret = 0;
  switch ((int)v0) {
  case 1: { // load rom: a1=dst, a2=capacity -> returns size
    FILE *f = fopen(rom_path, "rb"); size_t n = fread(G(a1), 1, a2, f); fclose(f); ret = n; break; }
  case 2: fwrite(G(a1), 1, a2, stdout); fflush(stdout); ret = a2; break;
  case 3: exit_code = (int)a1; uc_emu_stop(uc); break;
  case 6: g_prof_on = (int)a1; break;
  case 5: ret = (int64_t)(g_instr - g_instr_mark); g_instr_mark = g_instr; break;  // instructions since last mark
  case 4: // dump block: a1=ptr a2=len
    if (dump_f) fwrite(G(a1), 1, a2, dump_f); break;
  default: fprintf(stderr, "bad hostcall %d\n", (int)v0); uc_emu_stop(uc);
  }
  uc_reg_write(uc, UC_MIPS_REG_V0, &ret);

}
int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: host prog.elf [rom] [dumpfile]\n"); return 2; }
  rom_path = argc > 2 ? argv[2] : "";
  g_prof = getenv("PROF") != NULL; if (getenv("PROF_FROM_START") == NULL) g_prof_on = 0; if (g_prof) g_prof_hist = calloc((4u << 20) / 4, 8);
  if (argc > 3) dump_f = fopen(argv[3], "wb");
  FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t *elf = malloc(sz); fread(elf, 1, sz, f); fclose(f);
  mem = calloc(1, MEM_SIZE);
  // ELF32 (o64 uses ELF32 container)
  uint32_t phoff = be32(elf + 28); int phnum = be16(elf + 44), phentsz = be16(elf + 42);
  for (int i = 0; i < phnum; i++) {
    uint8_t *ph = elf + phoff + i * phentsz;
    if (be32(ph) != PT_LOAD) continue;
    uint32_t off = be32(ph + 4), va = be32(ph + 8), fsz = be32(ph + 16);
    memcpy(G((int32_t)va), elf + off, fsz);
  }
  uint64_t entry = (int64_t)(int32_t)be32(elf + 24);
  if (uc_open(UC_ARCH_MIPS, UC_MODE_MIPS64 | UC_MODE_BIG_ENDIAN, &uc)) { puts("uc_open failed"); return 1; }
  uc_ctl_set_cpu_model(uc, UC_CPU_MIPS64_R4000);
  uc_err e = uc_mem_map_ptr(uc, 0, MEM_SIZE, UC_PROT_ALL, mem);
  if (e) { printf("map: %s\n", uc_strerror(e)); return 1; }
  uc_hook h; uc_hook_add(uc, &h, UC_HOOK_INTR, hook_intr, NULL, 1, 0);
  { uc_hook hb; uc_hook_add(uc, &hb, UC_HOOK_BLOCK, hook_block, NULL, 1, 0); }
  if (getenv("ALIGN_HOOK")) { uc_hook h2; uc_hook_add(uc, &h2, UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, hook_mem, NULL, 1, 0); }
  // enable FPU (CU1) + 64-bit FPRs (FR) like libdragon does
  uint64_t sr; uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &sr); sr |= (1u << 29) | (1u << 26); uc_reg_write(uc, UC_MIPS_REG_CP0_STATUS, &sr);
  uint64_t sentinel = 0xffffffff80000000ull;
  e = uc_emu_start(uc, (uint64_t)(int64_t)(int32_t)entry, sentinel, 0, 0);
  if (exit_code < 0) {
    uint64_t pc, bad; uc_reg_read(uc, UC_MIPS_REG_PC, &pc);
    bad = 0;
    printf("\n*** STOP: %s at pc=%08llx badvaddr=%08llx\n", uc_strerror(e), (unsigned long long)(uint32_t)pc, (unsigned long long)(uint32_t)bad);
    return 99;
  }
  if (dump_f) fclose(dump_f);
  if (g_prof) { FILE *pf = fopen("prof.bin", "wb"); fwrite(g_prof_hist, 8, (4u << 20) / 4, pf); fclose(pf); }
  return exit_code;
}
