/* SPDX-License-Identifier: GPL-2.0 */
/* Native beacon prepare, field-update, TX gate and TX-report tracing. */
.set noreorder
.set mips16
.text
.globl beacon_trace
.type beacon_trace,@function
.ent beacon_trace
beacon_trace:
 addiu $sp, -64
 sw $v1, 60($sp)
 sw $a0, 32($sp)
 sw $a1, 36($sp)
 sw $a2, 40($sp)
 sw $a3, 44($sp)
 li $v0, 252
 sw $v0, 20($sp)
 jal trace_tsf
 nop
 sw $v0, 28($sp)
 jal trace_log
 nop
 lw $a0, 32($sp)
 lw $a1, 36($sp)
 lw $a2, 40($sp)
 jal beacon_body
 lw $a3, 44($sp)
 sw $v0, 48($sp)
 sw $v0, 28($sp)
 li $v0, 253
 sw $v0, 20($sp)
 jal trace_log
 nop
 lw $v0, 48($sp)
 lw $a3, 60($sp)
 jr $a3
 addiu $sp, 64
.end beacon_trace

/* The caller owns the o32 outgoing argument area and saved arguments. */
.balign 4
.type trace_log,@function
.ent trace_log
trace_log:
 sw $ra, 52($sp)
 lw $v0, 32($sp)
 lw $v1, 36($sp)
 sll $v1, 8
 or $v0, $v1
 lw $v1, 40($sp)
 sll $v1, 8
 sll $v1, 8
 or $v0, $v1
 sw $v0, 24($sp)
.balign 4
trace_values:
 sw $ra, 52($sp)
 li $v0, 3
 sw $v0, 16($sp)
 li $v1, 0xb8e1
 sll $v1, 8
 sll $v1, 8
 lw $a0, 0x4b10($v1)
 lw $a1, 0x4b14($v1)
 lw $v1, -26384($v1)
 li $a3, 857
trace_emit:
.if WA_BEACON_TRACE
 jalr $v1
.else
 nop
.endif
 li $a2, 2
 lw $a3, 52($sp)
 jr $a3
 nop
.end trace_log

.balign 4
.type beacon_body,@function
.ent beacon_body
beacon_body:
 addiu $sp, -120
 sw $ra, 116($sp)
 sw $s1, 112($sp)
 sw $s0, 108($sp)
 li $a3, 0xb89a
 sll $a3, 8
 sll $a3, 8
 addiu $a3, 0x4821
 jr $a3
 nop
.end beacon_body

/* Common port0 TSF clock; single-board diagnostics do not adjust that clock. */
.balign 4
.ent trace_tsf
trace_tsf:
 sw $ra, 56($sp)
 li $v0, 0xb8e1
 sll $v0, 8
 sll $v0, 8
 lw $v0, -27436($v0)
 li $a0, 0xc438
 jalr $v0
 nop
 lw $a3, 56($sp)
 jr $a3
 nop
.end trace_tsf

.balign 4
.globl field_trace
.ent field_trace
field_trace:
 addiu $sp, -64
 sw $v1, 60($sp)
 sw $a0, 32($sp)
 sw $a1, 36($sp)
 sw $a2, 40($sp)
 sw $a3, 44($sp)
 jal field_body
 nop
 sw $v0, 48($sp)
 jal trace_tsf
 nop
 sw $v0, 28($sp)
 li $v0, 250
 sw $v0, 20($sp)
 lw $v0, 36($sp)
 lw $v1, 44($sp)
 lw $a0, 4($v1)
 sll $a0, 8
 or $v0, $a0
 lw $a0, 8($v1)
 sll $a0, 8
 sll $a0, 8
 or $v0, $a0
 lw $v1, 32($sp)
 sll $v1, 8
 sll $v1, 8
 sll $v1, 8
 or $v0, $v1
 sw $v0, 24($sp)
 jal trace_values
 nop
 lw $v0, 48($sp)
 lw $a3, 60($sp)
 jr $a3
 addiu $sp, 64
.end field_trace
.balign 4
.ent field_body
field_body:
 addiu $sp, -24
 sw $ra, 20($sp)
 sw $s0, 16($sp)
 li $v0, 0xb8e1
 li $a2, 0xb89a
 sll $a2, 8
 sll $a2, 8
 addiu $a2, 0x41e3
 jr $a2
 nop
.end field_body

.balign 4
.globl gate_trace
.ent gate_trace
gate_trace:
 addiu $sp, -64
 sw $v1, 60($sp)
 sw $a0, 32($sp)
 sw $a1, 36($sp)
 sw $a2, 40($sp)
 sw $a3, 44($sp)
 jal gate_body
 nop
 sw $v0, 48($sp)
 jal trace_tsf
 nop
 sw $v0, 28($sp)
 li $v0, 248
 sw $v0, 20($sp)
 lw $v0, 36($sp)
 lw $v1, 40($sp)
 sll $v1, 8
 or $v0, $v1
 lw $v1, 32($sp)
 sll $v1, 8
 sll $v1, 8
 sll $v1, 8
 or $v0, $v1
 sw $v0, 24($sp)
 jal trace_values
 nop
 lw $v0, 48($sp)
 lw $a3, 60($sp)
 jr $a3
 addiu $sp, 64
.end gate_trace
.balign 4
.ent gate_body
gate_body:
 addiu $sp, -32
 sw $ra, 28($sp)
 sw $s1, 24($sp)
 sw $s0, 20($sp)
 li $v0, 0xb89a
 sll $v0, 8
 sll $v0, 8
 addiu $v0, 0x6c89
 jr $v0
 nop
.end gate_body


.set mips16
.balign 4
.globl tx_trace
.ent tx_trace
tx_trace:
 addiu $sp, -80
 sw $v0, 32($sp)
 sw $v1, 36($sp)
 sw $a0, 40($sp)
 sw $a1, 44($sp)
 sw $a2, 48($sp)
 sw $a3, 60($sp)
 move $v1, $t8
 sw $v1, 64($sp)
 move $v1, $t9
 sw $v1, 68($sp)
 sw $ra, 72($sp)
 lbu $a0, 16($v0)
 li $v1, 15
 and $a0, $v1
 sll $a0, 8
 sll $a0, 8
 lbu $a1, 17($v0)
 li $v1, 7
 and $v1, $a1
 or $a0, $v1
 srl $a1, 4
 sll $a1, 8
 or $a0, $a1
 lw $v0, 136($sp)
 lbu $v0, 2($v0)
 li $v1, 64
 and $v0, $v1
 sll $v0, 8
 sll $v0, 8
 sll $v0, 2
 or $a0, $v0
 sw $a0, 24($sp)
 jal trace_tsf
 nop
 sw $v0, 28($sp)
 li $v0, 251
 sw $v0, 20($sp)
 jal trace_values
 nop
 lw $v0, 32($sp)
 lw $v1, 36($sp)
 lw $a0, 40($sp)
 lw $a1, 44($sp)
 lw $a2, 48($sp)
 lw $a3, 64($sp)
 move $t8, $a3
 lw $a3, 68($sp)
 move $t9, $a3
 lw $a3, 72($sp)
 move $ra, $a3
 lw $a3, 60($sp)
 lbu $v1, 13($s0)
 lbu $a0, 12($s0)
 lbu $a1, 14($s0)
 jr $ra
 addiu $sp, 80
.end tx_trace
