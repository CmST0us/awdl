/* SPDX-License-Identifier: GPL-2.0 */
/* Trace completed native channel calls with the current TSF. */
.set noreorder
.set mips16
.text
.globl nan_channel_trace
.ent nan_channel_trace
nan_channel_trace:
 addiu $sp,-64
 sw $v1,60($sp)
 lbu $v0,0($a0)
 lbu $v1,1($a0)
 sll $v1,8
 or $v0,$v1
 lbu $v1,2($a0)
 sll $v1,8
 sll $v1,8
 or $v0,$v1
 sw $v0,32($sp)
 jal channel_body
 nop
 sw $v0,36($sp)
 sll $v0,24
 lw $v1,32($sp)
 or $v0,$v1
 sw $v0,24($sp)
 li $v1,0xb8e1
 sll $v1,8
 sll $v1,8
 lw $v1,-27436($v1)
 li $a0,0xc438
 jalr $v1
 nop
 sw $v0,28($sp)
 li $v0,245
 sw $v0,20($sp)
 li $v0,3
 sw $v0,16($sp)
 li $v1,0xb8e1
 sll $v1,8
 sll $v1,8
 lw $a0,0x4b10($v1)
 lw $a1,0x4b14($v1)
 lw $v1,-26384($v1)
 li $a3,857
 jalr $v1
 li $a2,2
 lw $v0,36($sp)
 lw $a3,60($sp)
 jr $a3
 addiu $sp,64
.end nan_channel_trace
.balign 4
.ent channel_body
channel_body:
 addiu $sp,-120
 sw $ra,116($sp)
 sw $s1,112($sp)
 sw $s0,108($sp)
 li $v0,0xb89a
 sll $v0,8
 sll $v0,8
 addiu $v0,11341
 jr $v0
 nop
.end channel_body
