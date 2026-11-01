/* SPDX-License-Identifier: GPL-2.0 */
/* U2 native NAN lifecycle. Linked into the code-section extension. */
.set noreorder
.set mips16
.text

.macro base reg
 li \reg, 0xb8e1
 sll \reg, 8
 sll \reg, 8
.endm

/* Entry veneers pass the original return address in $v1. Callbacks accept
 * at most $a0; the trampoline uses $a3 to return to the native body. */
.macro callback name, required, faw=0, notify_dw=0
.balign 4
.globl \name
.type \name,@function
.ent \name
\name:
 addiu $sp, -48
 sw $v1, 44($sp)
 sw $a0, 16($sp)
 sw $a1, 20($sp)
 sw $a2, 24($sp)
 sw $a3, 28($sp)
 jal 0xb89ba75c
 nop
 sw $v0, 32($sp)
 li $v0, 0
 sw $v0, 36($sp)
 base $v0
 lbu $v0, 0x6e2d($v0)
 li $v1, \required
 and $v0, $v1
 cmpi $v0, \required
 btnez 1f
.if \faw
 base $v0
 lhu $v0, 0x6ae2($v0)
 beqz $v0, 1f
.endif
 lw $a0, 16($sp)
 lw $a1, 20($sp)
 lw $a2, 24($sp)
 jal \name\()_body
 lw $a3, 28($sp)
 sw $v0, 36($sp)
.if \notify_dw
 /* Publish readiness follows the native SDF window, using the existing
  * TSF notification builder and the current DW port. */
 base $v0
 lbu $v1, 0x68b8($v0)
 bnez $v1, 1f
 lw $v0, 0x6e08($v0)
 lbu $a1, 76($v0)
 li $a0, 0
 jal 0xb89a766c
 li $a2, 1
.endif
1:
 lw $a0, 32($sp)
 jal 0xb89ba7a0
 nop
 lw $v0, 36($sp)
 lw $a3, 44($sp)
 jr $a3
 addiu $sp, 48
.end \name
.endm

callback dw_release, 3, 0, 1
callback dw_end, 3
callback faw_begin, 7, 1
callback faw_end, 7, 1
callback dw_initial, 3
callback warmup, 3

.balign 4
.globl stop_dispatch
.type stop_dispatch,@function
.ent stop_dispatch
stop_dispatch:
 addiu $sp, -48
 sw $ra, 44($sp)
 sw $s0, 40($sp)
 sw $s1, 36($sp)
 move $s0, $a0
 lbu $v0, 6($a0)
 lbu $v1, 7($a0)
 or $v0, $v1
 bnez $v0, 3f
 jal 0xb89ba75c
 nop
 move $s1, $v0
 base $v0
 lbu $v0, 0x6dfe($v0)
 sw $v0, 24($sp)
 jal 0xb89ac6d0
 move $a0, $s0
 sw $v0, 28($sp)
 /* Native status values are translated to H2C status by the dispatcher. */
 base $v1
 lw $v1, 0x454c($v1)
 cmp $v0, $v1
 btnez 2f
 lw $a0, 24($sp)
 li $a1, 0
 li $a2, 0
 jal 0xb8979c0c
 li $a3, 36
 base $v0
 li $v1, 0
 sb $v1, 0x68bc($v0)
 sb $v1, 0x6ad4($v0)
 li $v1, 0xffff
 sh $v1, 0x68be($v0)
 sh $v1, 0x6aa8($v0)
2:
 jal 0xb89ba7a0
 move $a0, $s1
 lw $v0, 28($sp)
 b 4f
3:
 jal 0xb89ac6d0
 move $a0, $s0
4:
 lw $a3, 44($sp)
 lw $s0, 40($sp)
 lw $s1, 36($sp)
 jr $a3
 addiu $sp, 48
.end stop_dispatch

/* NAN startup resets TSF on an allocated port. Stop its counter before
 * resetting TSF so the first TBTT uses the new clock, like later TBTTs.
 * Native entry supplies s0=PORT_CFG, s1=write32, v0=current PORT_CFG. */
.balign 4
.globl port_tsf_reset
.type port_tsf_reset,@function
.ent port_tsf_reset
port_tsf_reset:
 addiu $sp, -32
 sw $ra, 28($sp)
 sw $v0, 16($sp)
 li $a1, 1
 sll $a1, 16
 addiu $a1, 5
 neg $a1
 and $a1, $v0
 sw $a1, 20($sp)
 jalr $s1
 move $a0, $s0
 lw $a1, 20($sp)
 li $v0, 32
 or $a1, $v0
 jalr $s1
 move $a0, $s0
 move $a0, $s0
 addiu $a0, 52
 jalr $s1
 li $a1, 0
 lw $a1, 16($sp)
 jalr $s1
 move $a0, $s0
 lw $a3, 28($sp)
 jr $a3
 addiu $sp, 32
.end port_tsf_reset

/* Exact native prefixes and continuation addresses are emitted by the builder. */
.include "native_prefixes.S"
