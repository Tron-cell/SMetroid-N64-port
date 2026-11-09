// RSP cycle model fed by the cxd4 instruction trace. It follows ares' RSP
// interpreter pipeline (rsp.hpp Pipeline / canDualIssue), which is the most
// accurate model available:
//  * an SU and a VU instruction in adjacent slots issue together, unless the
//    second reads or writes a vector register the first writes; never after a
//    branch (the delay slot issues alone), and a taken branch to an address
//    with bit 2 set issues its first instruction alone
//  * each issue group checks hazards against the last three groups:
//    reading a GPR written by a non-bypassed instruction (loads, mfc0/mfc2/
//    cfc2) one group earlier: 2 stalls, two groups earlier: 1 stall;
//    reading a vector register written 1/2/3 groups earlier: 3/2/1 stalls;
//    a store while the group two back was a load: stall (repeatedly).
//    mfc0/mtc0/mfc2/mtc2/cfc2/ctc2 count as both load and store.
//  * after a branch delay slot: 1 stall
// DMA is modelled as 24 cycles + 1 cycle per 4 bytes, one at a time; polling
// DMA_BUSY / DMA_FULL waits for it.
#include <stdint.h>
#include <string.h>

typedef unsigned long long u64;
typedef struct { int load, store, branch, vector, bypass; uint32_t ruse, rdef, vuse, vdef; } Op;
typedef struct { int load; uint32_t rw, vw; } Stage;

static u64 t;                       // cycles
static Stage prev[3];
static int single_issue, after_branch_group;
static int have_pend; static Op pend; static unsigned pend_pc;
static u64 dma_free, dma_free_prev, pend_wait;
u64 g_rspcyc_cycles;
u64 g_rspcyc_pc[1024];

void rspcyc_reset(void) {
  t = 0; memset(prev, 0, sizeof prev); single_issue = after_branch_group = 0; have_pend = 0;
  dma_free = dma_free_prev = 0; pend_wait = 0; g_rspcyc_cycles = 0;
}

void rspcyc_dma(unsigned bytes, int write) {
  (void)write;
  u64 start = t > dma_free ? t : dma_free;
  dma_free_prev = dma_free;
  dma_free = start + 24 + (bytes + 3) / 4;
}

static Op decode(unsigned in, u64 *wait) {
  Op o = {0};
  unsigned op = in >> 26, rs = (in >> 21) & 31, rt = (in >> 16) & 31, rd = (in >> 11) & 31, fn = in & 63;
  #define RU(r) (o.ruse |= 1u << (r))
  #define RD(r) (o.rdef |= (r) ? 1u << (r) : 0)
  switch (op) {
  case 0:
    if (fn == 8) { o.branch = 1; RU(rs); }
    else if (fn == 9) { o.branch = 1; RU(rs); RD(rd); o.bypass = 1; }
    else if (fn == 13) {}
    else if (fn < 4) { RU(rt); RD(rd); o.bypass = 1; }
    else { RU(rs); RU(rt); RD(rd); o.bypass = 1; }
    break;
  case 1: o.branch = 1; RU(rs); break;
  case 2: o.branch = 1; break;
  case 3: o.branch = 1; RD(31); o.bypass = 1; break;
  case 4: case 5: o.branch = 1; RU(rs); RU(rt); break;
  case 6: case 7: o.branch = 1; RU(rs); break;
  case 15: RD(rt); o.bypass = 1; break;
  case 16:                          // COP0
    o.load = o.store = 1;
    if (rs == 0) { RD(rt); if (rd == 5) *wait = dma_free_prev; else if (rd == 6) *wait = dma_free; }
    else RU(rt);
    break;
  case 18:                          // COP2
    if (in & (1u << 25)) {          // VU
      o.vector = 1; o.vdef = 1u << ((in >> 6) & 31); o.vuse = (1u << rd) | (1u << rt);
    } else {
      o.load = o.store = 1;
      if (rs == 0) { RD(rt); o.vuse = 1u << rd; }
      else if (rs == 2) RD(rt);
      else if (rs == 4) { RU(rt); o.vdef = 1u << rd; }
      else if (rs == 6) RU(rt);
    }
    break;
  case 32: case 33: case 35: case 36: case 37: case 39: RU(rs); RD(rt); o.load = 1; break;
  case 40: case 41: case 43: RU(rs); RU(rt); o.store = 1; break;
  case 50: RU(rs); o.vdef = 1u << rt; o.load = 1; break;   // LWC2
  case 58: RU(rs); o.vuse = 1u << rt; o.store = 1; break;  // SWC2
  default: RU(rs); RD(rt); o.bypass = 1; break;             // immediates
  }
  return o;
}

static void stall(void) { prev[2] = prev[1]; prev[1] = prev[0]; memset(&prev[0], 0, sizeof prev[0]); t++; }

// Issue a group of one or two instructions.
static void issue(const Op *a, const Op *b, unsigned pc, u64 wait) {
  u64 t0 = t;
  uint32_t rr = a->ruse | (b ? b->ruse : 0), vr = a->vuse | (b ? b->vuse : 0);
  uint32_t rw = (a->bypass ? 0 : a->rdef) | (b && !b->bypass ? b->rdef : 0), vw = a->vdef | (b ? b->vdef : 0);
  int load = a->load || (b && b->load), store = a->store || (b && b->store), branch = a->branch || (b && b->branch);
  while (t < wait) stall();         // DMA register poll
  if (rr & prev[0].rw) { stall(); stall(); } else if (rr & prev[1].rw) stall();
  if (vr & prev[0].vw) { stall(); stall(); stall(); } else if (vr & prev[1].vw) { stall(); stall(); } else if (vr & prev[2].vw) stall();
  if (store) while (prev[1].load) stall();
  prev[2] = prev[1]; prev[1] = prev[0]; prev[0].load = load; prev[0].rw = rw; prev[0].vw = vw;
  t++;
  if (after_branch_group) { stall(); after_branch_group = 0; }   // after the delay slot
  if (branch) after_branch_group = 1;
  single_issue = branch;
  g_rspcyc_pc[(pc >> 2) & 1023] += t - t0;
  g_rspcyc_cycles = t > dma_free ? t : dma_free;
}

void rspcyc_inst(unsigned pc, unsigned in) {
  u64 wait = 0;
  Op o = decode(in, &wait);
  if (have_pend) {
    int can = pc == pend_pc + 4 && !single_issue && !pend.branch && pend.vector != o.vector &&
              !(pend.vdef & (o.vuse | o.vdef)) && pend_wait == 0 && wait == 0;
    if (can) { issue(&pend, &o, pend_pc, 0); have_pend = 0; return; }
    issue(&pend, NULL, pend_pc, pend_wait);
    // a taken branch to an address with bit 2 set: its first instruction issues alone
    if (pc != pend_pc + 4 && (pc & 4)) single_issue = 1;
  }
  pend = o; pend_pc = pc; pend_wait = wait; have_pend = 1;
}
