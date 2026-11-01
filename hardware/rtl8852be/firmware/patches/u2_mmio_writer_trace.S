/* SPDX-License-Identifier: GPL-2.0 */
/* Diagnostic-only MMIO gateway tracing; original writes execute unchanged. */
.set noreorder
.set mips16
.text
.globl writer_install
.ent writer_install
writer_install:
 li $v1,0xb8e1
 sll $v1,8
 sll $v1,8
 lw $v0,-27456($v1)
 la $a0,writer8
 cmp $v0,$a0
 bteqz installed
 la $v0,writer8
 sw $v0,-27456($v1)
 la $v0,writer16
 sw $v0,-27452($v1)
 la $v0,writer32
 sw $v0,-27448($v1)
installed:
 jr $ra
 nop
.end writer_install

.macro gateway name, target, marker
.balign 4
.globl \name
.ent \name
\name:
 addiu $sp,-64
 sw $ra,60($sp)
 sw $a0,32($sp)
 sw $a1,36($sp)
 li $v0,\marker
 sw $v0,20($sp)
 jal \target
 nop
 sw $v0,56($sp)
 lw $v0,32($sp)
 li $v1,0xc604
 cmp $v0,$v1
 btnez 1f
 lw $v0,60($sp)
 sw $v0,24($sp)
 lw $v0,36($sp)
 sw $v0,28($sp)
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
 move $a0,$sp
 jal writer_parent
 nop
 beqz $v0,1f
 sw $v0,24($sp)
 li $v0,243
 sw $v0,20($sp)
 lw $v0,36($sp)
 sw $v0,28($sp)
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
1:
 lw $v0,56($sp)
 lw $a3,60($sp)
 jr $a3
 addiu $sp,64
.end \name
.endm

gateway writer8, 0xb890ce48, 241
gateway writer16, 0xb890ce08, 242
gateway writer32, 0xb890cdc8, 244

/* Gateway64 + setter24 + dispatcher24; inspect only the known call chain. */
.balign 4
.globl writer_parent
.ent writer_parent
writer_parent:
 li $v0,0
 lw $v1,60($a0)
 li $a1,0xb89a
 sll $a1,16
 addiu $a1,-27655
 cmp $v1,$a1
 btnez parent_done
 lw $v1,84($a0)
 li $a1,0xb89a
 sll $a1,16
 addiu $a1,-25315
 cmp $v1,$a1
 btnez parent_done
 lw $v0,108($a0)
parent_done:
 jr $ra
 nop
.end writer_parent
