/* SPDX-License-Identifier: GPL-2.0 */
/* Refresh both clock-domain anchors before rebuilding native FAW candidates. */
.set noreorder
.set mips16
.text
.globl faw_anchor
.ent faw_anchor
faw_anchor:
 addiu $sp,-32
 sw $v1,28($sp)
 sw $a0,24($sp)
 sw $a1,20($sp)
 li $v0,0xb8e1
 sll $v0,8
 sll $v0,8
 lw $v0,0x6e08($v0)
 lbu $a1,88($v0)
 li $a3,0xb8e1
 sll $a3,8
 sll $a3,8
 addiu $a3,0x6de4
 li $a0,0
 jal 0xb89aabac
 li $a2,0
 lw $a0,24($sp)
 lw $a1,20($sp)
 lw $a3,28($sp)
 addiu $sp,32
 addiu $sp,-112
 sw $a3,108($sp)
 sw $s1,104($sp)
 sw $s0,100($sp)
 li $v0,0xb89b
 sll $v0,8
 sll $v0,8
 addiu $v0,-10551
 jr $v0
 nop
.end faw_anchor
