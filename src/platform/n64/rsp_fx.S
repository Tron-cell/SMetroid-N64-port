##############################################################################
# rsp_fx.S - "2.5D" look for the N64 RSP: bevelled level blocks and drop shadows.
#
# Runs after rsp_ppu.S has drawn a frame. For each drawn row, rsp_ppu.S also
# writes the z values of its pixels (main screen; the sub screen where the
# main one only shows BG3 or the backdrop) to the mask image
# (JOB_MASK + row * 512). From those, for every line flagged in JOB_FXBITS,
# this pass reads the framebuffer line and changes:
#   * BG1 (level block) pixels on a block's edge: half the colour; on the top
#     or left edge (and not also the bottom or right edge) plus 12 per
#     channel, so blocks look lit from the top left.
#   * Background pixels (BG2, BG3, backdrop) below-right of BG1 or sprite
#     pixels: a soft shadow, 0.22 darker for each of the taps (x-1, y-1),
#     (x-2, y-2), (x-3, y-3) that is covered.
# Mask rows of lines not flagged in JOB_VALIDBITS (HUD, forced blank, mode 7,
# CPU-drawn, past the picture) and the columns left and right of the picture
# count as "outside": no shadow, and no edge next to them.
#
# Mask rows are converted to one word per pixel:
#   background 0x0001, sprite 0x3800, BG1 0x3802, outside 0x0002
# so that with C = centre, U/D/L/R = neighbours, A/B/X = shadow taps:
#   edge  = C & 2 & ~(U & L & D & R)        light = C & 2 & D & R & ~(U & L)
#   taps  = (A & 0x3800) + (B & 0x3800) + (X & 0x3800)
#   amount = C & 1 ? taps : edge ? 0x8000 : 0
# and each channel c becomes c - floor(c * amount / 65536) (+ 12 if light).
# No compares in the pixel loop.
#
# Checked pixel for pixel against a C model (rsptest/fx/fxhost.c).
#
# Standalone microcode (no rspq): the CPU fills JOB in DMEM, runs from PC 0,
# and waits for the break.
##############################################################################
#include <rsp.inc>

    .set noat

    .data
    .align 4
# ---- Job, written by the CPU before each run (96 bytes) ----
JOB_FB:       .long 0          # framebuffer address of line 0 (RGBA5551)
JOB_STRIDE:   .long 0          # framebuffer stride in bytes (unused: see JOB_SH)
JOB_MASK:     .long 0          # mask image (512 bytes per line)
JOB_NLINES:   .half 0
JOB_SH:       .byte 0, 0       # stride = (1 << JOB_SH[0]) + (1 << JOB_SH[1])
              .long 0, 0, 0, 0
JOB_FXBITS:   .long 0, 0, 0, 0, 0, 0, 0, 0     # bit (y & 31) of word y >> 5: apply the effect to line y
JOB_VALIDBITS:.long 0, 0, 0, 0, 0, 0, 0, 0     # same layout: line y's mask row is valid

    .align 4
# pixel loop: e0 shadow tap, e1 background bit, e2 BG1 bit, e3 edge amount / 2,
#             e4 light add / 2, e5..e7 r / g / b fields
V_K:     .half 0x3800, 0x0001, 0x0002, 0x4000, 0x318c, 0xf800, 0x07c0, 0x003e
# conversion: e0 >>8 (vmudl), e1 7, e2 5, e3 4
V_C:     .half 0x0100, 7, 5, 4, 0, 0, 0, 0
V_BG1:   .half 0x3802, 0x3802, 0x3802, 0x3802, 0x3802, 0x3802, 0x3802, 0x3802
V_OBJ:   .half 0x3800, 0x3800, 0x3800, 0x3800, 0x3800, 0x3800, 0x3800, 0x3800
V_BG:    .half 1, 1, 1, 1, 1, 1, 1, 1
V_OUT:   .half 2, 2, 2, 2, 2, 2, 2, 2

#define NSLOTS 5
TAGS:    .long 0x7fff0000, 0x7fff0000, 0x7fff0000, 0x7fff0000, 0x7fff0000   # mask row held by each slot
PTRS:    .long 0, 0, 0, 0, 0   # this line's rows y-3 .. y+1: pixel 0 in their slots

# Five converted mask rows (rows y-3 .. y+1), 16 bytes of padding each side.
    .align 4
ROWS:    .space NSLOTS * 544
#define ROW_SIZE 544
    .align 4
RAW:     .space 512            # a mask row as DMA'd in
    .align 4
FBL:     .space 512            # the framebuffer line
DUMMY:   .space 32             # the pixel loop's first store

#define vzero  $v00
#define vk     $v30
# (the pixel loop uses $v01..$v29 and $v31, so vc..vout are reloaded per line)
#define vc     $v29
#define vbg1   $v28
#define vobj   $v27
#define vbg    $v26
#define vout   $v25

# s6 = line count, s5 = line, s2 = row being prefetched into RAW (-1 none),
# s1 = next line to do, s7 = the previous line done, s3 / s0 = framebuffer
# address of this / the next line, s4 = half of the line being done

    .text
    .globl _start
_start:
    vxor vzero, vzero, vzero
    li t0, %lo(V_K)
    lqv vk,   0x00, t0
    lqv vout, 0x50, t0
    # padding of all slots: outside
    li t0, %lo(ROWS)
    li t1, NSLOTS
1:  sqv vout, 0, t0
    sqv vout, ROW_SIZE - 16, t0
    addiu t1, -1
    bnez t1, 1b
    addiu t0, ROW_SIZE

    lhu s6, %lo(JOB_NLINES)(zero)
    jal NextFx
    move a0, zero
    beq v0, s6, Done
    move s5, v0
    li s7, -100                     # no previous line
    jal PrefetchRaw
    move a0, s5
    jal PrefetchFb
    move a0, s5

LineLoop:
    jal DmaWait                     # this line's pixels and mask row, the previous line's output
    li t0, %lo(V_C)
    lqv vc,   0x00, t0              # (the pixel loop uses these registers)
    lqv vbg1, 0x10, t0
    lqv vobj, 0x20, t0
    lqv vbg,  0x30, t0
    lqv vout, 0x40, t0
    bltz s2, 1f
    nop
    jal Victim
    nop
    sw s2, 0(t4)                    # its tag
    move s0, v0                     # its pixel 0
    jal ConvRaw
    move a1, v0
    # the line after the previous one, and its row y+1 just arrived: the
    # rows are the previous line's shifted by one, plus the new one
    addiu t0, s7, 1
    bne t0, s5, 1f
    lw t0, %lo(PTRS)+4(zero)
    lw t1, %lo(PTRS)+8(zero)
    lw t2, %lo(PTRS)+12(zero)
    lw t3, %lo(PTRS)+16(zero)
    sw t0, %lo(PTRS)+0(zero)
    sw t1, %lo(PTRS)+4(zero)
    sw t2, %lo(PTRS)+8(zero)
    sw t3, %lo(PTRS)+12(zero)
    j 5f
    sw s0, %lo(PTRS)+16(zero)
1:
    # mask rows y-3 .. y+1 (normally all there already) -> PTRS
    jal MapRows
    nop
    li s0, 0
3:  lw t0, %lo(PTRS)(s0)
    bnez t0, 4f
    srl a0, s0, 2
    addu a0, s5
    jal LoadRow
    addiu a0, -3
    sw a1, %lo(PTRS)(s0)
4:  addiu s0, 4
    li t0, 20
    bne s0, t0, 3b
    nop
5:  move s7, s5                     # (for the next line's fast path)
    # the next line, and its new mask row
    jal NextFx
    addiu a0, s5, 1
    move s1, v0
    li s2, -1
    beq s1, s6, 2f
    nop
    jal PrefetchRaw
    move a0, s1
2:
    # pointers (pixel x of a row at its slot + 16 + 2x)
    lw v0, %lo(PTRS)+0(zero)
    addiu t5, v0, -6                # X: (x-3, y-3)
    lw v0, %lo(PTRS)+4(zero)
    addiu t9, v0, -4                # A: (x-2, y-2)
    lw v0, %lo(PTRS)+8(zero)
    addiu a1, v0, -2                # B: (x-1, y-1)
    move a2, v0                     # U: (x, y-1)
    lw v0, %lo(PTRS)+12(zero)
    addiu a3, v0, -2                # L: (x-1, y)
    move t6, v0                     # C: (x, y)
    addiu t7, v0, 2                 # R: (x+1, y)
    lw t8, %lo(PTRS)+16(zero)       # D: (x, y+1)
    move s4, t9
    jal LineAddr                    # this line's and the next one's framebuffer addresses
    move a0, s5
    move s3, t0
    jal LineAddr
    move a0, s1
    move s0, t0
    move a0, s4
    li t9, %lo(FBL)                 # pixels
    li v1, 8                        # pairs of 8-pixel groups: the left half first
    li s4, 0                        # half (0 left, 256 right)
    # the first pair's inputs; k0 = where the previous pair's pixels go (a dummy first)
    lqv $v17, 0x00, a2
    lqv $v18, 0x00, a3
    lrv $v18, 0x10, a3
    lqv $v19, 0x00, t8
    lqv $v20, 0x00, t7
    lrv $v20, 0x10, t7
    lqv $v25, 0x00, a0
    lrv $v25, 0x10, a0
    lqv $v26, 0x00, a1
    lrv $v26, 0x10, a1
    lqv $v29, 0x00, t5
    lrv $v29, 0x10, t5
    lqv $v21, 0x10, a2
    lqv $v22, 0x10, a3
    lrv $v22, 0x20, a3
    lqv $v23, 0x10, t8
    lqv $v24, 0x10, t7
    lrv $v24, 0x20, t7
    lqv $v27, 0x10, a0
    lrv $v27, 0x20, a0
    lqv $v28, 0x10, a1
    lrv $v28, 0x20, a1
    lqv $v31, 0x10, t5
    lrv $v31, 0x20, t5
    li k0, %lo(DUMMY)
PixLoop:
    sqv $v08, 0x00, k0
    vand $v03, $v17, $v18
    sqv $v16, 0x10, k0
    vand $v11, $v21, $v22
    move k0, t9
    vand $v07, $v19, $v20
    lqv $v05, 0x00, t6
    vand $v15, $v23, $v24
    lqv $v13, 0x10, t6
    vand $v01, $v25, vk.e0
    lqv $v08, 0x00, t9
    vand $v09, $v27, vk.e0
    lqv $v16, 0x10, t9
    vand $v02, $v26, vk.e0
    vand $v10, $v28, vk.e0
    vnand $v06, $v03, $v07
    vnand $v14, $v11, $v15
    vaddc $v01, $v01, $v02
    vaddc $v09, $v09, $v10
    vand $v02, $v29, vk.e0
    vand $v10, $v31, vk.e0
    vnand $v03, $v03, $v03
    vnand $v11, $v11, $v11
    lqv $v17, 0x20, a2
    vand $v04, $v05, vk.e2
    lqv $v18, 0x20, a3
    vand $v12, $v13, vk.e2
    lrv $v18, 0x30, a3
    vand $v05, $v05, vk.e1
    lqv $v19, 0x20, t8
    vand $v13, $v13, vk.e1
    lqv $v20, 0x20, t7
    vaddc $v01, $v01, $v02
    lrv $v20, 0x30, t7
    vaddc $v09, $v09, $v10
    lqv $v25, 0x20, a0
    vand $v07, $v07, $v04
    lrv $v25, 0x30, a0
    vand $v15, $v15, $v12
    lqv $v26, 0x20, a1
    vand $v06, $v06, $v04
    lrv $v26, 0x30, a1
    vand $v14, $v14, $v12
    lqv $v29, 0x20, t5
    vand $v02, $v08, vk.e5
    lrv $v29, 0x30, t5
    vand $v10, $v16, vk.e5
    lqv $v21, 0x30, a2
    vand $v07, $v07, $v03
    lqv $v22, 0x30, a3
    vand $v15, $v15, $v11
    lrv $v22, 0x40, a3
    vmudn $v01, $v01, $v05
    lqv $v23, 0x30, t8
    vmudn $v09, $v09, $v13
    lqv $v24, 0x30, t7
    vmudn $v06, $v06, vk.e3
    lrv $v24, 0x40, t7
    vmudn $v14, $v14, vk.e3
    lqv $v27, 0x30, a0
    vand $v04, $v08, vk.e6
    lrv $v27, 0x40, a0
    vand $v12, $v16, vk.e6
    lqv $v28, 0x30, a1
    vmudn $v07, $v07, vk.e4
    lrv $v28, 0x40, a1
    vmudn $v15, $v15, vk.e4
    lqv $v31, 0x30, t5
    vand $v03, $v08, vk.e7
    lrv $v31, 0x40, t5
    vand $v11, $v16, vk.e7
    addiu a0, 32
    vor $v01, $v01, $v06
    addiu a1, 32
    vor $v09, $v09, $v14
    addiu a2, 32
    vaddc $v08, $v08, $v07
    addiu a3, 32
    vaddc $v16, $v16, $v15
    addiu t5, 32
    vmudl $v02, $v02, $v01
    addiu t6, 32
    vmudl $v10, $v10, $v09
    addiu t7, 32
    vmudl $v04, $v04, $v01
    addiu t8, 32
    vmudl $v12, $v12, $v09
    addiu v1, -1
    vmudl $v03, $v03, $v01
    vmudl $v11, $v11, $v09
    vand $v02, $v02, vk.e5
    vand $v10, $v10, vk.e5
    vand $v04, $v04, vk.e6
    vand $v12, $v12, vk.e6
    vand $v03, $v03, vk.e7
    vand $v11, $v11, vk.e7
    vor $v02, $v02, $v04
    vor $v10, $v10, $v12
    vsubc $v08, $v08, $v03
    vsubc $v16, $v16, $v11
    vsubc $v08, $v08, $v02
    vsubc $v16, $v16, $v10
    bnez v1, PixLoop
    addiu t9, 32
    sqv $v08, 0x00, k0
    sqv $v16, 0x10, k0

    # write this half back, then read the next line's half into the same
    # place (the DMA engine does them in order) while the other half runs
    addu t0, s3, s4
    addiu t1, s4, %lo(FBL)
    jal DmaOut
    li t2, 256-1
    beq s1, s6, 1f
    addu t0, s0, s4
    addiu t1, s4, %lo(FBL)
    jal DmaIn
    li t2, 256-1
1:  bnez s4, 2f
    li k0, %lo(DUMMY)               # right half: same loop, nothing to store first
    li v1, 8
    j PixLoop
    li s4, 256
2:  move s5, s1
    bne s5, s6, LineLoop
    nop

Done:
    jal DmaWait
    nop
    break
    nop

##############################################################################
# NextFx: a0 = line -> v0 = first line >= a0 flagged in JOB_FXBITS, or s6.
##############################################################################
NextFx:
    move v0, a0
1:  beq v0, s6, 9f
    srl t0, v0, 5
    sll t0, 2
    lw t0, %lo(JOB_FXBITS)(t0)
    srlv t0, t0, v0
    andi t0, 1
    beqz t0, 1b
    addiu v0, 1
    addiu v0, -1
9:  jr ra
    nop

##############################################################################
# LineAddr: a0 = line -> t0 = its framebuffer address
# (stride = (1 << JOB_SH[0]) + (1 << JOB_SH[1])). Destroys t1, t2.
##############################################################################
LineAddr:
    lbu t1, %lo(JOB_SH)(zero)
    lbu t2, %lo(JOB_SH)+1(zero)
    lw t0, %lo(JOB_FB)(zero)
    sllv t1, a0, t1
    sllv t2, a0, t2
    addu t0, t1
    jr ra
    addu t0, t2

##############################################################################
# PrefetchFb: a0 = line. Starts the DMA of its pixels into FBL.
# PrefetchRaw: a0 = line. Starts the DMA of its mask row y+1 into RAW if
# that row will be needed (s2 = that row, or -1).
##############################################################################
PrefetchFb:
    move t5, ra
    jal LineAddr
    nop
    li t1, %lo(FBL)
    jal DmaIn
    li t2, 512-1
    jr t5
    nop

PrefetchRaw:
    move t5, ra
    addiu t3, a0, 1                 # mask row
    li s2, -1
    jal RowValid
    move a0, t3
    beqz v0, 9f
    nop
    jal FindRow
    move a0, t3
    bgez v0, 9f                     # already in a slot
    lw t0, %lo(JOB_MASK)(zero)
    sll t1, t3, 9
    addu t0, t1
    li t1, %lo(RAW)
    jal DmaIn
    li t2, 512-1
    move s2, t3
9:  jr t5
    nop

##############################################################################
# RowValid: a0 = row -> v0 = 1 if it's a valid mask row. Destroys t0.
##############################################################################
RowValid:
    bltz a0, 9f
    move v0, zero
    sltu t0, a0, s6
    beqz t0, 9f
    srl t0, a0, 5
    sll t0, 2
    lw t0, %lo(JOB_VALIDBITS)(t0)
    srlv t0, t0, a0
    andi v0, t0, 1
9:  jr ra
    nop

##############################################################################
# FindRow: a0 = row -> v0 = DMEM address of its pixel 0, or -1 if it isn't
# in a slot. Destroys t0, t1.
##############################################################################
FindRow:
    li t0, %lo(TAGS)
    li v0, %lo(ROWS) + 16
1:  lw t1, 0(t0)
    beq t1, a0, 9f
    addiu t0, 4
    li t1, %lo(TAGS) + 4 * NSLOTS
    bne t0, t1, 1b
    addiu v0, ROW_SIZE
    li v0, -1
9:  jr ra
    nop

##############################################################################
# Victim: a slot holding no row of y-3 .. y+1 (s5 = y) -> v0 = its pixel 0,
# t4 = its tag address. Destroys t0, t1.
##############################################################################
Victim:
    li t4, %lo(TAGS)
    li v0, %lo(ROWS) + 16
1:  lw t1, 0(t4)
    subu t1, s5
    addiu t1, 3                     # row - (y - 3): 0..4 is in use
    sltiu t1, t1, 5
    beqz t1, 9f
    nop
    addiu t4, 4
    b 1b
    addiu v0, ROW_SIZE
9:  jr ra
    nop

##############################################################################
# MapRows: PTRS[i] = pixel 0 of row y-3+i (s5 = y), or 0 if not in a slot.
# Destroys t0..t3, v0.
##############################################################################
MapRows:
    sw zero, %lo(PTRS)+0(zero)
    sw zero, %lo(PTRS)+4(zero)
    sw zero, %lo(PTRS)+8(zero)
    sw zero, %lo(PTRS)+12(zero)
    sw zero, %lo(PTRS)+16(zero)
    li t0, %lo(TAGS)
    li v0, %lo(ROWS) + 16
1:  lw t1, 0(t0)
    subu t1, s5
    addiu t1, 3                     # row - (y - 3)
    sltiu t2, t1, 5
    beqz t2, 2f
    sll t1, 2
    sw v0, %lo(PTRS)(t1)
2:  addiu t0, 4
    li t3, %lo(TAGS) + 4 * NSLOTS
    bne t0, t3, 1b
    addiu v0, ROW_SIZE
    jr ra
    nop

##############################################################################
# LoadRow: a0 = row (not in a slot) -> loaded and converted into a free slot,
# a1 = its pixel 0. Destroys t0..t4, v0, $v01..$v16, $v31.
##############################################################################
LoadRow:
    move t3, ra
    jal Victim
    nop
    sw a0, 0(t4)
    move a1, v0                     # slot pixel 0
    jal RowValid
    nop
    beqz v0, 5f
    lw t0, %lo(JOB_MASK)(zero)
    sll t1, a0, 9
    addu t0, t1
    li t1, %lo(RAW)
    jal DmaIn
    li t2, 512-1
    jal DmaWait
    nop
    move t4, a1
    jal ConvRaw
    nop
    jr t3
    move a1, t4
5:  # outside
    move t4, a1
    li t1, 32
6:  sqv vout, 0x00, a1
    addiu t1, -1
    bnez t1, 6b
    addiu a1, 16
    jr t3
    move a1, t4

##############################################################################
# ConvRaw: RAW (z values) -> classes at a1, 8 groups per step:
# t = (z >> 8) & 7: BG1 (t == 0) 0x3802, sprite ((t & 5) == 4) 0x3800, else 0x0001.
# Destroys t0, t1, a1, $v01..$v16, $v31.
##############################################################################
ConvRaw:
    vaddc $v31, vzero, vzero        # VCO = 0 (the compares below read it; the pixel loop leaves it set)
    li t0, %lo(RAW)
    li t1, 4
1:
    lqv $v01, 0x00, t0
    lqv $v02, 0x10, t0
    lqv $v03, 0x20, t0
    lqv $v04, 0x30, t0
    lqv $v05, 0x40, t0
    lqv $v06, 0x50, t0
    lqv $v07, 0x60, t0
    lqv $v08, 0x70, t0
    vmudl $v01, $v01, vc.e0
    vmudl $v02, $v02, vc.e0
    vmudl $v03, $v03, vc.e0
    vmudl $v04, $v04, vc.e0
    vmudl $v05, $v05, vc.e0
    vmudl $v06, $v06, vc.e0
    vmudl $v07, $v07, vc.e0
    vmudl $v08, $v08, vc.e0
    vand $v01, $v01, vc.e1
    vand $v02, $v02, vc.e1
    vand $v03, $v03, vc.e1
    vand $v04, $v04, vc.e1
    vand $v05, $v05, vc.e1
    vand $v06, $v06, vc.e1
    vand $v07, $v07, vc.e1
    vand $v08, $v08, vc.e1
    veq $v31, $v01, vzero
    vmrg $v09, vbg1, vbg
    veq $v31, $v02, vzero
    vmrg $v10, vbg1, vbg
    veq $v31, $v03, vzero
    vmrg $v11, vbg1, vbg
    veq $v31, $v04, vzero
    vmrg $v12, vbg1, vbg
    veq $v31, $v05, vzero
    vmrg $v13, vbg1, vbg
    veq $v31, $v06, vzero
    vmrg $v14, vbg1, vbg
    veq $v31, $v07, vzero
    vmrg $v15, vbg1, vbg
    veq $v31, $v08, vzero
    vmrg $v16, vbg1, vbg
    vand $v01, $v01, vc.e2
    vand $v02, $v02, vc.e2
    vand $v03, $v03, vc.e2
    vand $v04, $v04, vc.e2
    vand $v05, $v05, vc.e2
    vand $v06, $v06, vc.e2
    vand $v07, $v07, vc.e2
    vand $v08, $v08, vc.e2
    veq $v31, $v01, vc.e3
    vmrg $v09, vobj, $v09
    veq $v31, $v02, vc.e3
    vmrg $v10, vobj, $v10
    veq $v31, $v03, vc.e3
    vmrg $v11, vobj, $v11
    veq $v31, $v04, vc.e3
    vmrg $v12, vobj, $v12
    veq $v31, $v05, vc.e3
    vmrg $v13, vobj, $v13
    veq $v31, $v06, vc.e3
    vmrg $v14, vobj, $v14
    veq $v31, $v07, vc.e3
    vmrg $v15, vobj, $v15
    veq $v31, $v08, vc.e3
    vmrg $v16, vobj, $v16
    sqv $v09, 0x00, a1
    sqv $v10, 0x10, a1
    sqv $v11, 0x20, a1
    sqv $v12, 0x30, a1
    sqv $v13, 0x40, a1
    sqv $v14, 0x50, a1
    sqv $v15, 0x60, a1
    sqv $v16, 0x70, a1
    addiu t0, 128
    addiu t1, -1
    bnez t1, 1b
    addiu a1, 128
    jr ra
    nop

##############################################################################
# DMA helpers. t0 = RDRAM address, t1 = DMEM address, t2 = length - 1.
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
