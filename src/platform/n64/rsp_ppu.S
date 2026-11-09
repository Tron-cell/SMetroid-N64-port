##############################################################################
# rsp_ppu.S - SNES PPU (mode 1) scanline renderer for the N64 RSP.
#
# Draws a frame captured by snes/ppu_rsp.c: for each PpuLineRec it builds the
# sprite line from pre-decoded slivers, draws BG1/BG2 (4bpp) and BG3 (2bpp)
# into main/sub z-buffers, then does palette lookup, colour window, colour
# math and brightness, and writes RGBA5551 pixels to the framebuffer.
#
# Must match PpuRsp_RenderFrameC() (snes/ppu_rsp.c) pixel for pixel.
#
# Z-buffer values are the C renderer's values XOR 0x8000, so the RSP's signed
# vector compares order them like the C code's unsigned compares.
#
# Standalone microcode (no rspq): the CPU fills JOB in DMEM, runs from PC 0,
# and waits for the break.
##############################################################################
#include <rsp.inc>

    .set noat

# ---- PpuLineRec field offsets (see ppu_rsp.h) ----
#define LR_FLAGS      0
#define LR_BRIGHT     1
#define LR_SCREN      2     // +sub
#define LR_SCRWIN     4     // +sub
#define LR_MATHEN     6
#define LR_CGIDX      7
#define LR_W1L        8
#define LR_W1R        9
#define LR_W2L        10
#define LR_W2R        11
#define LR_CLIPMODE   12
#define LR_PMM        13
#define LR_MATHFLAGS  14
#define LR_SLIVCNT    15
#define LR_FIXED      16
#define LR_SLIVSTART  18
#define LR_WINSEL     20
#define LR_BG         24    // 10 bytes per layer: tm0, tm1, ta0, ta1, hscroll
#define LR_BLOCK      54    // u16 per layer: tile block index or 0xffff
#define LR_DRAWMASK   60    // bits 0-2 main BG1-3, 3-5 sub BG1-3, bit 6 sub screen rendered, bit 7 sprites built in MAINBUF
#define LR_CBITS      61    // compose bits: 0 keep, 1 sub usable, 2 half, 3 half w/o sub, 4 subtract, 5 math

#define F_FORCEDBLANK 1
#define F_CPUDRAWN    2
#define F_SPRITES     4
#define F_VALID       8
#define F_MODE7       16    // BG1 from a mode 7 row (LR_BLOCK = row index)
#define F_COPY        32    // same pixels as this line of the previous frame: copy them (JOB_PREVD)

    .data
    .align 4
# ---- Job, written by the CPU before each run ----
JOB_VRAM:     .long 0          # RDRAM address of VRAM snapshot (64 KB)
JOB_LINES:    .long 0          # PpuLineRec array
JOB_CGRAMS:   .long 0          # CGRAM snapshots, 512 bytes each
JOB_SLIVERS:  .long 0          # PpuSliver array
JOB_FB:       .long 0          # framebuffer address of line record 0 (RGBA5551)
JOB_STRIDE:   .long 0          # framebuffer stride in bytes
JOB_NLINES:   .half 0          # number of line records
JOB_DBGFLAGS: .half 0          # bit0: dump z-buffers per line to JOB_DBG
JOB_DBG:      .long 0
JOB_BLOCKS:   .long 0          # PpuTileBlock array
JOB_M7:       .long 0          # mode 7 BG1 rows, 256 bytes each
JOB_PREVD:    .long 0          # previous frame's framebuffer - this frame's (F_COPY lines)
JOB_MASK:     .long 0          # 0, or where drawn lines' main z-buffers go (512 bytes per line; rsp_fx.S)

    .align 4
V_POWN:  .half 1, 2, 4, 8, 16, 32, 64, 128                 # lane k: << k     (normal tiles)
V_POWF:  .half 128, 64, 32, 16, 8, 4, 2, 1                 # lane k: << (7-k) (h-flipped tiles)
V_LANE:  .half 0, 1, 2, 3, 4, 5, 6, 7
V_K1:    .half 0x0080, 0x8000, 0x0200, 0x0004, 0x0800, 0x0010, 0x00ff, 0x8500
V_K2:    .half 0x001f, 0x0100, 4370, 0x000f, 0x0008, 0x4000, 0x0001, 0x0800
V_K3:    .half 0x0040, 0x0002, 0x0800, 0x0040, 0x8000, 0x2000, 0x001f, 0xffff
V_K4:    .half 0x03e0, 0x7c00, 0, 0x0020, 0x0400, 0x003e, 0, 0
# K4: e0 g mask in place, e1 b mask in place, e2 CGRAM address (set at start),
#     e3 >>11 (vmudl), e4 >>6 (vmudl), e5 0x3e (doubled channel mask / 2 * 31)
# Brightness b: T(c) = ((((c << 3) | (c >> 2)) * b) / 15) >> 3 equals
# (2c * V_BMUL[b]) >> 16 exactly for c = 0..31 (found by search).
V_BMUL:  .half 0, 2185, 4469, 6554, 8937, 10923, 13493, 15729
         .half 18023, 20165, 22528, 24576, 26986, 29258, 31458, 32768
V_NIB:   .half 1, 16, 256, 4096, 1, 16, 256, 4096         # lane k: nibble k of its halfword to the top
# K1: e0 bit7, e1 bit15 / MIN (biased), e2 >>7 (vmudl), e3 >>14, e4 >>5, e5 >>12, e6 0xff, e7 biased backdrop
# K2: e0 0x1f, e1 >>8 (vmudl), e2 *4370 (vmudl: /15), e3 0x0f, e4 <<3 (vmudn), e5 >>2 (vmudl), e6 1, e7 <<11 (vmudn)
# K3: e0 <<6 (vmudn), e1 <<1 (vmudn), e2 >>5 (vmudl), e3 >>10 (vmudl), e4 >>1 (vmudl), e5 >>3 (vmudl), e6 31, e7 0xffff

    .align 4
SCR:      .space 64            # scratch: gathered colours etc.
    .align 4
LINE:     .space 64            # current PpuLineRec
    .align 4
CGRAM:    .space 512
    .align 4
          .space 16            # pad: Compose's math loop stores a dummy group here
OBJBUF:   .space 544           # sprite line, 272 px (pixel x at index x + 8); reused as OUT
    .align 4
MAINBUF:  .space 544+16        # main screen z-buffer, 272 px (+16: BlkFast's 34th tile)
    .align 4
SUBBUF:   .space 544+16        # sub screen z-buffer (+16 padding)
    .align 4
LINEN:    .space 64            # next line's PpuLineRec (prefetched)
# Two sets of one layer's tile row data, so the next layer's DMA runs while
# this one is drawn: +0 z values (36), +80 planes 0/1 of rows (y & 4)..+3,
# 34 tiles x 8 bytes, +352 the same for planes 2/3.
    .align 4
BGSET0:   .space 624
BGSET1:   .space 624
#define SET_Z    0
#define SET_P01  80
#define SET_P23  352
#define SET_SIZE 624
#define P01BUF   (BGSET0 + SET_P01)   // also the sliver records (before any BG prefetch)
#define BLOCK_SIZE 2248
#define BLOCK_TILES 200

#define OUTBUF OBJBUF

# ---- vector registers ----
#define vzero  $v00
#define vpown  $v20
#define vlane  $v22
#define vk1    $v23
#define vk2    $v24
#define vk3    $v25
#define vline  $v26     // per line: e0 brightness, e1/e2/e3 fixed colour r/g/b
#define vwin   $v18     // window params: e0 l1, e1 r1+1, e2 l2, e3 r2+1
#define vxpos  $v10     // input to WinMask: screen x of each lane
#define vones  $v27     // 0xffff in every lane
#define vk4    $v28
#define vcm    $v21     // Compose: per-line masks
#define vone1  $v30     // 1 in every lane
#define vnib   $v16     // DrawBg (4bpp): V_NIB

    .text
    .globl _start
_start:
    vxor vzero, vzero, vzero
    li t0, %lo(V_POWN)
    lqv vpown, 0x00, t0
    lqv vlane, 0x20, t0
    lqv vk1,   0x30, t0
    lqv vk2,   0x40, t0
    lqv vk3,   0x50, t0
    lqv vk4,   0x60, t0
    li t1, %lo(CGRAM)
    mtc2 t1, vk4.e2
    vor vones, vzero, vk3.e7
    vor vone1, vzero, vk2.e6
    li s7, -1                       # cached CGRAM index
    sw zero, %lo(SCR)+44(zero)      # line-to-line flags (+45, +46)
    lhu s6, %lo(JOB_NLINES)(zero)
    move s5, zero                   # line index
    lw t0, %lo(JOB_LINES)(zero)     # first line record
    li t1, %lo(LINEN)
    jal DmaIn
    li t2, 64-1

LineLoop:
    beq s5, s6, Done
    nop
    # wait for the previous line's DMAs (output: OUTBUF is reused as OBJBUF;
    # next record), unless that line ended with SCR+45 set: then its record is
    # in, and the output DMA is waited for only where OBJBUF / OUTBUF are
    # written early (BuildObj with sprites, ForcedBlank)
    li t0, %lo(LINEN)
    lbu t1, %lo(SCR)+45(zero)
    bnez t1, 1f
    sb zero, %lo(SCR)+45(zero)
    jal DmaWait
    nop
1:
    lqv $v01, 0x00, t0
    lqv $v02, 0x10, t0
    lqv $v03, 0x20, t0
    lqv $v04, 0x30, t0
    li t1, %lo(LINE)
    sqv $v01, 0x00, t1
    sqv $v02, 0x10, t1
    sqv $v03, 0x20, t1
    sqv $v04, 0x30, t1
    # prefetch the next line's record (the array has room past the last line)
    lw t0, %lo(JOB_LINES)(zero)
    sll t1, s5, 6
    addu t0, t1
    addiu t0, 64
    li t1, %lo(LINEN)
    jal DmaIn
    li t2, 64-1
    lbu t0, %lo(LINE)+LR_FLAGS(zero)
    andi t1, t0, F_VALID|F_CPUDRAWN
    xori t1, F_VALID
    bnez t1, NextLine               # not valid, or drawn by the CPU
    andi t1, t0, F_FORCEDBLANK
    bnez t1, ForcedBlank
    andi t1, t0, F_COPY
    bnez t1, CopyLine
    nop

    # palette
    lbu t0, %lo(LINE)+LR_CGIDX(zero)
    beq t0, s7, 1f
    move s7, t0
    lw t1, %lo(JOB_CGRAMS)(zero)
    sll t0, 9
    addu t0, t1
    li t1, %lo(CGRAM)
    jal DmaIn                       # (already RGBA5551: PpuRsp_ConvertCgrams; waited for before Compose)
    li t2, 512-1
1:
    jal BuildObj
    nop
    # BG draws of this line, in order: bits 0-2 main BG1-3, bits 3-5 sub BG1-3
    lbu t0, %lo(LINE)+LR_DRAWMASK(zero)
    andi t0, 0x3f
    sb t0, %lo(SCR)+44(zero)
    # the first draw's data: prefetched during the previous line's Compose
    # (SCR+46 = its set), or now
    lhu t1, %lo(SCR)+46(zero)
    bnez t1, 4f
    li gp, %lo(LINE)
    jal PrefetchFirst
    li v1, %lo(BGSET0)
4:  lhu t1, %lo(SCR)+46(zero)
    sh zero, %lo(SCR)+46(zero)
    sh t1, %lo(SCR)+40(zero)        # set holding the next draw's data

    # main screen
    li a0, %lo(MAINBUF)
    jal DrawScreen
    li a1, 0

    # sub screen (drawn only when the compose step can use it)
    lbu t0, %lo(LINE)+LR_DRAWMASK(zero)
    andi s3, t0, 0x40               # s3 = sub screen rendered
    beqz s3, NoSub
    li a0, %lo(SUBBUF)
    jal DrawScreen
    li a1, 1
NoSub:
#ifdef PPU_DEBUG_DUMP
    # optional debug dump of the z-buffers
    lhu t0, %lo(JOB_DBGFLAGS)(zero)
    andi t0, 1
    beqz t0, 1f
    nop
    jal DebugDump
    nop
1:
#endif
    jal DmaWait                     # palette must be in
    nop
    # start the next line's first BG draw DMAs now, into BGSET1 (free; BGSET0
    # also takes the next line's slivers), so they run during Compose
    li gp, %lo(LINEN)
    jal PrefetchFirst
    li v1, %lo(BGSET1)
    jal Compose
    nop
    # the z values for rsp_fx.S: main screen, or the sub screen where the main
    # one shows only BG3 or the backdrop (rooms whose fog / haze BG3 covers the
    # main screen and the level is on the sub screen). Compose is done with
    # both buffers; the next line's DmaWait covers the DMA.
    lw t0, %lo(JOB_MASK)(zero)
    beqz t0, WriteLineN
    lbu t3, %lo(LINE)+LR_DRAWMASK(zero)
    andi t3, 0x40                   # sub screen rendered?
    beqz t3, 2f
    li t1, %lo(MAINBUF) + 16
    vaddc $v14, vzero, vzero        # VCO = 0 for the compares
    vor $v15, vzero, vk4.e4
    vor $v15, $v15, vk1.e2
    vor $v15, $v15, vk2.e1          # 0x0700: layer bits of the biased z
    li t3, 32                       # (one group per pass: IMEM is full)
1:
    lqv $v01, 0x00, t1
    lqv $v05, 0x230, t1             # SUBBUF = MAINBUF + 0x230
    vand $v09, $v01, $v15
    veq $v14, $v09, vk1.e2          # BG3
    vmrg $v01, $v05, $v01
    veq $v14, $v01, vk1.e7          # backdrop
    vmrg $v01, $v05, $v01
    sqv $v01, 0x00, t1
    addiu t3, -1
    bnez t3, 1b
    addiu t1, 16
    li t1, %lo(MAINBUF) + 16
2:  sll t2, s5, 9
    addu t0, t2
    jal DmaOut
    li t2, 512-1
    j WriteLine
    nop

ForcedBlank:
    # 256 pixels of RGBA5551 black (0x0001) (ClearBufV fills 272: OUTBUF has room),
    # once the previous line's output has left OUTBUF
    jal DmaWait
    li a0, %lo(OUTBUF)
    jal ClearBufV
    vor $v01, vzero, vk2.e6
    j WriteLine
    nop

CopyLine:
    # the previous frame's pixels of this line (the CPU checked that all inputs are equal)
    lw t0, %lo(JOB_FB)(zero)
    lw t1, %lo(JOB_PREVD)(zero)
    addu t0, t1
    li t1, %lo(OUTBUF)
    jal DmaIn
    li t2, 512-1
    jal DmaWait
    nop
WriteLineN:                         # the next record is in, MAINBUF isn't being written out
    li t0, 1
    sb t0, %lo(SCR)+45(zero)
WriteLine:
    lw t0, %lo(JOB_FB)(zero)        # this line's framebuffer address
    li t1, %lo(OUTBUF)
    jal DmaOut
    li t2, 512-1

NextLine:
    lw t0, %lo(JOB_FB)(zero)        # advanced line by line
    lw t1, %lo(JOB_STRIDE)(zero)
    addu t0, t1
    sw t0, %lo(JOB_FB)(zero)
    j LineLoop
    addiu s5, 1

Done:
    jal DmaWait
    nop
    break
    nop

##############################################################################
# DMA helpers. t0 = RDRAM address, t1 = DMEM address, t2 = length - 1.
# Async: they only wait for a free slot in the DMA queue. Destroy at.
##############################################################################
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

##############################################################################
# ClearBuf: a0 = 272-pixel buffer, filled with biased backdrop 0x8500.
##############################################################################
ClearBuf:
    vor $v01, vzero, vk1.e7
ClearBufV:                          # fill with $v01 (35 x 16 bytes: for OBJBUF the
    li t1, 5                        # 35th is MAINBUF's unused first 16 bytes)
    move t0, a0
1:  sqv $v01, 0x000, t0
    sqv $v01, 0x010, t0
    sqv $v01, 0x020, t0
    sqv $v01, 0x030, t0
    sqv $v01, 0x040, t0
    sqv $v01, 0x050, t0
    sqv $v01, 0x060, t0
    addiu t1, -1
    bnez t1, 1b
    addiu t0, 0x70
    jr ra
    nop

##############################################################################
# WinSetup: prepare the window mask for layer a2 on screen a1 (0 main, 1 sub).
# Output: t8 = mode (0: no mask, 1: everything masked, 2: per-pixel mask
#   with bit2 window1 active, bit3 window2 active, bit4 w1 outside-type,
#   bit5 w2 outside-type), vwin loaded.
# Same rules as Windows_Calc: window k enabled + (l <= r) => masks inside
# (or outside if inverted); enabled + l > r => masks nothing, or everything
# (if) inverted.
# Destroys t3..t7.
##############################################################################
WinSetup:
    lbu t3, %lo(LINE)+LR_SCRWIN(a1)
    srlv t3, t3, a2
    andi t3, 1
    beqz t3, WinNone                # layer not windowed on this screen
    lw t4, %lo(LINE)+LR_WINSEL(zero)
    sll t5, a2, 2
    srlv t4, t4, t5                 # t4 = window flags for the layer
WinSetupFlags:
    move t8, zero
    # window 1
    andi t5, t4, 2                  # enabled
    beqz t5, 2f
    lbu t6, %lo(LINE)+LR_W1L(zero)
    lbu t7, %lo(LINE)+LR_W1R(zero)
    sltu t5, t7, t6                 # r < l: invalid
    beqz t5, 1f
    andi t5, t4, 1                  # inverted?
    bnez t5, WinAll
    nop
    j 2f
    nop
1:  ori t8, 4
    sll t5, 4
    or t8, t5                       # bit4 = inverted
    mtc2 t6, vwin.e0
    addiu t7, 1
    mtc2 t7, vwin.e1
2:  # window 2
    andi t5, t4, 8
    beqz t5, 4f
    lbu t6, %lo(LINE)+LR_W2L(zero)
    lbu t7, %lo(LINE)+LR_W2R(zero)
    sltu t5, t7, t6
    beqz t5, 3f
    andi t5, t4, 4
    bnez t5, WinAll
    nop
    j 4f
    nop
3:  ori t8, 8
    sll t5, 3                       # bit2 (inverted) -> bit5
    or t8, t5
    mtc2 t6, vwin.e2
    addiu t7, 1
    mtc2 t7, vwin.e3
4:  beqz t8, WinNone
    nop
    jr ra
    ori t8, 2
WinNone:
    jr ra
    move t8, zero
WinAll:
    jr ra
    li t8, 1

##############################################################################
# WinMask: for the lanes' screen x in vxpos, sets VCC = pixel allowed
# (not masked). Uses t8 from WinSetup (mode 2). Destroys $v11..$v13, k0.
##############################################################################
WinMask:
    vxor $v13, $v13, $v13
    andi k0, t8, 4
    beqz k0, 1f
    andi k0, t8, 16
    vsubc $v11, vxpos, vwin.e0      # x - l
    vsubc $v12, vxpos, vwin.e1      # x - (r+1)
    vxor $v11, $v11, vk3.e7         # ~(x - l)
    vand $v11, $v11, $v12           # sign set = inside
    beqz k0, 5f
    nop
    vxor $v11, $v11, vk3.e7         # outside-type window
5:  vor $v13, $v13, $v11
1:  andi k0, t8, 8
    beqz k0, 2f
    andi k0, t8, 32
    vsubc $v11, vxpos, vwin.e2
    vsubc $v12, vxpos, vwin.e3
    vxor $v11, $v11, vk3.e7
    vand $v11, $v11, $v12
    beqz k0, 6f
    nop
    vxor $v11, $v11, vk3.e7
6:  vor $v13, $v13, $v11
2:  jr ra
    # VCC = sign clear = allowed. Against -1, not 0: VGE settles "equal" with
    # the carry flags of the last VSUBC, and $v13 is often exactly 0 left of a
    # window (~(x - l) & (x - r - 1)), which then came out masked: holes in
    # windowed layers left of the window (the security eyes' beams). A value
    # of -1 means a 1-pixel window at x, where that VSUBC left "masked": right.
    vge $v11, $v13, vones

##############################################################################
# DecodeRow4: $v01.e0 = planes 0/1 word, $v01.e1 = planes 2/3 word (0 for
# 2bpp). Output $v02 = pixel values for lanes 0..7 (bit 7 = leftmost pixel;
# tiles and slivers are stored already flipped).
##############################################################################
DecodeRow4:
    vmudn $v03, vpown, $v01.e0
    vmudn $v04, vpown, $v01.e1
    vand $v05, $v03, vk1.e0
    vand $v06, $v03, vk1.e1
    vand $v07, $v04, vk1.e0
    vand $v08, $v04, vk1.e1
    vmudl $v02, $v05, vk1.e2
    vmadl $v02, $v06, vk1.e3
    vmadl $v02, $v07, vk1.e4
    jr ra
    vmadl $v02, $v08, vk1.e5

##############################################################################
# BuildObj: OBJBUF = sprite line from this line's slivers.
##############################################################################
BuildObj:
    lbu s0, %lo(LINE)+LR_SLIVCNT(zero)
    beqz s0, BuildObjEnd            // no sprites: OBJBUF isn't read (DrawScreen checks F_SPRITES)
    move s2, ra
    lhu t0, %lo(LINE)+LR_SLIVSTART(zero)
    lw t1, %lo(JOB_SLIVERS)(zero)
    sll t0, 3
    addu t0, t1
    li t1, %lo(P01BUF)
    sll t2, s0, 3
    jal DmaIn
    addiu t2, -1
    jal DmaWait                     # the slivers, and the previous line's output out of OBJBUF
    nop
    # backdrop (with the colour math flag: the sprite pass copies it to the main screen)
    lbu t0, %lo(LINE)+LR_MATHEN(zero)
    andi t0, 0x20
    sll t0, 6
    ori t0, 0x8500
    mtc2 t0, $v01.e0
    # sprites only on the main screen, without a window there (drawMask bit 7,
    # worked out by the CPU): built straight into MAINBUF, which then needs no
    # clear and no copy (SCR+36 tells DrawScreen)
    lb t1, %lo(LINE)+LR_DRAWMASK(zero)
    bltz t1, 1f
    li a0, %lo(MAINBUF)
    li a0, %lo(OBJBUF)
1:  slt t1, t1, zero
    sb t1, %lo(SCR)+36(zero)
    jal ClearBufV
    vor $v01, vzero, $v01.e0
    li s1, %lo(P01BUF)
    lbu t9, %lo(LINE)+LR_MATHEN(zero)
    andi t9, 0x10
    sll t9, 7                       # 0x0800 if OBJ has colour math
SliverLoop:
    # DecodeRow4 inline, so the scalar work pairs with the vector work
    lsv $v01.e0, 6, s1              # planes 0/1 (b1 << 8 | b0)
    lsv $v01.e1, 4, s1              # planes 2/3
    lh t0, 0(s1)                    # x
    lhu t1, 2(s1)                   # z
    sll t0, 1
    vmudn $v03, vpown, $v01.e0
    xori t1, 0x8000
    vmudn $v04, vpown, $v01.e1
    srl t2, t1, 8
    andi t2, 7
    vand $v05, $v03, vk1.e0
    xori t2, 4
    vand $v06, $v03, vk1.e1
    bnez t2, 1f                     # palettes 0-3 (layer 6): no colour math
    addu t0, a0                     # &buf[x + 8] - 16
    or t1, t9
1:  vand $v07, $v04, vk1.e0
    mtc2 t1, $v09.e0
    vand $v08, $v04, vk1.e1
    lqv $v14, 0x10, t0
    vmudl $v02, $v05, vk1.e2        # (no other vector op inside the MAC chain)
    lrv $v14, 0x20, t0
    vmadl $v02, $v06, vk1.e3
    addiu s0, -1
    vmadl $v02, $v07, vk1.e4
    vmadl $v02, $v08, vk1.e5        # pixels
    vand $v13, $v14, vk1.e6
    vaddc $v09, $v02, $v09.e0       # z + pixel
    vne $v12, $v02, vzero           # pixel != 0
    vmrg $v09, $v09, $v14           # ... ? z+pixel : dst
    veq $v13, $v13, vzero           # dst still empty
    vmrg $v14, $v09, $v14
    sqv $v14, 0x10, t0
    srv $v14, 0x20, t0
    bnez s0, SliverLoop
    addiu s1, 8
BuildObjEnd:
    jr s2
    nop

##############################################################################
# DrawScreen: a0 = MAINBUF/SUBBUF, a1 = 0 main / 1 sub.
# Clears the buffer, then sprites (copied through the OBJ window) and BG1..3.
##############################################################################
DrawScreen:
    move s4, ra
    lbu t0, %lo(SCR)+36(zero)       # BuildObj made this (main) screen's sprite line in place
    bnez t0, 3f
    sb zero, %lo(SCR)+36(zero)
    # backdrop; bit 0x0800 marks pixels whose layer has colour math (see Compose)
    lbu t0, %lo(LINE)+LR_MATHEN(zero)
    andi t0, 0x20
    sll t0, 6
    ori t0, 0x8500
    mtc2 t0, $v01.e0
    jal ClearBufV
    vor $v01, vzero, $v01.e0
    j 1f
    nop
3:  move s0, a0                     # sprites in place: on to the BGs
    j 2f
    lbu s1, %lo(LINE)+LR_SCREN(a1)
DrawScreenNoClear:
    move s4, ra
1:  move s0, a0                     # s0 = buffer
    lbu s1, %lo(LINE)+LR_SCREN(a1)  # s1 = screenEnabled bits
    # sprites
    lbu t1, %lo(LINE)+LR_FLAGS(zero)
    andi t1, F_SPRITES
    beqz t1, 2f
    andi t1, s1, 0x10
    beqz t1, 2f
    li a2, 4
    jal WinSetup
    nop
    li t0, 1
    beq t8, t0, 2f                  # sprites fully masked
    li t2, %lo(OBJBUF) + 16
    addiu t3, s0, 16
    li t4, 32
    move t1, zero                   # screen x of the group
6:  lqv $v01, 0, t2
    beqz t8, 7f
    lqv $v14, 0, t3
    mtc2 t1, vxpos.e0
    vaddc vxpos, vlane, vxpos.e0
    jal WinMask
    nop
    vmrg $v01, $v01, $v14           # allowed ? obj : backdrop
7:  sqv $v01, 0, t3
    addiu t1, 8
    addiu t2, 16
    addiu t4, -1
    bnez t4, 6b
    addiu t3, 16
2:  # BG1, BG2 (4bpp), BG3 (2bpp)
    andi t0, s1, 1
    beqz t0, 3f
    li a2, 0
    jal DrawBg
    nop
3:  andi t0, s1, 2
    beqz t0, 4f
    li a2, 1
    jal DrawBg
    nop
4:  andi t0, s1, 4
    beqz t0, 5f
    li a2, 2
    jal DrawBg
    nop
5:  jr s4
    nop

##############################################################################
# DrawBg: layer a2 (0,1: 4bpp, 2: 2bpp), screen a1, buffer s0.
# The CPU guarantees a tile block for every enabled layer of an RSP line.
# Without a window the tiles go through BlkFast4/BlkFast2: two tiles per
# iteration, no branches, ordered so the RSP can pair scalar and vector
# instructions and hide latencies. They draw 34 tiles (the 34th lands in the
# buffer padding). Windowed layers (rare) use BlkWin.
##############################################################################
DrawBg:
    sw ra, %lo(SCR)+60(zero)
    jal DmaWait                     # this draw's data (prefetched)
    nop
    lhu t0, %lo(SCR)+40(zero)       # set holding it
    li t9, %lo(BGSET0)
    addiu t9, %lo(BGSET1)
    subu t9, t0                     # the other set
    sh t9, %lo(SCR)+40(zero)
    sw t0, %lo(SCR)+32(zero)
    # prefetch the next draw (index 3 * screen + layer) into the other set
    sll t1, a1, 1
    addu t1, a1
    addu t1, a2
    addiu t1, 1
    lbu t2, %lo(SCR)+44(zero)
    srlv t2, t2, t1
    beqz t2, 3f
    nop
1:  andi t3, t2, 1
    bnez t3, 2f
    srl t2, 1
    j 1b
    addiu t1, 1
2:  sltiu t3, t1, 3
    bnez t3, 4f
    move a3, t1
    addiu a3, t1, -3
4:  jal PrefetchLayer
    move v1, t9
3:  jal WinSetup
    nop
    li t0, 1
    beq t8, t0, DrawBgEnd           # layer fully masked
    lbu t0, %lo(LINE)+LR_FLAGS(zero)
    andi t0, F_MODE7
    bnez t0, BlkM7
    sll t0, a2, 3
    sll t1, a2, 1
    addu s2, t0, t1                 # s2 = 10 * layer
    lhu t7, %lo(LINE)+LR_BG+4(s2)   # ta0: row = ta0 & 7
    lw k1, %lo(SCR)+32(zero)        # this draw's set
1:  lhu t6, %lo(LINE)+LR_BG+8(s2)   # hscroll
    andi t7, 3
    sll t7, 1                       # row within the quarter
    andi t6, 7                      # fine x
    subu t1, zero, t6               # screen x of the first tile's pixel 0
    sll t2, t1, 1
    addu v0, s0, t2
    addiu v0, 16                    # &buf[x + 8]
    addiu fp, k1, SET_P01
    addu fp, t7                     # planes 0/1 of this row, tile 0
    addiu t6, fp, SET_P23 - SET_P01 # planes 2/3
    lhu t0, %lo(LINE)+LR_BG+8(s2)
    srl t0, 2
    andi t0, 6                      # z of the first column: (2 * column) & 7 into the DMA'd z
    addu t5, k1, t0
    # main screen: mark this layer's pixels if it has colour math
    bnez a1, 4f
    lbu t0, %lo(LINE)+LR_MATHEN(zero)
    srlv t0, t0, a2
    andi t0, 1
    beqz t0, 4f
    lqv $v01, 0x00, k1              # (t1 = screen x, needed by BlkWin)
    lqv $v02, 0x10, k1
    lqv $v03, 0x20, k1
    vor $v01, $v01, vk1.e4
    lqv $v04, 0x30, k1
    vor $v02, $v02, vk1.e4
    lqv $v05, 0x40, k1
    vor $v03, $v03, vk1.e4
    sqv $v01, 0x00, k1
    vor $v04, $v04, vk1.e4
    sqv $v02, 0x10, k1
    vor $v05, $v05, vk1.e4
    sqv $v03, 0x20, k1
    sqv $v04, 0x30, k1
    sqv $v05, 0x40, k1
4:  li t0, %lo(V_NIB)
    lqv vnib, 0, t0
    bnez t8, BlkWin
    li t3, 2
    beq a2, t3, BlkFast2
    nop
    # ---- 4bpp, 2 tiles per iteration (A, B), software-pipelined: iteration k
    # merges the pixels of pair k - 1 into the buffer and decodes pair k. The
    # first iteration merges a fully transparent pair -1, which writes back
    # the 32 bytes before the buffer unchanged. 4bpp tiles hold one nibble per
    # pixel (the CPU converted them): fp has pixels 0-3 of the row, t6 pixels
    # 4-7, loaded into lanes 0 and 4; vmudn by V_NIB (.h0) moves each lane's
    # nibble to the top, vmudl >> 12 gives the pixel. VCC order: A's vne,
    # vmrg, then B's, then the two vge. Vector results are read 4 groups later. ----
    lsv $v01.e0, 0, fp
    lsv $v01.e4, 0, t6
    lsv $v10.e0, 8, fp
    lsv $v10.e4, 8, t6
    vxor $v09, $v09, $v09
    vxor $v15, $v15, $v15
    addiu v0, -32
    addiu t5, -4
    li gp, 18
BlkFast4:
    vaddc $v21, $v09, $v02.e0       # pair k-1 A: z + pixel
    lqv $v14, 0x00, v0              # dst A
    vne $v26, $v09, vzero           # A opaque
    lrv $v14, 0x10, v0
    vaddc $v31, $v15, $v11.e0       # pair k-1 B: z + pixel
    lqv $v29, 0x10, v0              # dst B
    vmudn $v03, vnib, $v01.h0       # pair k A: nibbles to the top
    lrv $v29, 0x20, v0
    vmrg $v21, $v21, vk1.e1         # A transparent -> MIN
    lsv $v02.e0, 4, t5              # pair k's z
    vne $v26, $v15, vzero           # B opaque
    lsv $v11.e0, 6, t5
    vmrg $v31, $v31, vk1.e1         # B transparent -> MIN
    lsv $v01.e0, 16, fp             # pair k+1's pixels
    vmudn $v12, vnib, $v10.h0       # pair k B
    lsv $v01.e4, 16, t6
    vge $v14, $v14, $v21            # max(dst, candidate) (compares set VCC: after both vmrg)
    lsv $v10.e0, 24, fp
    vmudl $v09, $v03, vk1.e5        # pixels A of pair k (>> 12)
    lsv $v10.e4, 24, t6
    vge $v29, $v29, $v31
    addiu t5, 4
    vmudl $v15, $v12, vk1.e5        # pixels B
    addiu fp, 16
    addiu t6, 16
    sqv $v14, 0x00, v0
    srv $v14, 0x10, v0
    sqv $v29, 0x10, v0
    srv $v29, 0x20, v0
    addiu gp, -1
    bnez gp, BlkFast4
    addiu v0, 32
    j DrawBgEnd
    nop

    # ---- 2bpp, same scheme ----
BlkFast2:
    lsv $v01.e0, 0, fp
    lsv $v10.e0, 8, fp
    vxor $v09, $v09, $v09
    vxor $v15, $v15, $v15
    addiu v0, -32
    addiu t5, -4
    li gp, 18
1:  vaddc $v21, $v09, $v02.e0
    lqv $v14, 0x00, v0
    vne $v26, $v09, vzero
    lrv $v14, 0x10, v0
    vmudn $v03, vpown, $v01.e0
    lqv $v29, 0x10, v0
    vmudn $v12, vpown, $v10.e0
    lrv $v29, 0x20, v0
    vmrg $v21, $v21, vk1.e1
    lsv $v01.e0, 16, fp
    vaddc $v31, $v15, $v11.e0
    lsv $v10.e0, 24, fp
    vne $v26, $v15, vzero
    addiu fp, 16
    vand $v05, $v03, vk1.e0
    lsv $v02.e0, 4, t5
    vand $v06, $v03, vk1.e1
    lsv $v11.e0, 6, t5
    vmrg $v31, $v31, vk1.e1
    addiu t5, 4
    vge $v14, $v14, $v21
    addiu gp, -1
    vand $v16, $v12, vk1.e0
    vand $v17, $v12, vk1.e1
    vge $v29, $v29, $v31
    vmudl $v09, $v05, vk1.e2        # (VU ops write the accumulator: keep MAC chains together)
    vmadl $v09, $v06, vk1.e3
    sqv $v14, 0x00, v0
    vmudl $v15, $v16, vk1.e2
    srv $v14, 0x10, v0
    vmadl $v15, $v17, vk1.e3
    sqv $v29, 0x10, v0
    srv $v29, 0x20, v0
    bnez gp, 1b
    addiu v0, 32
    j DrawBgEnd
    nop

    # ---- windowed layer: one tile at a time with the window mask ----
BlkWin:
    li gp, 33
2:  bne a2, t3, 3f
    lsv $v01.e0, 0, fp
    mtc2 zero, $v01.e1              # 2bpp: planes 0/1 only
    jal DecodeRow4
    nop
    j 4f
    nop
3:  lsv $v01.e4, 0, t6              # 4bpp: nibbles (see BlkFast4)
    vmudn $v03, vnib, $v01.h0
    vmudl $v02, $v03, vk1.e5
4:
    lsv $v09.e0, 0, t5
    vaddc $v09, $v02, $v09.e0       # z + pixel
    vne $v15, $v02, vzero
    vmrg $v09, $v09, vk1.e1         # transparent -> MIN
    mtc2 t1, vxpos.e0
    vaddc vxpos, vlane, vxpos.e0
    jal WinMask
    nop
    vmrg $v09, $v09, vk1.e1         # masked -> MIN
    lqv $v14, 0x00, v0
    lrv $v14, 0x10, v0
    vge $v14, $v14, $v09
    sqv $v14, 0x00, v0
    srv $v14, 0x10, v0
    addiu t5, 2
    addiu fp, 8
    addiu t6, 8
    addiu t1, 8
    addiu gp, -1
    bnez gp, 2b
    addiu v0, 16
    # done (this used to fall through into BlkM7, which drew the row's tile
    # bytes again as mode 7 pixels: garbage on windowed layers, e.g. around
    # the security eyes' beams). The delay slot is BlkM7's first instruction:
    # harmless here, and one instruction is all IMEM has left.
    j DrawBgEnd
    # ---- mode 7: z = 0x5000 + colour index (0 transparent), one group of 8 at a time ----
BlkM7:
    lw t5, %lo(SCR)+32(zero)        # this draw's set (also BlkWin's delay slot)
    addiu t5, SET_P01               # the row
    li t0, 0x5000 ^ 0x8000
    bnez a1, 1f                     # (math flag only on the main screen)
    lbu t1, %lo(LINE)+LR_MATHEN(zero)
    andi t1, 1
    sll t1, 11
    or t0, t1
1:  mtc2 t0, $v02.e0
    addiu v0, s0, 16
    move t1, zero                   # screen x of the group
    li gp, 32
2:  luv $v03, 0, t5
    lqv $v14, 0, v0
    vmudl $v03, $v03, vk1.e2        # byte << 7 -> byte
    vaddc $v04, $v03, $v02.e0
    vne $v05, $v03, vzero
    vmrg $v04, $v04, vk1.e1         # transparent -> MIN
    beqz t8, 3f
    mtc2 t1, vxpos.e0
    vaddc vxpos, vlane, vxpos.e0
    jal WinMask
    nop
    vmrg $v04, $v04, vk1.e1         # masked -> MIN
3:  vge $v14, $v14, $v04
    sqv $v14, 0, v0
    addiu t5, 8
    addiu v0, 16
    addiu gp, -1
    bnez gp, 2b
    addiu t1, 8
DrawBgEnd:
    lw ra, %lo(SCR)+60(zero)
    jr ra
    nop

##############################################################################
# PrefetchLayer: start the DMAs of layer a3's tile row data into the set at
# DMEM v1 (z values, planes 0/1 and, for 4bpp, planes 2/3 of this line's row).
# Destroys t0..t3, t6, t7, k1.
##############################################################################
# PrefetchFirst: gp = line record, v1 = set. For a line the RSP draws with BG
# draws: SCR+46 = v1, and the first draw's DMAs start. Destroys t0..t3, t6,
# t7, k1, a3.
PrefetchFirst:
    lbu t0, LR_FLAGS(gp)
    andi t0, F_VALID|F_CPUDRAWN|F_FORCEDBLANK|F_COPY
    xori t0, F_VALID
    bnez t0, 9f
    lbu t0, LR_DRAWMASK(gp)
    andi t0, 0x3f
    beqz t0, 9f
    li a3, 0                        # first draw: its index (3 * screen + layer)
2:  andi t1, t0, 1
    bnez t1, 3f
    srl t0, 1
    j 2b
    addiu a3, 1
3:  sltiu t1, a3, 3
    bnez t1, 4f
    nop
    addiu a3, -3                    # -> layer
4:  j PrefetchLayerAt               # (returns to our caller)
    sh v1, %lo(SCR)+46(zero)
9:  jr ra
    nop

PrefetchLayer:
    li gp, %lo(LINE)
PrefetchLayerAt:                    # gp = line record
    lbu t0, LR_FLAGS(gp)
    andi t0, F_MODE7
    beqz t0, 1f
    lhu t0, LR_BLOCK(gp)
    sll t0, 8                       # mode 7: the row's 256 colour indices
    lw t1, %lo(JOB_M7)(zero)
    addu t0, t1
    addiu t1, v1, SET_P01
    j DmaIn                         # (returns to our caller)
    li t2, 256-1
1:  sw ra, %lo(SCR)+28(zero)
    sll t0, a3, 1
    addu t0, gp
    lhu t0, LR_BLOCK(t0)
    # block address = JOB_BLOCKS + t0 * 2248 (2248 = 2048 + 128 + 64 + 8)
    sll t1, t0, 11
    sll t2, t0, 7
    addu t1, t2
    sll t2, t0, 6
    addu t1, t2
    sll t2, t0, 3
    addu t1, t2
    lw t0, %lo(JOB_BLOCKS)(zero)
    addu k1, t0, t1
    sll t0, a3, 3
    sll t1, a3, 1
    addu t2, t0, t1
    addu t2, gp
    lhu t7, LR_BG+4(t2)             # ta0: row = ta0 & 7
    lhu t3, LR_BG+8(t2)             # hscroll: first column = (hscroll >> 3) & 63
    srl t3, 3
    andi t3, 63
    sw t3, %lo(SCR)+24(zero)
    sll t0, t3, 1
    andi t0, 0xfff8
    addu t0, k1                     # z values from that column (8-byte aligned DMA)
    move t1, v1
    jal DmaIn
    li t2, 80-1
    andi t6, t7, 4                  # rows 0-3 or 4-7: which 8-byte quarter of each tile
    sll t6, 1
    addu k1, t6
    addiu k1, BLOCK_TILES           # quarter of column 0's planes 0/1
    # planes 0/1 (then 2/3 for 4bpp) of 34 columns, in two parts if they wrap past column 63
    addiu t1, v1, SET_P01
    jal PrefetchPlanes
    nop
    li t3, 2
    beq a3, t3, 1f
    addiu k1, 16                    # planes 2/3
    jal PrefetchPlanes
    addiu t1, v1, SET_P23
1:  lw ra, %lo(SCR)+28(zero)
    jr ra
    nop

# PrefetchPlanes: 34 x 8 bytes (stride 32) from column SCR+24 on, wrapping at
# 64, from k1 (column 0) to DMEM t1. Destroys t0, t2, t3, t4, k0.
PrefetchPlanes:
    move k0, ra
    lw t3, %lo(SCR)+24(zero)        # first column
    li t4, 64
    subu t4, t3                     # columns before the wrap
    sltiu t0, t4, 34
    bnez t0, 2f
    sll t0, t3, 5
    addu t0, k1
    li t2, (8-1) | ((34-1) << 12) | (24 << 20)
    jal DmaIn
    move ra, k0
2:  addiu t2, t4, -1
    sll t2, 12
    li t3, (8-1) | (24 << 20)
    or t2, t3
    jal DmaIn
    addu t0, k1
    sll t0, t4, 3
    addu t1, t0                     # rest from column 0
    li t2, 34
    subu t2, t4
    addiu t2, -1
    sll t2, 12
    or t2, t3
    move t0, k1
    j DmaIn
    move ra, k0

##############################################################################
# Compose: MAINBUF (+SUBBUF) -> OUTBUF, 256 RGBA5551 pixels.
# Per pixel, same as RenderLine() in ppu_rsp.c.
#
# Palette lookup: the vector unit turns 8 z values into CGRAM addresses, then
# per pixel one mfc2 + one lsv puts the colour straight into its lane.
##############################################################################
# Compose: MAINBUF (+SUBBUF) -> OUTBUF, 256 RGBA5551 pixels, like RenderLine()
# in ppu_rsp.c. The colour window is constant along RSP lines (the CPU draws
# the others), so the clip and math decisions are made once per line.
#
# Per pixel: colour = CGRAM[z & 0xff] (the vector unit makes 8 addresses, then
# mfc2 + lsv put each colour into its lane). Colour math applies where the
# main pixel's z has bit 0x0800, set while drawing the main screen for layers
# with math enabled.
#
# The loop is software-pipelined: while the vector unit works on group i, the
# scalar unit gathers the colours of group i + 1.
##############################################################################
Compose:
    move s4, ra
    # kCB_WinMath: colour math only inside the colour window (bit 7: only
    # outside it), which changes along the line. Clear the math bit (0x0800)
    # of the main pixels where the window rules math out; the rest of the line
    # goes on as usual. Same windows as Windows_Calc in ppu_rsp.c (layer 5,
    # windowsel bits 20-23, the two windows OR'd).
    lb s2, %lo(LINE)+LR_CBITS(zero)     # (sign: bit 7)
    andi t0, s2, 64
    beqz t0, ComposeLine
    lw t4, %lo(LINE)+LR_WINSEL(zero)    # (also on the way past: harmless)
    jal WinSetupFlags
    srl t4, t4, 20                      # the colour window's flags
    vxor $v15, vones, vk1.e4            # mask outside the window: math bit cleared
    bgez s2, 1f
    vor $v16, vzero, vones              # inside: kept
    vor $v16, vzero, $v15               # math only outside: the other way round
    vor $v15, vzero, vones
1:  li a0, %lo(MAINBUF) + 16
    li a3, 32                           # groups of 8 pixels
    vor vxpos, vzero, vlane             # x of the group's lanes
2:  jal WinMask                         # VCC = outside the window
    lqv $v01, 0, a0
    vmrg $v02, $v15, $v16
    vand $v01, $v01, $v02
    vaddc vxpos, vxpos, vk2.e4          # + 8
    sqv $v01, 0, a0
    addiu a3, -1
    bnez a3, 2b
    addiu a0, 16
ComposeLine:
    # per-line constants
    lbu v1, %lo(LINE)+LR_BRIGHT(zero)
    lhu t9, %lo(LINE)+LR_FIXED(zero)
    andi t1, t9, 0x1f
    mtc2 t1, vline.e1
    srl t1, t9, 5
    andi t1, 0x1f
    mtc2 t1, vline.e2
    srl t1, t9, 10
    andi t1, 0x1f
    mtc2 t1, vline.e3
    xori v1, 15                     # v1 = 0 when brightness is 15 (no scaling needed)
    # vcm lanes from the compose bits (worked out by the CPU): e0 keep mask,
    # e1 sub screen usable, e2 half, e3 half even without sub, e4 +1 add / -1 subtract
    lbu t0, %lo(LINE)+LR_CBITS(zero)
    mtc2 t0, vcm.e0
    andi s3, t0, 32                 # s3 = line does colour math
    andi t0, 16
    beqz t0, 1f
    li t1, 1
    li t1, -1
1:  vand vcm, vpown, vcm.e0
    vne vcm, vcm, vzero
    vmrg vcm, vones, vzero
    mtc2 t1, vcm.e4
    lbu t1, %lo(LINE)+LR_BRIGHT(zero)
    sll t1, 1
    lhu t1, %lo(V_BMUL)(t1)
    mtc2 t1, vcm.e5                 # brightness multiplier for doubled channels
    li a0, %lo(MAINBUF) + 16
    li a1, %lo(SUBBUF) + 16
    li a2, %lo(OUTBUF)
    li a3, 32                       # groups
    # prologue: colours of group 0
    lqv $v19, 0, a0
    lqv $v18, 0, a1
    vand $v29, $v19, vk1.e6
    vand $v10, $v18, vk1.e6
    vmudn $v29, $v29, vk3.e1        # * 2
    vmudn $v10, $v10, vk3.e1
    vaddc $v29, $v29, vk4.e2        # + CGRAM
    vaddc $v10, $v10, vk4.e2
    jal GatherMain
    mfc2 t0, $v29.e0
    or t0, s3, v1
    beqz t0, CompSimple
    lqv $v17, 0x10, a0
    # everything else (the CPU draws the rare lines MathLoop can't: halving,
    # subtraction during a brightness fade)
    j MathSetup
    nop
# Lines without colour math at full brightness: out = keep ? colour : black.
# $v14 = colours of group i, $v29 = addresses of group i + 1.
CompSimple:
    vand $v29, $v17, vk1.e6
    vmudn $v29, $v29, vk3.e1
    vaddc $v29, $v29, vk4.e2
    vand $v12, vone1, vcm.e0
    vxor $v12, $v12, vone1          # 1 where clipped: OR'd in after the AND
    vor $v13, vcm, vone1            # keep mask with the alpha bit
    li v1, %lo(SCR)
SimpLoop:
    sqv $v29, 0, v1                 # addresses of group i + 1 (see MathLoop)
    vand $v03, $v14, $v13.e0
    lqv $v19, 0x20, a0              # z of group i + 2
    lhu t0, %lo(SCR)+0(zero)
    lhu t2, %lo(SCR)+2(zero)
    lhu t3, %lo(SCR)+4(zero)
    vor $v03, $v03, $v12
    lhu t4, %lo(SCR)+6(zero)
    vand $v29, $v19, vk1.e6
    lsv $v14.e0, 0, t0
    lhu t5, %lo(SCR)+8(zero)
    lsv $v14.e1, 0, t2
    lhu t6, %lo(SCR)+10(zero)
    vmudn $v29, $v29, vk3.e1
    lsv $v14.e2, 0, t3
    lhu t7, %lo(SCR)+12(zero)
    lsv $v14.e3, 0, t4
    lhu k1, %lo(SCR)+14(zero)
    vaddc $v29, $v29, vk4.e2
    lsv $v14.e4, 0, t5
    lsv $v14.e5, 0, t6
    lsv $v14.e6, 0, t7
    lsv $v14.e7, 0, k1
    addiu a0, 16
    addiu a3, -1
    sqv $v03, 0, a2
    bnez a3, SimpLoop
    addiu a2, 16
    jr s4
    nop
# Colour math lines at full brightness without halving (almost all of them).
# With add, out = min(main + second, 31) per channel. With subtract the loop
# works on complemented values: max(main - second, 0) = 31 - min((31 - main) +
# second, 31). $v31 is XOR'd into the main colour (0xffff to complement, 1
# otherwise, which only flips the unused alpha bit) and is also the base of the
# packing sum: 65535 - (r << 11 | g << 6 | b << 1) undoes the complement.
# second = sub colour where the sub pixel is opaque (s0 = sub screen usable),
# otherwise the fixed colour; 0 where the main pixel's layer has no math.
# Per group i: $v14/$v15 = main/sub colours, $v29/$v10 = addresses of group i+1.
MathSetup:
    lbu t0, %lo(LINE)+LR_CBITS(zero)
    bnez s3, 1f
    andi s0, t0, 2
    move s0, zero                   # no math on this line: second colour 0
    move t0, zero
    vxor vline, vline, vline
1:  vaddc vline, vline, vline       # fixed colour doubled, like all channels here
    andi t0, 16
    li t2, 2048
    li t3, 64
    li t4, 2
    beqz t0, 2f
    vor $v31, vzero, vone1
    subu t2, zero, t2
    subu t3, zero, t3
    subu t4, zero, t4
    vor $v31, vzero, vones
2:  mtc2 t2, vline.e4
    mtc2 t3, vline.e5
    mtc2 t4, vline.e6
    li t2, 1
    mtc2 t2, vline.e7
    vor $v11, vzero, vline.e1       # second colour without a sub screen
    vor $v12, vzero, vline.e2
    vor $v13, vzero, vline.e3
    # fixed colour packed like CGRAM (vline.e0), chosen where the sub pixel is transparent
    andi t1, t9, 0x1f
    sll t1, 11
    andi t2, t9, 0x3e0
    sll t2, 1
    or t1, t2
    srl t2, t9, 9
    andi t2, 0x3e
    or t1, t2
    mtc2 t1, vline.e0
    jal GatherSub
    lqv $v18, 0x10, a1
    vand $v29, $v17, vk1.e6         # addresses of group 1
    vand $v10, $v18, vk1.e6
    vmudn $v29, $v29, vk3.e1
    vmudn $v10, $v10, vk3.e1
    vaddc $v29, $v29, vk4.e2
    vaddc $v10, $v10, vk4.e2
    addiu a2, -16                   # the output of group i is stored in iteration i + 1
    li v1, %lo(SCR)
# Iteration i: add, clamp, scale and pack group i - 1 (the first time: garbage,
# stored in the pad before OUTBUF); main colour of group i; gather the colours
# of group i + 1; addresses of group i + 2. SU and VU instructions alternate.
MathLoop:
    # (SU and VU instructions pair up; mfc2 counts as a load and a store for the
    # pipeline hazards, so addresses go through DMEM: sqv + lhu)
    sqv $v29, 0, v1                 # addresses of group i + 1
    vaddc $v03, $v03, $v07          # group i - 1: + second colour
    lqv $v16, 0, a0                 # main z(i)
    vaddc $v04, $v04, $v08
    lhu t0, %lo(SCR)+0(zero)
    vaddc $v05, $v05, $v09
    lhu t2, %lo(SCR)+2(zero)
    vand $v01, $v14, vcm.e0         # group i: keep (colour window)
    lhu t3, %lo(SCR)+4(zero)
    vlt $v03, $v03, vk4.e5          # clamp (2 * 31)
    lhu t4, %lo(SCR)+6(zero)
    vlt $v04, $v04, vk4.e5
    lhu t5, %lo(SCR)+8(zero)
    vlt $v05, $v05, vk4.e5
    lhu t6, %lo(SCR)+10(zero)
    vxor $v01, $v01, $v31
    lhu t7, %lo(SCR)+12(zero)
    vmudl $v03, $v03, vcm.e5        # brightness, back to 0..31
    lhu k1, %lo(SCR)+14(zero)
    vmudl $v04, $v04, vcm.e5
    lqv $v19, 0x20, a0              # main z(i+2)
    vmudl $v05, $v05, vcm.e5
    lqv $v17, 0, a1                 # sub z(i)
    vand $v16, $v16, vk1.e4         # main layer has math?
    lqv $v18, 0x20, a1              # sub z(i+2)
    vmudn $v02, $v31, vline.e7      # pack group i - 1
    vmadn $v02, $v03, vline.e4
    vmadn $v02, $v04, vline.e5
    beqz s0, MathNoSub
    vmadn $v02, $v05, vline.e6
    # sub screen: second colour = sub colour where opaque, else fixed
    sqv $v10, 16, v1
    vand $v06, $v17, vk1.e6
    lhu t8, %lo(SCR)+16(zero)
    lhu t9, %lo(SCR)+18(zero)
    lhu s1, %lo(SCR)+20(zero)
    lhu s2, %lo(SCR)+22(zero)
    vne $v06, $v06, vzero
    lhu k0, %lo(SCR)+24(zero)
    vmrg $v07, $v15, vline.e0
    lhu v0, %lo(SCR)+26(zero)
    vand $v10, $v18, vk1.e6
    lhu $28, %lo(SCR)+28(zero)
    lhu $30, %lo(SCR)+30(zero)
    lsv $v15.e0, 0, t8
    vmudl $v11, $v07, vk3.e0        # r * 2
    lsv $v15.e1, 0, t9
    vmudl $v12, $v07, vk3.e2        # g * 2
    lsv $v15.e2, 0, s1
    vand $v13, $v07, vk4.e5         # b * 2
    lsv $v15.e3, 0, s2
    vmudn $v10, $v10, vk3.e1
    lsv $v15.e4, 0, k0
    vand $v11, $v11, vk4.e5
    lsv $v15.e5, 0, v0
    vand $v12, $v12, vk4.e5
    lsv $v15.e6, 0, $28
    lsv $v15.e7, 0, $30
    vaddc $v10, $v10, vk4.e2
MathNoSub:
    lsv $v14.e0, 0, t0              # main colours of group i + 1
    vand $v29, $v19, vk1.e6
    lsv $v14.e1, 0, t2
    veq $v16, $v16, vzero
    lsv $v14.e2, 0, t3
    vmudl $v03, $v01, vk3.e0        # r * 2
    lsv $v14.e3, 0, t4
    vmudl $v04, $v01, vk3.e2        # g * 2
    lsv $v14.e4, 0, t5
    vmrg $v07, vzero, $v11          # second colour, 0 where no math
    lsv $v14.e5, 0, t6
    vmudn $v29, $v29, vk3.e1
    lsv $v14.e6, 0, t7
    vmrg $v08, vzero, $v12
    lsv $v14.e7, 0, k1
    vmrg $v09, vzero, $v13
    addiu a0, 16
    vand $v05, $v01, vk4.e5         # b * 2
    addiu a1, 16
    vaddc $v29, $v29, vk4.e2
    sqv $v02, 0, a2
    vand $v03, $v03, vk4.e5
    addiu a3, -1
    vand $v04, $v04, vk4.e5
    bnez a3, MathLoop
    addiu a2, 16
    vaddc $v03, $v03, $v07          # last group
    vaddc $v04, $v04, $v08
    vaddc $v05, $v05, $v09
    vlt $v03, $v03, vk4.e5
    vlt $v04, $v04, vk4.e5
    vlt $v05, $v05, vk4.e5
    vmudl $v03, $v03, vcm.e5
    vmudl $v04, $v04, vcm.e5
    vmudl $v05, $v05, vcm.e5
    vmudn $v02, $v31, vline.e7
    vmadn $v02, $v03, vline.e4
    vmadn $v02, $v04, vline.e5
    vmadn $v02, $v05, vline.e6
    jr s4
    sqv $v02, 0, a2

# GatherMain: $v14 = CGRAM colours at the addresses in $v29 (t0 = lane 0 address
# already loaded). GatherSub: $v15 from $v10. Destroy t0, t2..t7, k1.
GatherMain:
    mfc2 t2, $v29.e1
    mfc2 t3, $v29.e2
    mfc2 t4, $v29.e3
    lsv $v14.e0, 0, t0
    mfc2 t5, $v29.e4
    lsv $v14.e1, 0, t2
    mfc2 t6, $v29.e5
    lsv $v14.e2, 0, t3
    mfc2 t7, $v29.e6
    lsv $v14.e3, 0, t4
    mfc2 k1, $v29.e7
    lsv $v14.e4, 0, t5
    lsv $v14.e5, 0, t6
    lsv $v14.e6, 0, t7
    jr ra
    lsv $v14.e7, 0, k1
GatherSub:
    mfc2 t0, $v10.e0
    mfc2 t2, $v10.e1
    mfc2 t3, $v10.e2
    mfc2 t4, $v10.e3
    lsv $v15.e0, 0, t0
    mfc2 t5, $v10.e4
    lsv $v15.e1, 0, t2
    mfc2 t6, $v10.e5
    lsv $v15.e2, 0, t3
    mfc2 t7, $v10.e6
    lsv $v15.e3, 0, t4
    mfc2 k1, $v10.e7
    lsv $v15.e4, 0, t5
    lsv $v15.e5, 0, t6
    lsv $v15.e6, 0, t7
    jr ra
    lsv $v15.e7, 0, k1


#ifdef PPU_DEBUG_DUMP
##############################################################################
# DebugDump: OBJBUF, MAINBUF, SUBBUF and rendered_sub to JOB_DBG + line * 0x700.
##############################################################################
DebugDump:
    move s2, ra
    lw t0, %lo(JOB_DBG)(zero)
    sll t1, s5, 11
    sll t2, s5, 8
    subu t1, t2                     # line * 0x700
    addu s0, t0, t1
    move t0, s0
    li t1, %lo(OBJBUF)
    jal DmaOut
    li t2, 544-1
    addiu t0, s0, 0x220
    li t1, %lo(MAINBUF)
    jal DmaOut
    li t2, 544-1
    addiu t0, s0, 0x440
    li t1, %lo(SUBBUF)
    jal DmaOut
    li t2, 544-1
    sw s3, %lo(SCR)+48(zero)
    addiu t0, s0, 0x660
    li t1, %lo(SCR) + 48
    jal DmaOut
    li t2, 16-1
    jal DmaWait
    nop
    jr s2
    nop
#endif
