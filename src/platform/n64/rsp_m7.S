##############################################################################
# rsp_m7.S - mode 7 rows for unrotated, unscaled lines (Ceres) on the RSP.
#
# For such a line BG1's 256 colour indices are a window of one row of the
# 1024 x 1024 mode 7 plane: row Y, starting at column X0 (wrapping at 1024).
# The CPU lists those lines (snes/ppu_rsp.c, Mode7Row); this microcode builds
# their rows from the tilemap and tile pixels (g_m7map / g_m7pix, the CPU's
# byte copies of mode 7 VRAM) and writes them to m7rows, where rsp_ppu.S
# reads them. It runs before rsp_ppu.S.
#
# Lines of one tile row share their 33 tiles: those are fetched (64 bytes
# each) once and reused while the tile row and first column stay the same.
#
# Must match Mode7Row's scale-1 path; checked against it in
# tools/rsp_test/m7host.c.
#
# Standalone microcode (no rspq): the CPU fills JOB in DMEM, runs from PC 0,
# and waits for the break.
##############################################################################
#include <rsp.inc>

    .set noat

    .data
    .align 4
# ---- Job, written by the CPU before each run (32 bytes) ----
JOB_MAP:      .long 0          # mode 7 tilemap, 128 x 128 bytes
JOB_PIX:      .long 0          # mode 7 tile pixels, 256 tiles x 64 bytes
JOB_OUT:      .long 0          # m7rows: 256 bytes per row index
JOB_LIST:     .long 0          # entries: u16 row index, u16 Y, u16 X0, u16 0
JOB_N:        .half 0          # number of entries
              .half 0
              .long 0, 0, 0

#define CHUNK 32
    .align 4
V_K:      .half 0x0100, 0, 0, 0, 0, 0, 0, 0
    .align 4
LIST:     .space CHUNK * 8     # entries being worked on
    .align 4
MAPROW:   .space 128           # tilemap row TY
    .align 4
TILES:    .space 33 * 64       # the tiles of columns c0 .. c0 + 32
    .align 4
TEMP:     .space 272           # the 33 tile rows of this line, back to back
    .align 4
OUT:      .space 256

# s0 = entries left, s1 = entries left in LIST, s2 = LIST pointer, s3 = next
# list address in RDRAM, s4 = current tile row (-1 none), s5 = current first
# column (-1 none)

#define vk $v31

    .text
    .globl _start
_start:
    li t0, %lo(V_K)
    lqv vk, 0, t0
    lhu s0, %lo(JOB_N)(zero)
    lw s3, %lo(JOB_LIST)(zero)
    li s4, -1
    li s5, -1
    move s1, zero

Next:
    beqz s0, Done
    nop
    bnez s1, 1f
    nop
    # refill LIST with up to CHUNK entries
    move t0, s3
    li t1, %lo(LIST)
    jal DmaIn
    li t2, CHUNK * 8 - 1
    jal DmaWait
    nop
    addiu s3, CHUNK * 8
    li s1, CHUNK
    li s2, %lo(LIST)
1:  lhu t7, 0(s2)                   # row index
    lhu t8, 2(s2)                   # Y
    lhu t9, 4(s2)                   # X0
    addiu s2, 8
    addiu s1, -1
    addiu s0, -1

    # tilemap row
    srl t3, t8, 3                   # TY
    beq t3, s4, 2f
    move s4, t3
    lw t0, %lo(JOB_MAP)(zero)
    sll t1, t3, 7
    addu t0, t1
    li t1, %lo(MAPROW)
    jal DmaIn
    li t2, 128-1
    jal DmaWait
    li s5, -1                       # the tiles must be fetched again
2:
    # the 33 tiles from column c0
    srl t4, t9, 3                   # c0
    beq t4, s5, 4f
    move s5, t4
    li t5, %lo(TILES)
    li t6, 33
3:  andi t3, t4, 127
    lbu t3, %lo(MAPROW)(t3)         # tile number
    lw t0, %lo(JOB_PIX)(zero)
    sll t3, 6
    addu t0, t3
    move t1, t5
    jal DmaIn
    li t2, 64-1
    addiu t4, 1
    addiu t6, -1
    bnez t6, 3b
    addiu t5, 64
    jal DmaWait
    nop
4:
    # this line's pixel row of each tile, back to back
    andi t3, t8, 7
    sll t3, 3
    addiu t3, %lo(TILES)            # row r of tile 0
    li t5, %lo(TEMP)
    li t6, 11
5:  ldv $v01.e0, 0, t3              # three at a time (a store right after its load stalls)
    ldv $v02.e0, 64, t3
    ldv $v03.e0, 128, t3
    addiu t3, 192
    addiu t6, -1
    sdv $v01.e0, 0, t5
    sdv $v02.e0, 8, t5
    sdv $v03.e0, 16, t5
    bnez t6, 5b
    addiu t5, 24
    # the previous row's DMA must be done before OUT is rewritten
    jal DmaWait
    nop
    # 256 bytes starting at pixel X0 & 7 (vector loads need an even address:
    # for an odd start, combine the halfwords at start - 1 and start + 1)
    andi t3, t9, 7
    andi t4, t3, 1
    subu t3, t4
    addiu t3, %lo(TEMP)
    li t5, %lo(OUT)
    bnez t4, 7f
    li t6, 16
6:  lqv $v01, 0x00, t3
    lrv $v01, 0x10, t3
    sqv $v01, 0x00, t5
    addiu t3, 16
    addiu t6, -1
    bnez t6, 6b
    addiu t5, 16
    j 8f
    nop
7:  lqv $v01, 0x00, t3
    lrv $v01, 0x10, t3
    addiu t4, t3, 2
    lqv $v02, 0x00, t4
    lrv $v02, 0x10, t4
    vmudn $v01, $v01, vk.e0         # << 8 (low byte of each halfword to the top)
    vmudl $v02, $v02, vk.e0         # >> 8
    vor $v01, $v01, $v02
    sqv $v01, 0x00, t5
    addiu t3, 16
    addiu t6, -1
    bnez t6, 7b
    addiu t5, 16
8:
    lw t0, %lo(JOB_OUT)(zero)
    sll t1, t7, 8
    addu t0, t1
    li t1, %lo(OUT)
    jal DmaOut
    li t2, 256-1
    j Next
    nop

Done:
    jal DmaWait
    nop
    break
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
