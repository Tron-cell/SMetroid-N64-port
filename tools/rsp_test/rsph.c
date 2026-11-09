#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsph.h"
#include "rsp.h"
typedef unsigned int (*DoRspCycles_t)(unsigned int);
extern void InitiateRSP(RSP_INFO info, uint32_t *cycles);
extern unsigned int DoRspCycles(unsigned int);

uint8_t *rsph_rdram, *rsph_dmem, *rsph_imem;
static uint8_t *spmem;
static uint32_t mi_intr, sp_mem_addr, sp_dram_addr, sp_rd_len, sp_wr_len, sp_status, sp_dma_full, sp_dma_busy, sp_pc, sp_sema;
static uint32_t dpc_start, dpc_end, dpc_current, dpc_status, dpc_clock, dpc_bufbusy, dpc_pipebusy, dpc_tmem;
static void nop(void) {}

// Host is little-endian; cxd4 stores 32-bit words in host order: byte a is at a ^ 3.
void rsph_write(uint8_t *mem, uint32_t addr, const void *be, size_t n) {
  const uint8_t *s = be;
  for (size_t i = 0; i < n; i++) mem[(addr + i) ^ 3] = s[i];
}
void rsph_read(const uint8_t *mem, uint32_t addr, void *be, size_t n) {
  uint8_t *d = be;
  for (size_t i = 0; i < n; i++) d[i] = mem[(addr + i) ^ 3];
}
void rsph_w32(uint8_t *mem, uint32_t addr, uint32_t v) { uint8_t b[4] = { v >> 24, v >> 16, v >> 8, v }; rsph_write(mem, addr, b, 4); }
uint32_t rsph_r32(const uint8_t *mem, uint32_t addr) { uint8_t b[4]; rsph_read(mem, addr, b, 4); return b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }

void rsph_init(void) {
  rsph_rdram = aligned_alloc(64, RSPH_RDRAM_SIZE); memset(rsph_rdram, 0, RSPH_RDRAM_SIZE);
  spmem = aligned_alloc(64, 0x2000); memset(spmem, 0, 0x2000);
  rsph_dmem = spmem; rsph_imem = spmem + 0x1000;
  RSP_INFO info; memset(&info, 0, sizeof(info));
  info.RDRAM = rsph_rdram; info.DMEM = rsph_dmem; info.IMEM = rsph_imem;
  info.MI_INTR_REG = &mi_intr;
  info.SP_MEM_ADDR_REG = &sp_mem_addr; info.SP_DRAM_ADDR_REG = &sp_dram_addr;
  info.SP_RD_LEN_REG = &sp_rd_len; info.SP_WR_LEN_REG = &sp_wr_len;
  info.SP_STATUS_REG = &sp_status; info.SP_DMA_FULL_REG = &sp_dma_full; info.SP_DMA_BUSY_REG = &sp_dma_busy;
  info.SP_PC_REG = &sp_pc; info.SP_SEMAPHORE_REG = &sp_sema;
  info.DPC_START_REG = &dpc_start; info.DPC_END_REG = &dpc_end; info.DPC_CURRENT_REG = &dpc_current;
  info.DPC_STATUS_REG = &dpc_status; info.DPC_CLOCK_REG = &dpc_clock; info.DPC_BUFBUSY_REG = &dpc_bufbusy;
  info.DPC_PIPEBUSY_REG = &dpc_pipebusy; info.DPC_TMEM_REG = &dpc_tmem;
  info.CheckInterrupts = nop; info.ProcessDList = nop; info.ProcessAList = nop; info.ProcessRdpList = nop; info.ShowCFB = nop;
  sp_status = 1; // halted
  InitiateRSP(info, NULL);
}

static uint8_t *slurp(const char *fn, size_t *n) {
  FILE *f = fopen(fn, "rb"); if (!f) { perror(fn); exit(1); }
  fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t *b = malloc(*n + 1); if (fread(b, 1, *n, f) != *n) exit(1); fclose(f); return b;
}
void rsph_load_file(const char *text_bin, const char *data_bin) {
  size_t n; uint8_t *b = slurp(text_bin, &n);
  if (n > 0x1000) { fprintf(stderr, "ucode text too big: %zu\n", n); exit(1); }
  memset(rsph_imem, 0, 0x1000); rsph_write(rsph_imem, 0, b, n); free(b);
  b = slurp(data_bin, &n);
  memset(rsph_dmem, 0, 0x1000); rsph_write(rsph_dmem, 0, b, n); free(b);
}
int rsph_run(void) {
  sp_pc = 0; sp_status = 0;
  for (int i = 0; i < 100000 && !(sp_status & 1); i++) DoRspCycles(0x7fffffff);
  if (!(sp_status & 1)) { fprintf(stderr, "RSP did not halt\n"); return 1; }
  return 0;
}
