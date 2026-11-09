    .set noreorder
    .section .text.start
    .globl _start
_start:
    la    $gp, _gp
    la    $sp, __stack_top
    la    $t0, __bss_start
    la    $t1, __bss_end
1:  beq   $t0, $t1, 2f
    nop
    sd    $zero, 0($t0)
    b     1b
    addiu $t0, $t0, 8
2:  jal   main
    nop
    move  $a1, $v0
    li    $v0, 3          # hostcall exit
    syscall
3:  b 3b
    nop

    .globl hostcall
hostcall:                 # v0 = hostcall(a0=code, a1, a2, a3)
    move  $v0, $a0
    syscall
    jr    $ra
    nop
