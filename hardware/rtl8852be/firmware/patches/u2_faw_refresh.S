/* SPDX-License-Identifier: GPL-2.0 */
/* Rebuild missed FAW candidates before native window selection. */
.set noreorder
.set mips16
.text

.macro base reg
 li \reg, 0xb8e1
 sll \reg, 8
 sll \reg, 8
.endm

.globl faw_refresh
.type faw_refresh,@function
.ent faw_refresh
faw_refresh:
 addiu $sp, -64
 sw $v1, 60($sp)
 sw $s0, 56($sp)
 sw $s1, 52($sp)
 sw $a0, 32($sp)
 /* The native selector is called with the NAN critical section held. */
 base $v0
 lbu $v1, 0x6e2d($v0)
 li $a1, 7
 and $v1, $a1
 cmpi $v1, 7
 btnez select_window
 lw $s0, 0x6e10($v0)
 lw $v1, 120($s0)
 beqz $v1, select_window
 lhu $v1, 0x6ae2($v0)
 beqz $v1, select_window
 /* Both queued starts and this clock are in the free-running domain. */
 lw $v0, -25612($v0)
 jalr $v0
 li $a0, 0
 lw $a1, 8($s0)
 sltu $v1, $a1
 btnez select_window
 cmp $v1, $a1
 btnez refresh_records
 lw $a1, 4($s0)
 sltu $v0, $a1
 btnez select_window
refresh_records:
 sw $v0, 28($sp)
 lw $v1, 4($s0)
 sw $v1, 24($sp)
 li $v0, 247
 sw $v0, 20($sp)
 li $v0, 3
 sw $v0, 16($sp)
 base $v1
 lw $a0, 0x4b10($v1)
 lw $a1, 0x4b14($v1)
 lw $v0, -26384($v1)
 li $a3, 857
 jalr $v0
 li $a2, 2
 li $s0, 0
 base $s1
 addiu $s1, 0x6ae0
record:
 lhu $v0, 2($s1)
 li $v1, 1
 sllv $v1, $s0
 and $v0, $v1
 beqz $v0, next_record
 li $v0, 108
 mult $s0, $v0
 mflo $v0
 addu $v0, $s1, $v0
 lbu $a1, 4($v0)
 jal 0xb89ad6c0
 move $a0, $s1
 base $v1
 lw $v1, 0x454c($v1)
 cmp $v0, $v1
 btnez done
next_record:
 addiu $s0, 1
 cmpi $s0, 5
 btnez record
 jal 0xb89ad420
 move $a0, $s1
 base $v1
 lw $v1, 0x454c($v1)
 cmp $v0, $v1
 btnez done
select_window:
 jal select_body
 lw $a0, 32($sp)
done:
 lw $a3, 60($sp)
 lw $s0, 56($sp)
 lw $s1, 52($sp)
 jr $a3
 addiu $sp, 64
.end faw_refresh

.balign 4
.type select_body,@function
.ent select_body
select_body:
 addiu $sp, -72
 sw $ra, 68($sp)
 sw $s1, 64($sp)
 sw $s0, 60($sp)
 li $a3, 0xb89b
 sll $a3, 8
 sll $a3, 8
 addiu $a3, -19839
 jr $a3
 nop
.end select_body
