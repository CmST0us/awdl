/* SPDX-License-Identifier: GPL-2.0 */
/* Record the native candidate calculator's current TSF and caller. */
.set noreorder
.set mips16
.text
.globl faw_clock_trace
.ent faw_clock_trace
faw_clock_trace:
 li $v0,0xb8e1
 sll $v0,8
 sll $v0,8
 addiu $v0,0x2080
 sw $a0,0($v0)
 sw $a1,4($v0)
 sw $a2,8($v0)
 sw $a3,12($v0)
 sw $v1,16($v0)
 lw $a3,20($v0)
 addiu $a3,1
 sw $a3,20($v0)
 lw $a3,12($v0)
 addiu $sp,-96
 sw $v1,92($sp)
 sw $s1,88($sp)
 sw $s0,84($sp)
 li $v0,0xb89b
 sll $v0,8
 sll $v0,8
 addiu $v0,-11019
 jr $v0
 nop
.end faw_clock_trace
