/* SPDX-License-Identifier: GPL-2.0 */
.set noreorder
.set mips16
.text
.balign 4
.globl big_dispatch_entry
.ent big_dispatch_entry
big_dispatch_entry:
 addiu $sp,-24
 sw $v1,20($sp)
 jal u2_bigtk_dispatch
 nop
 lw $a3,20($sp)
 jr $a3
 addiu $sp,24
.end big_dispatch_entry

.balign 4
.globl big_faw_begin_entry
.ent big_faw_begin_entry
big_faw_begin_entry:
 addiu $sp,-24
 sw $v1,20($sp)
 jal u2_nan_faw_begin
 nop
 lw $a3,20($sp)
 jr $a3
 addiu $sp,24
.end big_faw_begin_entry

/* Original begin prologue; the owner wrapper holds the NAN critical section. */
.balign 4
.globl big_native_faw_begin
.ent big_native_faw_begin
big_native_faw_begin:
 addiu $sp,-64
 sw $ra,60($sp)
 sw $s1,56($sp)
 sw $s0,52($sp)
 li $a3,0xb89b
 sll $a3,8
 sll $a3,8
 addiu $a3,-15047
 jr $a3
 nop
.end big_native_faw_begin

.balign 4
.globl big_native_dispatch
.ent big_native_dispatch
big_native_dispatch:
 addiu $sp,-24
 sw $ra,20($sp)
 sw $s0,16($sp)
 move $v0,$a0
 li $a3,0xb89a
 sll $a3,8
 sll $a3,8
 li $v1,0xddbd
 or $a3,$v1
 jr $a3
 nop
.end big_native_dispatch

/* tx_trace already owns an 80-byte frame; retain its raw register snapshot. */
.balign 4
.globl big_trace_done
.ent big_trace_done
big_trace_done:
 sw $ra,76($sp)
 jal 0xb89bb8f4
 nop
 jal u2_bigtk_done
 lw $a0,24($sp)
 lw $a3,76($sp)
 jr $a3
 nop
.end big_trace_done

.macro native name, address
.balign 4
.globl \name
.ent \name
\name:
 li $v0,(\address >> 16)
 sll $v0,8
 sll $v0,8
 li $v1,(\address & 0xffff)
 or $v0,$v1
 jr $v0
 nop
.end \name
.endm
native big_enter,0xb89ba75d
native big_leave,0xb89ba7a1
native big_native_prepare,0xb89bb91d
native big_native_gate,0xb89bb9fd
native big_queue_lock,0xb89a3fb9
native big_queue_unlock,0xb89a4061
native big_queued,0xb89a409d
native big_macid_gate,0xb8979c0d
native big_native_faw_end,0xb89a6841
native big_native_faw_channel,0xb89a2c45
