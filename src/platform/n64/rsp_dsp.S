//////////////////////////////////////////////////////////////////////////////
// rsp_dsp.S - SNES S-DSP (sound) for the N64 RSP.
//
// The CPU runs the SPC sound driver (spc_player.c) and records every DSP
// register write with the sample number it happened at (snes/dsp_rsp.h).
// This microcode replays those writes and generates the 32 kHz stereo output
// like dsp_cycle() in snes/dsp.c (with MY_CHANGES): BRR decoding, Gaussian
// interpolation, ADSR/GAIN envelopes, echo with FIR, and the 16-bit wrap and
// clamp quirks. Not supported: pitch modulation (PMON), which Super Metroid
// never uses.
//
// The 8 voices run in the 8 vector lanes. Rare events (BRR block decode,
// envelope steps, register writes) are scalar code.
//
// Persistent state lives in RDRAM (JOB_STATE) and is loaded and saved each
// run, because the PPU microcode uses the RSP between runs.
//
// Arithmetic notes:
//  * vmudm a, b (b unsigned) = (a * b) >> 16, so (x * g) >> 10 = vmudm(2x, 32g)
//  * (x * v) >> k for a signed byte v: split v << (16-k) = hi * 65536 + lo
//    (lo unsigned) and do vmudm(x, lo) + vmadh(x, hi); the result is clamped
//    to 16 bits, like the C code's clamps.
//  * vaddc adds with 16-bit wrap, vadd saturates (and adds VCO as carry in,
//    so VCO is cleared before each vadd that follows a vaddc).
//////////////////////////////////////////////////////////////////////////////
#include <rsp.inc>

    .set noat

#define CHUNK_MAX 32

    .data
    .align 4
// ---- Job, written by the CPU before each run (32 bytes) ----
JOB_ARAM:   .long 0     // RDRAM address of the 64 KB SPC RAM (8-byte aligned, 16 bytes readable past the end)
JOB_STATE:  .long 0     // RDRAM address of the persistent state (DSPR_STATE_SIZE bytes, zeroed at start)
JOB_LOG:    .long 0     // RDRAM address of the write log (4 bytes per entry, 8-byte aligned)
JOB_NLOG:   .half 0     // number of log entries
JOB_NSAMP:  .half 0     // samples to generate
JOB_OUT:    .long 0     // RDRAM address of output (int16 L, R per sample; 8 bytes slack)
            .long 0, 0, 0

    .align 4
// e0 32, e1 0xfffe, e2 0x8000, e3 0x07f8, e4 GAUSS (set at start), e5 0x7fff, e6 1, e7 0
// (V_K30 at DMEM 0x20 and V_M7 at 0x30: the sample loop loads V_M7 from 0x30 directly)
V_K30:      .half 32, 0xfffe, 0x8000, 0x07f8, 0, 0x7fff, 1, 0
V_M7:       .half 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0

WTAB:       .long W_VolL, W_VolR, W_PitchL, W_PitchH, W_Srcn, W_Adsr1, W_Adsr2, W_Gain
            .long W_Ret, W_Ret, W_Ret, W_Ret, W_GlobC, W_GlobD, W_Ret, W_Fir
V_L48:      .half 0, 48, 96, 144, 192, 240, 288, 336
DECFTAB:    .long DecF0, DecF1, DecF2, DecF3   // BRR filter code (DecSample)

RATES:      .half 0, 2048, 1536, 1280, 1024, 768, 640, 512
            .half 384, 320, 256, 192, 160, 128, 96, 80
            .half 64, 48, 40, 32, 24, 20, 16, 12
            .half 10, 8, 6, 5, 4, 3, 2, 1

// Gaussian table, reordered: entry o = { g[0xff-o], g[0x1ff-o], g[0x100+o], g[o] } * 32
    .align 3
GAUSS:
#include "rsp_dsp_gauss.inc"

// ---- Persistent state (DMA'd from/to JOB_STATE) ----
    .align 4
STATE:
S_PC:       .space 16   // pitch counter
S_PITCH:    .space 16
S_GAIN:     .space 16   // envelope 0..0x7ff
S_RC:       .space 16   // envelope rate counter
S_INC:      .space 16   // 1 if the rate counter runs
S_RATEEFF:  .space 16   // rate threshold, 0x7fff if never
S_RELDEC:   .space 16   // 8 in release, else 0
S_KEEP:     .space 16   // 0 for direct gain, else 0xffff
S_GVAL:     .space 16   // direct gain value or 0
S_VLL:      .space 16   // voice volume L: (v & 63) << 10
S_VLH:      .space 16   //                 v >> 6
S_VRL:      .space 16
S_VRH:      .space 16
S_ECHOM:    .space 16   // 0xffff if echo enabled
// lane 0 = left, lane 1 = right
S_MVL:      .space 16   // master volume (v & 127) << 9
S_MVH:      .space 16   //               v >> 7
S_EVL:      .space 16   // echo volume
S_EVH:      .space 16
S_FBL:      .space 16   // echo feedback (lanes 0 and 1)
S_FBH:      .space 16
S_FIRL:     .space 16   // FIR coefficient, lane i: (c & 63) << 10
S_FIRH:     .space 16   //                          c >> 6
S_RINGL:    .space 32   // FIR history, 8 entries stored twice (ring[k] == ring[k+8])
S_RINGR:    .space 32
// per voice, 32 bytes each
#define C_STATE   0     // u8: 0 attack, 1 decay, 2 sustain, 3 gain, 4 release
#define C_USEGAIN 1
#define C_GMODE   2
#define C_DIRECT  3
#define C_SRCN    4
#define C_PFLAGS  5
#define C_RATES   8     // u16[4]
#define C_SUSTAIN 16
#define C_GVALUE  18
#define C_OFFSET  20    // BRR decode offset
#define C_OLD     22
#define C_OLDER   24
#define C_PFADDR  26    // ARAM address held in the BRR prefetch buffer
#define C_PFVALID 28
S_CH:       .space 8*32
// decoded samples * 2, 19 per voice (16 + 3 kept for interpolation), 48 bytes per voice
DECBUF:     .space 8*48
// globals
#define G_DIRPAGE  0
#define G_ESA      2
#define G_EDL      4
#define G_EREMAIN  6
#define G_EIDX     8
#define G_NOISE    10
#define G_NRATE    12
#define G_NCOUNT   14
#define G_NONMASK  16
#define G_MUTE     17
#define G_RESET    18
#define G_ECHOWR   19
#define G_FIRIDX   20
S_GLOB:     .space 32
STATE_END:
#define STATE_SIZE (STATE_END - STATE)
#define SO(x) ((x) - STATE)
#define GO(x) (SO(S_GLOB) + (x))

// ---- Work buffers (not saved) ----
    .align 4
BRRBUF:     .space 8*16     // BRR block prefetch, 16 bytes per voice
PFVALID:    .space 16       // byte per voice: prefetch buffer valid
SCR:        .space 96       // +0 sample addresses, +16 gauss addresses, +32 gathered samples,
                            // +48 ARAM read (16), +64 echo RDRAM address, +68 echo length
OUTBUF:     .space CHUNK_MAX*4
ECHOBUF:    .space CHUNK_MAX*4+16
LOGBUF:     .space 32*4

// ---- registers ----
#define vzero  $v00
#define vPC    $v16
#define vPITCH $v17
#define vGAIN  $v18
#define vRC    $v19
#define vINC   $v20
#define vRATE  $v21
#define vREL   $v22
#define vKEEP  $v23
#define vGVAL  $v24
#define vVLL   $v25
#define vVLH   $v26
#define vVRL   $v27
#define vVRH   $v28
#define vECHOM $v29
#define vK     $v30
#define vBUF   $v31
// scalar: s0 sample, s1 nsamp, s2/s3 log buffer read/end, s4 chunk samples left,
// s5 OUTBUF write pointer, s6 echo entry, s7 output RDRAM address,
// fp = STATE, sp = SCR, gp = FIR index, t9 = log entries not yet fetched,
// t8 = log RDRAM pointer. Subroutines keep these.

    .text
    .globl _start
_start:
    vxor vzero, vzero, vzero
    li t0, %lo(V_K30)
    lqv vK, 0, t0
    li t0, %lo(GAUSS)
    mtc2 t0, vK.e4
    li t0, %lo(DECBUF)
    mtc2 t0, $v01.e0
    li t0, %lo(V_L48)
    lqv vBUF, 0, t0
    vaddc vBUF, vBUF, $v01.e0
    ctc2 zero, COP2_CTRL_VCO
    li fp, %lo(STATE)
    li sp, %lo(SCR)
    // load state
    lw t0, %lo(JOB_STATE)(zero)
    move t1, fp
    jal DmaIn
    li t2, STATE_SIZE-1
    jal DmaWait
    nop
    // BRR prefetch buffers don't survive between runs
    li t0, %lo(PFVALID)
    sqv vzero, 0, t0
    lhu s1, %lo(JOB_NSAMP)(zero)
    move s0, zero
    lw s7, %lo(JOB_OUT)(zero)
    li s5, %lo(OUTBUF)
    lw t8, %lo(JOB_LOG)(zero)
    lhu t9, %lo(JOB_NLOG)(zero)
    li s2, %lo(LOGBUF)
    move s3, s2
    lbu gp, GO(G_FIRIDX)(fp)

ChunkLoop:
    // ---- apply log entries due at this sample ----
ApplyLoop:
    jal PeekLog                     // v0 = sample of next entry (0xffff if none)
    nop
    bne v0, s0, ApplyDone
    lbu a0, 2(s2)
    lbu a1, 3(s2)
    jal ApplyWrite
    addiu s2, 4
    j ApplyLoop
    nop
ApplyDone:
    beq s0, s1, Finish
    nop
    // ---- chunk length: up to the next write, the echo wrap, CHUNK_MAX, the end ----
    subu s4, s1, s0
    subu t0, v0, s0
    sltu t1, t0, s4
    beqz t1, 1f
    nop
    move s4, t0
1:  li t0, CHUNK_MAX
    sltu t1, t0, s4
    beqz t1, 1f
    nop
    move s4, t0
1:  lhu t0, GO(G_EREMAIN)(fp)
    sltu t1, t0, s4
    beqz t1, 1f
    nop
    move s4, t0
1:  // echo address (assumes the echo buffer does not wrap past 0xffff)
    lhu t0, GO(G_EIDX)(fp)
    lhu t1, GO(G_ESA)(fp)
    sll t0, 2
    addu t0, t1
    andi a0, t0, 0xffff
    // echo entries in: covering 8-byte aligned range
    move t0, a0
    andi t3, t0, 7
    li s6, %lo(ECHOBUF)
    addu s6, t3                     // first entry
    subu t0, t3
    lw t1, %lo(JOB_ARAM)(zero)
    addu t0, t1
    sll t2, s4, 2
    addu t2, t3
    addiu t2, 7
    li t1, ~7
    and t2, t1
    sw t0, 64(sp)                   // for the write back
    sw t2, 68(sp)
    addiu t2, -1
    jal DmaIn
    li t1, %lo(ECHOBUF)
    jal DmaWait
    nop
    // live vectors
    lqv vPC,    SO(S_PC),      fp
    lqv vPITCH, SO(S_PITCH),   fp
    lqv vGAIN,  SO(S_GAIN),    fp
    lqv vRC,    SO(S_RC),      fp
    lqv vINC,   SO(S_INC),     fp
    lqv vRATE,  SO(S_RATEEFF), fp
    lqv vREL,   SO(S_RELDEC),  fp
    lqv vKEEP,  SO(S_KEEP),    fp
    lqv vGVAL,  SO(S_GVAL),    fp
    lqv vVLL,   SO(S_VLL),     fp
    lqv vVLH,   SO(S_VLH),     fp
    lqv vVRL,   SO(S_VRL),     fp
    lqv vVRH,   SO(S_VRH),     fp
    lqv vECHOM, SO(S_ECHOM),   fp
    addu s0, s4
    // echo index bookkeeping for after this chunk
    lhu t0, GO(G_EIDX)(fp)
    lhu t1, GO(G_EREMAIN)(fp)
    addu t0, s4
    subu t1, s4
    bnez t1, 1f
    nop
    lhu t1, GO(G_EDL)(fp)
    move t0, zero
1:  sh t0, GO(G_EIDX)(fp)
    sh t1, GO(G_EREMAIN)(fp)

SampleLoop:
    // ---- 1. pitch counters; a carry means the next BRR block is due ----
    vaddc vPC, vPC, vPITCH
    cfc2 t0, COP2_CTRL_VCO
    andi t0, 0xff
    beqz t0, AfterDecodes
    ctc2 zero, COP2_CTRL_VCO
    li t1, %lo(DecodeBrr)
    jal DoSlow
    sw t1, 72(sp)
AfterDecodes:
    // The work of one sample, scheduled so the scalar unit (echo entry, FIR
    // ring, sample gathers, stores) and the vector unit run side by side.
    // P1: interpolation addresses (VU) | echo entry into the FIR ring (SU).
    // Samples: buf + (pc >> 12) * 2; Gaussian table: GAUSS + ((pc >> 4) & 0xff) * 8.
    vmudl $v01, vPC, vK.e0
    lbu t0, 0(s6)
    vmudl $v02, vPC, vK.e2
    lb t1, 1(s6)
    lbu t2, 2(s6)
    lb t3, 3(s6)
    vand $v01, $v01, vK.e1
    sll t1, 8
    vand $v02, $v02, vK.e3
    sll t3, 8
    or t0, t1
    or t2, t3
    vadd $v01, $v01, vBUF
    sra t0, 1                       // (int16)L >> 1
    vadd $v02, $v02, vK.e4
    sra t2, 1                       // (int16)R >> 1
    sll t3, gp, 1
    addu t3, fp
    sqv $v01, 0, sp
    sh t0, SO(S_RINGL)(t3)
    sqv $v02, 16, sp
    sh t0, SO(S_RINGL)+16(t3)
    sh t2, SO(S_RINGR)(t3)
    sh t2, SO(S_RINGR)+16(t3)
    // P2: sample / Gaussian gathers (SU) | envelope and FIR (VU). The envelope
    // comes first: its saturating ops need VCO clear, the FIR sums set it.
    addiu t4, t3, SO(S_RINGL)+2     // oldest .. newest
    vadd vRC, vRC, vINC             // envelope rate counter
    addiu t5, t3, SO(S_RINGR)+2
    vsub vGAIN, vGAIN, vREL         // release: -8 ...
    lhu t0, 0(sp)
    lhu t1, 2(sp)
    lhu t2, 16(sp)
    lhu t3, 18(sp)
    ldv $v11.e0, 0, t4
    vge $v15, vRC, vRATE            // VCC = rate counter reached its rate
    cfc2 v0, COP2_CTRL_VCC
    ldv $v11.e4, 8, t4
    vge vGAIN, vGAIN, vzero         // ... not below 0
    ldv $v12.e0, 0, t5
    ldv $v12.e4, 8, t5
    lqv $v13, SO(S_FIRL), fp
    lqv $v14, SO(S_FIRH), fp
    vand vGAIN, vGAIN, vKEEP        // direct gain
    ldv $v03.e0, 0, t0
    ldv $v03.e4, 0, t1
    ldv $v04.e0, 0, t2
    vor vGAIN, vGAIN, vGVAL
    ldv $v04.e4, 0, t3
    vmudm $v15, $v11, $v13
    lhu t0, 4(sp)
    vmadh $v11, $v11, $v14          // (x * c) >> 6, L
    lhu t1, 6(sp)
    vmudm $v15, $v12, $v13
    lhu t2, 20(sp)
    vmadh $v12, $v12, $v14          // R
    lhu t3, 22(sp)
    vmudn $v02, vGAIN, vK.e0        // gain * 32 (again after the gain slow path)
    lqv $v15, 0x30, zero            // V_M7
    ldv $v05.e0, 0, t0
    ldv $v05.e4, 0, t1
    ldv $v06.e0, 0, t2
    vand $v13, $v11, $v15           // first 7 taps
    ldv $v06.e4, 0, t3
    vand $v14, $v12, $v15
    lhu t0, 8(sp)
    vaddc $v13, $v13, $v13.q1
    lhu t1, 10(sp)
    vaddc $v14, $v14, $v14.q1
    lhu t2, 24(sp)
    lhu t3, 26(sp)
    vaddc $v13, $v13, $v13.h2
    ldv $v07.e0, 0, t0
    vaddc $v14, $v14, $v14.h2
    ldv $v07.e4, 0, t1
    ldv $v08.e0, 0, t2
    vaddc $v13, $v13, $v13.e4       // wrapped sum of 7 taps in lane 0
    ldv $v08.e4, 0, t3
    vaddc $v14, $v14, $v14.e4
    lhu t0, 12(sp)
    ctc2 zero, COP2_CTRL_VCO
    lhu t1, 14(sp)
    vadd $v13, $v13, $v11.e7        // + last tap, saturating
    lhu t2, 28(sp)
    vadd $v14, $v14, $v12.e7
    lhu t3, 30(sp)
    li k0, 1                        // (k0: kept through the gain slow path)
    ctc2 k0, COP2_CTRL_VCC          // lane 0 = left, others = right
    ldv $v09.e0, 0, t0
    vmrg $v11, $v13, $v14.e0        // FIR sum: lane 0 L, others R
    ldv $v09.e4, 0, t1
    lqv $v13, SO(S_EVL), fp
    lqv $v14, SO(S_EVH), fp
    ldv $v10.e0, 0, t2
    ldv $v10.e4, 0, t3
    vmudm $v15, $v11, $v13
    lqv $v01, SO(S_FBL), fp
    vmadh $v12, $v11, $v14          // (sum * evol) >> 7
    lqv $v13, SO(S_FBH), fp
    vmudm $v03, $v03, $v04          // interpolation: 4 terms per voice, two voices per vector:
    vmudm $v05, $v05, $v06          // (2s * 32g) >> 16 = (s * g) >> 10
    vmudm $v15, $v11, $v01
    vmadh $v11, $v11, $v13          // (sum * efb) >> 7
    vmudm $v07, $v07, $v08
    vmudm $v09, $v09, $v10
    // P3: rest of the interpolation (first three terms wrap at 16 bits, the
    // fourth saturates)
    vaddc $v04, $v03, $v03.h1
    vaddc $v06, $v05, $v05.h1
    vaddc $v08, $v07, $v07.h1
    vaddc $v10, $v09, $v09.h1
    vaddc $v04, $v04, $v03.h2
    vaddc $v06, $v06, $v05.h2
    vaddc $v08, $v08, $v07.h2
    vaddc $v10, $v10, $v09.h2
    ctc2 zero, COP2_CTRL_VCO
    vadd $v04, $v04, $v03.h3
    andi v0, 0xff
    vadd $v06, $v06, $v05.h3
    lqv $v13, SO(S_MVL), fp
    vadd $v08, $v08, $v07.h3
    lqv $v14, SO(S_MVH), fp
    vadd $v10, $v10, $v09.h3
    vmudm $v04, $v04, vK.e2         // >> 1
    vmudm $v06, $v06, vK.e2
    vmudm $v08, $v08, vK.e2
    vmudm $v10, $v10, vK.e2
    ssv $v04.e0, 32, sp
    ssv $v04.e4, 34, sp
    ssv $v06.e0, 36, sp
    ssv $v06.e4, 38, sp
    ssv $v08.e0, 40, sp
    ssv $v08.e4, 42, sp
    ssv $v10.e0, 44, sp
    ssv $v10.e4, 46, sp
    lqv $v01, 32, sp                // voice samples
    beqz v0, AfterTriggers
    move t0, v0
    li t1, %lo(HandleGain)
    jal DoSlow
    sw t1, 72(sp)
    vmudn $v02, vGAIN, vK.e0        // gain * 32 (changed)
AfterTriggers:
    // ---- 4. sample * gain >> 11, voice volumes ----
    vmudm $v01, $v01, $v02          // voice output
    lbu t7, GO(G_ECHOWR)(fp)
    vmudm $v03, $v01, vVLL          // (MAC pairs: the accumulator is shared)
    vmadh $v03, $v01, vVLH          // (out * volL) >> 6
    vmudm $v04, $v01, vVRL
    vmadh $v04, $v01, vVRH
    vand $v05, $v03, vECHOM
    vand $v06, $v04, vECHOM
    // ---- 5. mix voices 0..7 in order, saturating after each one ----
    vadd $v07, vzero, $v03.e0
    vadd $v08, vzero, $v04.e0
    vadd $v09, vzero, $v05.e0
    vadd $v10, vzero, $v06.e0
    vadd $v07, $v07, $v03.e1
    vadd $v08, $v08, $v04.e1
    vadd $v09, $v09, $v05.e1
    vadd $v10, $v10, $v06.e1
    vadd $v07, $v07, $v03.e2
    vadd $v08, $v08, $v04.e2
    vadd $v09, $v09, $v05.e2
    vadd $v10, $v10, $v06.e2
    vadd $v07, $v07, $v03.e3
    vadd $v08, $v08, $v04.e3
    vadd $v09, $v09, $v05.e3
    vadd $v10, $v10, $v06.e3
    vadd $v07, $v07, $v03.e4
    vadd $v08, $v08, $v04.e4
    vadd $v09, $v09, $v05.e4
    vadd $v10, $v10, $v06.e4
    vadd $v07, $v07, $v03.e5
    vadd $v08, $v08, $v04.e5
    vadd $v09, $v09, $v05.e5
    vadd $v10, $v10, $v06.e5
    vadd $v07, $v07, $v03.e6
    vadd $v08, $v08, $v04.e6
    vadd $v09, $v09, $v05.e6
    vadd $v10, $v10, $v06.e6
    vadd $v07, $v07, $v03.e7
    vadd $v08, $v08, $v04.e7
    vadd $v09, $v09, $v05.e7
    vadd $v10, $v10, $v06.e7
    ctc2 k0, COP2_CTRL_VCC          // lane 0 = left, others = right
    vmrg $v01, $v07, $v08           // main mix
    vmrg $v02, $v09, $v10           // echo input
    vmudm $v15, $v01, $v13
    vmadh $v01, $v01, $v14          // (mix * mvol) >> 7
    ctc2 zero, COP2_CTRL_VCO
    vadd $v02, $v02, $v11           // echo write
    vadd $v01, $v01, $v12           // output
    vand $v02, $v02, vK.e1
    beqz t7, 1f
    nop
    mfc2 t1, $v02.e0
    sb t1, 0(s6)
    srl t1, 8
    sb t1, 1(s6)
    mfc2 t1, $v02.e1
    sb t1, 2(s6)
    srl t1, 8
    sb t1, 3(s6)
1:  lbu t0, GO(G_MUTE)(fp)
    beqz t0, 1f
    li t0, %lo(OUTBUF)+CHUNK_MAX*4
    vxor $v01, $v01, $v01
1:  ssv $v01.e0, 0, s5
    ssv $v01.e1, 2, s5
    addiu s5, 4
    addiu s6, 4
    addiu gp, 1
    andi gp, 7
    // ---- output buffer full? ----
    bne s5, t0, 1f
    nop
    jal FlushOut
    nop
1:  addiu s4, -1
    bnez s4, SampleLoop
    nop

    // ---- end of chunk ----
    sqv vPC,   SO(S_PC),   fp
    sqv vGAIN, SO(S_GAIN), fp
    sqv vRC,   SO(S_RC),   fp
    lbu t0, GO(G_ECHOWR)(fp)
    beqz t0, 1f
    nop
    lw t0, 64(sp)
    lw t2, 68(sp)
    li t1, %lo(ECHOBUF)
    jal DmaOut
    addiu t2, -1
1:  j ChunkLoop
    nop

Finish:
    li t0, %lo(OUTBUF)
    beq s5, t0, 1f
    nop
    jal FlushOut
    nop
1:  sb gp, GO(G_FIRIDX)(fp)
    jal DmaWait
    nop
    lw t0, %lo(JOB_STATE)(zero)
    move t1, fp
    jal DmaOut
    li t2, STATE_SIZE-1
    jal DmaWait
    nop
    break
    nop

//////////////////////////////////////////////////////////////////////////////
// Slow paths from the sample loop
//////////////////////////////////////////////////////////////////////////////
// DoSlow: t0 = voice mask, 72(sp) = routine to call for each voice (a0).
DoSlow:
    sw ra, 88(sp)
    sqv vGAIN, SO(S_GAIN), fp
    sqv vRC,   SO(S_RC),   fp
    move a3, t0
    move a2, zero
1:  andi t1, a3, 1
    beqz t1, 2f
    lw t1, 72(sp)
    jalr t1
    move a0, a2
2:  srl a3, 1
    bnez a3, 1b
    addiu a2, 1
    lqv vGAIN, SO(S_GAIN),    fp
    lqv vRC,   SO(S_RC),      fp
    lqv vINC,  SO(S_INC),     fp
    lqv vRATE, SO(S_RATEEFF), fp
    lqv vREL,  SO(S_RELDEC),  fp
    lqv vKEEP, SO(S_KEEP),    fp
    lqv vGVAL, SO(S_GVAL),    fp
    lw ra, 88(sp)
    jr ra
    nop

//////////////////////////////////////////////////////////////////////////////
// HandleGain: envelope step for voice a0 (state 0..3), like dsp_handleGain.
//////////////////////////////////////////////////////////////////////////////
HandleGain:
    move v1, ra
    sll t0, a0, 5
    addu t0, fp
    addiu t0, SO(S_CH)              // voice
    sll t1, a0, 1
    addu t1, fp                     // lane
    sh zero, SO(S_RC)(t1)
    lhu t2, SO(S_GAIN)(t1)
    lbu t3, C_STATE(t0)
    bnez t3, HG1
    nop
    // attack
    lhu t4, C_RATES(t0)
    li t5, 32
    li $1, 1
    bne t4, $1, 1f
    nop
    li t5, 1024
1:  addu t2, t5
    sltiu $1, t2, 0x7e0
    bnez $1, 2f
    nop
    li t4, 1
    sb t4, C_STATE(t0)
2:  sltiu $1, t2, 0x800
    bnez $1, HGStore
    nop
    j HGStore
    li t2, 0x7ff
HG1:
    li $1, 3
    beq t3, $1, HG3
    nop
    // decay / sustain: gain -= ((gain - 1) >> 8) + 1
    addiu t4, t2, -1
    sra t4, 8
    addiu t4, 1
    subu t2, t4
    li $1, 1
    bne t3, $1, HGStore
    nop
    lhu t5, C_SUSTAIN(t0)
    slt $1, t2, t5
    beqz $1, HGStore
    nop
    li t4, 2
    j HGStore
    sb t4, C_STATE(t0)
HG3:
    lbu t4, C_GMODE(t0)
    bnez t4, 1f
    nop
    // linear decrease
    addiu t2, -32
    bgez t2, HGStore
    nop
    j HGStore
    move t2, zero
1:  li $1, 1
    bne t4, $1, 2f
    nop
    // exponential decrease
    addiu t4, t2, -1
    sra t4, 8
    addiu t4, 1
    j HGStore
    subu t2, t4
2:  li $1, 2
    bne t4, $1, 3f
    li t5, 32
    j 4f                            // linear increase
    nop
3:  sltiu $1, t2, 0x600             // bent increase
    bnez $1, 4f
    nop
    li t5, 8
4:  addu t2, t5
    sltiu $1, t2, 0x800
    bnez $1, HGStore
    nop
    li t2, 0x7ff
HGStore:
    sh t2, SO(S_GAIN)(t1)
    jal RecomputeClass
    nop
    jr v1
    nop

//////////////////////////////////////////////////////////////////////////////
// RecomputeClass: set voice a0's lanes of the envelope helper vectors from its
// state (see the sample loop). Destroys t0..t7.
//////////////////////////////////////////////////////////////////////////////
RecomputeClass:
    sll t0, a0, 5
    addu t0, fp
    addiu t0, SO(S_CH)
    move t5, zero                   // release decrement
    move t6, zero                   // rate counter increment
    li t3, 0x7fff                   // rate
    li t7, 0xffff                   // keep mask
    move t4, zero                   // direct gain value
    lbu t2, C_STATE(t0)
    li $1, 4
    bne t2, $1, 1f
    nop
    j 3f
    li t5, 8
1:  lbu t1, C_USEGAIN(t0)
    lbu $1, C_DIRECT(t0)
    and t1, $1
    beqz t1, 2f
    sll t2, 1
    lhu t4, C_GVALUE(t0)
    j 3f
    move t7, zero
2:  addu t2, t0
    lhu t2, C_RATES(t2)
    beqz t2, 3f
    nop
    move t3, t2
    li t6, 1
3:  sll t1, a0, 1
    addu t1, fp
    sh t5, SO(S_RELDEC)(t1)
    sh t6, SO(S_INC)(t1)
    sh t3, SO(S_RATEEFF)(t1)
    sh t7, SO(S_KEEP)(t1)
    jr ra
    sh t4, SO(S_GVAL)(t1)

//////////////////////////////////////////////////////////////////////////////
// DecodeBrr: next BRR block for voice a0, like dsp_decodeBrr.
//////////////////////////////////////////////////////////////////////////////
// DecSample: one BRR sample (leaf). t2 = 4-bit sample, already sign-extended
// and, for shifts 13..15, reduced to its sign (see DecHave); t3 = shift
// (13 for those); k1 = this block's filter code (DecF0..DecF3, DECFTAB);
// t6 old, t7 older.
DecSample:
    sllv t2, t2, t3
    jr k1
    sra t2, 1
DecF1:
    subu t5, zero, t6               // 1: s += old + (-old >> 4)
    sra t5, 4
    addu t2, t6
    j DecF0
    addu t2, t5
DecF2:
    sll t5, t6, 1                   // 2: s += 2*old + ((3*-old) >> 5) - older + (older >> 4)
    addu t5, t6
    subu t5, zero, t5
    sra t5, 5
    addu t2, t6
    addu t2, t6
    addu t2, t5
    subu t2, t7
    sra t5, t7, 4
    j DecF0
    addu t2, t5
DecF3:
    sll t5, t6, 3                   // 3: s += 2*old + ((13*-old) >> 6) - older + ((3*older) >> 4)
    sll $1, t6, 2
    addu t5, $1
    addu t5, t6
    subu t5, zero, t5
    sra t5, 6
    addu t2, t6
    addu t2, t6
    addu t2, t5
    subu t2, t7
    sll t5, t7, 1
    addu t5, t7
    sra t5, 4
    addu t2, t5
DecF0:
    sra t5, t2, 15                  // within 16 bits: t2 >> 15 is 0 or -1
    addiu t5, 1
    sltiu t5, t5, 2
    beqz t5, DecClamp
    sll t5, t2, 17                  // clip to 15 bits
DecStore:
    sra t2, t5, 17
    move t7, t6
    move t6, t2
    sll t5, t2, 1
    sh t5, 0(a1)
    jr ra
    addiu a1, 2
DecClamp:                           // clamp to 16 bits first
    slt t5, t2, zero
    li t2, 0x7fff
    beqz t5, DecStore
    sll t5, t2, 17
    li t2, -0x8000
    j DecStore
    sll t5, t2, 17

DecodeBrr:
    move v1, ra
    sll t0, a0, 5
    addu k0, t0, fp
    addiu k0, SO(S_CH)              // voice
    sll t1, a0, 4
    addu t1, t0
    addu t1, fp
    addiu t1, SO(DECBUF)            // buffer
    lw t2, 32(t1)                   // keep the last 3 samples
    lhu t3, 36(t1)
    sw t2, 0(t1)
    sh t3, 4(t1)
    lbu t4, C_PFLAGS(k0)
    andi t5, t4, 1
    beqz t5, DecNoLoop
    nop
    // end/loop flag of the previous block: continue at the loop point
    lbu t6, C_SRCN(k0)
    lhu t7, GO(G_DIRPAGE)(fp)
    sll t6, 2
    addu a1, t6, t7
    addiu a1, 2
    jal ReadAram16
    andi a1, 0xffff
    sh v0, C_OFFSET(k0)
    lbu t4, C_PFLAGS(k0)
    li t5, 1
    bne t4, t5, DecNoLoop
    nop
    li t5, 4                        // end without loop: release, gain 0
    sb t5, C_STATE(k0)
    sll t6, a0, 1
    addu t6, fp
    sh zero, SO(S_GAIN)(t6)
    jal RecomputeClass
    nop
DecNoLoop:
    lhu t6, C_OFFSET(k0)
    lbu t7, %lo(PFVALID)(a0)
    lhu t5, C_PFADDR(k0)
    beqz t7, DecFetch
    nop
    beq t5, t6, DecHave
    nop
DecFetch:
    jal DmaWait
    nop
    andi t0, t6, 0xfff8
    lw t2, %lo(JOB_ARAM)(zero)
    addu t0, t2
    sll t1, a0, 4
    addiu t1, %lo(BRRBUF)
    jal DmaIn
    li t2, 15
DecHave:
    jal DmaWait
    nop
    sll t1, a0, 4
    addiu t1, %lo(BRRBUF)
    andi t2, t6, 7
    addu t1, t2                     // block header
    lbu t2, 0(t1)
    srl t3, t2, 4                   // shift
    andi t4, t2, 0x0c               // filter * 4
    lw k1, %lo(DECFTAB)(t4)         // its code
    andi t5, t2, 3
    sb t5, C_PFLAGS(k0)
    sltiu $1, t3, 13
    bnez $1, 1f
    li t4, 28                       // sample = byte bits >> 28 (sign-extended)
    li t3, 13                       // shifts 13..15: (s >> 3) << 12 = (sign << 13) >> 1
    li t4, 31
1:
    lh t6, C_OLD(k0)
    lh t7, C_OLDER(k0)
    sll t0, a0, 5
    sll a1, a0, 4
    addu a1, t0
    addu a1, fp
    addiu a1, SO(DECBUF)+6          // samples 3..18
    li v0, 8
DecByte:
    addiu t1, 1
    lbu t0, 0(t1)
    sll t2, t0, 24
    jal DecSample
    srav t2, t2, t4
    sll t2, t0, 28
    jal DecSample
    srav t2, t2, t4
    addiu v0, -1
    bnez v0, DecByte
    nop
    sh t6, C_OLD(k0)
    sh t7, C_OLDER(k0)
    lhu t6, C_OFFSET(k0)
    addiu t6, 9
    andi t6, 0xffff
    sh t6, C_OFFSET(k0)
    // prefetch the following block
    andi t0, t6, 0xfff8
    lw t2, %lo(JOB_ARAM)(zero)
    addu t0, t2
    sll t1, a0, 4
    addiu t1, %lo(BRRBUF)
    jal DmaIn
    li t2, 15
    sh t6, C_PFADDR(k0)
    li t0, 1
    sb t0, %lo(PFVALID)(a0)
    jr v1
    nop

//////////////////////////////////////////////////////////////////////////////
// ReadAram16: v0 = little-endian u16 at ARAM address a1. Destroys t0..t3, k1.
//////////////////////////////////////////////////////////////////////////////
ReadAram16:
    move k1, ra
    jal DmaWait
    nop
    andi t0, a1, 0xfff8
    lw t2, %lo(JOB_ARAM)(zero)
    addu t0, t2
    addiu t1, sp, 48
    jal DmaIn
    li t2, 15
    jal DmaWait
    nop
    andi t0, a1, 7
    addu t0, sp
    lbu v0, 48(t0)
    lbu t1, 49(t0)
    sll t1, 8
    or v0, t1
    jr k1
    nop

//////////////////////////////////////////////////////////////////////////////
// ForceReset: DSP reset flag: every voice to release with gain 0.
//////////////////////////////////////////////////////////////////////////////
ForceReset:
    move k0, ra
    li a0, 7
1:  sll t0, a0, 5
    addu t0, fp
    li t1, 4
    sb t1, SO(S_CH)+C_STATE(t0)
    sll t1, a0, 1
    addu t1, fp
    sh zero, SO(S_GAIN)(t1)
    jal RecomputeClass
    nop
    bgtz a0, 1b
    addiu a0, -1
    jr k0
    nop

//////////////////////////////////////////////////////////////////////////////
// PeekLog: v0 = sample number of the next log entry, or 0xffff. Refills LOGBUF.
//////////////////////////////////////////////////////////////////////////////
PeekLog:
    bne s2, s3, 2f
    nop
    beqz t9, 3f
    move k1, ra
    li t2, 32
    sltu $1, t9, t2
    beqz $1, 1f
    nop
    move t2, t9
1:  subu t9, t2
    sll t2, 2
    li s2, %lo(LOGBUF)
    addu s3, s2, t2
    move t0, t8
    addiu t8, 128
    move t1, s2
    jal DmaIn
    li t2, 127
    jal DmaWait
    nop
    move ra, k1
2:  jr ra
    lhu v0, 0(s2)
3:  jr ra
    li v0, 0xffff

//////////////////////////////////////////////////////////////////////////////
// FlushOut: write OUTBUF (s5 - OUTBUF bytes, rounded up to 8) to s7.
//////////////////////////////////////////////////////////////////////////////
FlushOut:
    move k1, ra
    li t1, %lo(OUTBUF)
    subu t2, s5, t1
    addiu t2, 7
    li t3, ~7
    and t2, t3
    move t0, s7
    addu s7, t2
    jal DmaOut
    addiu t2, -1
    jal DmaWait
    nop
    li s5, %lo(OUTBUF)
    jr k1
    nop

//////////////////////////////////////////////////////////////////////////////
// ApplyWrite: DSP register a0 = a1, like dsp_write. a0 = 0x80: dsp_reset.
//////////////////////////////////////////////////////////////////////////////
ApplyWrite:
    move v1, ra
    li t0, 0x80
    beq a0, t0, DspReset
    andi t1, a0, 0xf                // low nibble
    srl t2, a0, 4                   // voice
    sll t3, t2, 1
    addu t3, fp                     // lane
    sll t4, t2, 5
    addu t4, fp
    addiu t4, SO(S_CH)              // voice state
    sll t5, a1, 24
    sra t5, 24                      // signed value
    sll t6, t1, 2
    lw t6, %lo(WTAB)(t6)
    jr t6
    nop
W_Ret:
    jr v1
    nop

W_VolL:
    andi t6, t5, 63
    sll t6, 10
    sh t6, SO(S_VLL)(t3)
    sra t6, t5, 6
    j W_Ret
    sh t6, SO(S_VLH)(t3)
W_VolR:
    andi t6, t5, 63
    sll t6, 10
    sh t6, SO(S_VRL)(t3)
    sra t6, t5, 6
    j W_Ret
    sh t6, SO(S_VRH)(t3)
W_PitchL:
    lhu t6, SO(S_PITCH)(t3)
    andi t6, 0x3f00
    or t6, a1
    j W_Ret
    sh t6, SO(S_PITCH)(t3)
W_PitchH:
    lhu t6, SO(S_PITCH)(t3)
    andi t6, 0xff
    sll t7, a1, 8
    or t6, t7
    andi t6, 0x3fff
    j W_Ret
    sh t6, SO(S_PITCH)(t3)
W_Srcn:
    j W_Ret
    sb a1, C_SRCN(t4)
W_Adsr1:
    andi t6, a1, 0xf
    sll t6, 2                       // (v & 15) * 2 entries * 2 bytes
    lhu t7, %lo(RATES)+2(t6)
    sh t7, C_RATES+0(t4)
    srl t6, a1, 4
    andi t6, 7
    sll t6, 2
    lhu t7, %lo(RATES)+32(t6)
    sh t7, C_RATES+2(t4)
    srl t6, a1, 7
    xori t6, 1
    sb t6, C_USEGAIN(t4)
    j W_Class
    nop
W_Adsr2:
    andi t6, a1, 0x1f
    sll t6, 1
    lhu t7, %lo(RATES)(t6)
    sh t7, C_RATES+4(t4)
    srl t6, a1, 5
    addiu t6, 1
    sll t6, 8
    sh t6, C_SUSTAIN(t4)
    j W_Class
    nop
W_Gain:
    srl t6, a1, 7
    xori t7, t6, 1
    sb t7, C_DIRECT(t4)
    beqz t6, 1f
    nop
    srl t6, a1, 5
    andi t6, 3
    sb t6, C_GMODE(t4)
    andi t6, a1, 0x1f
    sll t6, 1
    lhu t7, %lo(RATES)(t6)
    j W_Class
    sh t7, C_RATES+6(t4)
1:  andi t6, a1, 0x7f
    sll t6, 4
    sh t6, C_GVALUE(t4)
W_Class:
    jal RecomputeClass
    move a0, t2
    j W_Ret
    nop
W_Fir:
    andi t6, t5, 63
    sll t6, 10
    sh t6, SO(S_FIRL)(t3)
    sra t6, t5, 6
    j W_Ret
    sh t6, SO(S_FIRH)(t3)

W_GlobC:                            // t2: 0 MVOLL 1 MVOLR 2 EVOLL 3 EVOLR 4 KON 5 KOF 6 FLG 7 ENDX
    andi t6, t5, 127
    sll t6, 9                       // lo part of v << 9
    sra t7, t5, 7                   // hi part
    sltiu $1, t2, 2
    beqz $1, 1f
    nop
    sll t0, t2, 1
    addu t0, fp
    sh t6, SO(S_MVL)(t0)
    j W_Ret
    sh t7, SO(S_MVH)(t0)
1:  sltiu $1, t2, 4
    beqz $1, 2f
    nop
    sll t0, t2, 1
    addu t0, fp
    sh t6, SO(S_EVL)-4(t0)
    j W_Ret
    sh t7, SO(S_EVH)-4(t0)
2:  li $1, 4
    beq t2, $1, W_Kon
    li $1, 5
    beq t2, $1, W_Kof
    li $1, 6
    bne t2, $1, W_Ret
    nop
    // FLG
    srl t6, a1, 7
    sb t6, GO(G_RESET)(fp)
    beqz t6, 1f
    nop
    jal ForceReset
    nop
1:
    srl t6, a1, 6
    andi t6, 1
    sb t6, GO(G_MUTE)(fp)
    srl t6, a1, 5
    andi t6, 1
    xori t6, 1
    sb t6, GO(G_ECHOWR)(fp)
    andi t6, a1, 0x1f
    sll t6, 1
    lhu t7, %lo(RATES)(t6)
    j W_Ret
    sh t7, GO(G_NRATE)(fp)
W_Kof:
    move a2, a1
    li a0, 0
1:  andi t0, a2, 1
    beqz t0, 2f
    sll t0, a0, 5
    addu t0, fp
    li t1, 4
    sb t1, SO(S_CH)+C_STATE(t0)
    jal RecomputeClass
    nop
2:  srl a2, 1
    bnez a2, 1b
    addiu a0, 1
    j W_Ret
    nop
W_Kon:
    move a2, a1
    li a0, 0
1:  andi t0, a2, 1
    beqz t0, 2f
    nop
    jal KeyOn
    nop
2:  srl a2, 1
    bnez a2, 1b
    addiu a0, 1
    j W_Ret
    nop

W_GlobD:                            // t2: 0 EFB 2 PMON 3 NON 4 EON 5 DIR 6 ESA 7 EDL
    bnez t2, 1f
    nop
    andi t6, t5, 127
    sll t6, 9
    sra t7, t5, 7
    sh t6, SO(S_FBL)(fp)
    sh t6, SO(S_FBL)+2(fp)
    sh t7, SO(S_FBH)(fp)
    j W_Ret
    sh t7, SO(S_FBH)+2(fp)
1:  li $1, 3
    bne t2, $1, 1f
    nop
    j W_Ret
    sb a1, GO(G_NONMASK)(fp)
1:  li $1, 4
    bne t2, $1, 2f
    nop
    li t0, 0                        // EON -> lane masks
1:  srlv t6, a1, t0
    andi t6, 1
    subu t6, zero, t6
    sll t7, t0, 1
    addu t7, fp
    sh t6, SO(S_ECHOM)(t7)
    addiu t0, 1
    li $1, 8
    bne t0, $1, 1b
    nop
    j W_Ret
    nop
2:  li $1, 5
    bne t2, $1, 1f
    sll t6, a1, 8
    j W_Ret
    sh t6, GO(G_DIRPAGE)(fp)
1:  li $1, 6
    bne t2, $1, 1f
    nop
    j W_Ret
    sh t6, GO(G_ESA)(fp)
1:  li $1, 7
    bne t2, $1, W_Ret
    andi t6, a1, 0xf
    sll t6, 9
    bnez t6, 1f
    nop
    li t6, 1
1:  j W_Ret
    sh t6, GO(G_EDL)(fp)

// KeyOn: voice a0 restarts its sample. Keeps a0, a2.
KeyOn:
    move a3, ra
    sll t0, a0, 5
    addu k0, t0, fp
    addiu k0, SO(S_CH)
    sb zero, C_PFLAGS(k0)
    sb zero, %lo(PFVALID)(a0)
    lbu t6, C_SRCN(k0)
    lhu t7, GO(G_DIRPAGE)(fp)
    sll t6, 2
    addu a1, t6, t7
    jal ReadAram16
    andi a1, 0xffff
    sh v0, C_OFFSET(k0)
    sll t0, a0, 5                   // clear the decode buffer
    sll t1, a0, 4
    addu t1, t0
    addu t1, fp
    sqv vzero, SO(DECBUF)+0, t1
    sqv vzero, SO(DECBUF)+16, t1
    sqv vzero, SO(DECBUF)+32, t1
    sll t1, a0, 1
    addu t1, fp
    sh zero, SO(S_GAIN)(t1)
    lbu t0, C_USEGAIN(k0)
    li t1, 3
    bnez t0, 1f
    nop
    move t1, zero
1:  lbu t0, GO(G_RESET)(fp)          // reset flag: stays released
    beqz t0, 2f
    nop
    li t1, 4
2:  sb t1, C_STATE(k0)
    jal RecomputeClass
    nop
    jr a3
    nop

// DspReset: like dsp_reset (all state zero, then the non-zero defaults).
DspReset:
    move t0, fp
    li t1, STATE_SIZE/16
1:  sqv vzero, 0, t0
    addiu t1, -1
    bnez t1, 1b
    addiu t0, 16
    li t0, 1
    sb t0, GO(G_MUTE)(fp)
    sb t0, GO(G_RESET)(fp)
    sh t0, GO(G_EDL)(fp)
    sh t0, GO(G_EREMAIN)(fp)
    move gp, zero
    vor $v01, vzero, vK.e5          // rate 0x7fff, keep 0xffff for every voice
    sqv $v01, SO(S_RATEEFF), fp
    vnor $v01, vzero, vzero
    sqv $v01, SO(S_KEEP), fp
    j W_Ret
    nop

//////////////////////////////////////////////////////////////////////////////
// DMA helpers. t0 = RDRAM address, t1 = DMEM address, t2 = length - 1.
//////////////////////////////////////////////////////////////////////////////
DmaIn:
    mfc0 $1, COP0_DMA_FULL
    bnez $1, DmaIn
    nop
    mtc0 t1, COP0_DMA_SPADDR
    mtc0 t0, COP0_DMA_RAMADDR
    jr ra
    mtc0 t2, COP0_DMA_READ

DmaOut:
    mfc0 $1, COP0_DMA_FULL
    bnez $1, DmaOut
    nop
    mtc0 t1, COP0_DMA_SPADDR
    mtc0 t0, COP0_DMA_RAMADDR
    jr ra
    mtc0 t2, COP0_DMA_WRITE

DmaWait:
    mfc0 $1, COP0_DMA_BUSY
    bnez $1, DmaWait
    nop
    jr ra
    nop
