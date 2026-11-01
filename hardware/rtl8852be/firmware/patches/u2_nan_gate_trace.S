/* SPDX-License-Identifier: GPL-2.0 */
/* Read back the MACID gates immediately after native DW/FAW writes. */
.set noreorder
.set mips16
.text
.globl nan_gate_trace
.ent nan_gate_trace
nan_gate_trace:
 addiu $sp,-80
 sw $v1,76($sp)
 sw $a0,32($sp)
 sw $a1,36($sp)
 sw $a2,40($sp)
 sw $a3,44($sp)
 jal native_body
 nop
 sw $v0,64($sp)
 lw $v0,44($sp)
 cmpi $v0,35
 bteqz observe
 cmpi $v0,36
 btnez done
observe:
 lw $v0,32($sp)
 srl $v0,5
 sll $v0,2
 li $a0,0xc2c0
 addu $a0,$v0
 jal read32
 nop
 lw $v1,32($sp)
 srlv $v0,$v1
 li $v1,1
 and $v0,$v1
 sll $v0,10
 sw $v0,56($sp)
 lw $v0,32($sp)
 srl $v0,5
 sll $v0,2
 li $a0,0x9eb0
 addu $a0,$v0
 jal read32
 nop
 lw $v1,32($sp)
 srlv $v0,$v1
 li $v1,1
 and $v0,$v1
 sll $v0,11
 lw $v1,56($sp)
 or $v0,$v1
 lw $v1,32($sp)
 or $v0,$v1
 lw $v1,36($sp)
 sll $v1,8
 or $v0,$v1
 lw $v1,40($sp)
 sll $v1,9
 or $v0,$v1
.ifdef GATE_SEQUENCE
 lw $v1,44($sp)
 addiu $v1,-35
 sll $v1,12
 or $v0,$v1
 li $a0,0xb8e1
 sll $a0,8
 sll $a0,8
 lw $v1,0x2124($a0)
 addiu $v1,1
 sw $v1,0x2124($a0)
 sll $v1,13
 or $v0,$v1
.else
 lw $v1,44($sp)
 sll $v1,8
 sll $v1,8
 or $v0,$v1
.endif
 sw $v0,24($sp)
 li $a0,0xc438
 jal read32
 nop
 sw $v0,28($sp)
.ifdef GATE_SEQUENCE
 li $v0,249
.else
 li $v0,246
.endif
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
done:
 lw $v0,64($sp)
 lw $a3,76($sp)
 jr $a3
 addiu $sp,80
.end nan_gate_trace

.balign 4
.ent read32
read32:
 sw $ra,68($sp)
 li $v1,0xb8e1
 sll $v1,8
 sll $v1,8
 lw $v1,-27436($v1)
 jalr $v1
 nop
 lw $a3,68($sp)
 jr $a3
 nop
.end read32

.balign 4
.ent native_body
native_body:
 addiu $sp,-72
 sw $ra,68($sp)
 sw $s1,64($sp)
 sw $s0,60($sp)
 li $v0,0xb898
 sll $v0,8
 sll $v0,8
 addiu $v0,-25579
 jr $v0
 nop
.end native_body
