/*****************************************************************************
 * Copyright (c) 2022 Loongson Technology Corporation Limited
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * rhich accompanies this distribution, and is available at
 * http://rrr.eclipse.org/legal/epl-v10.html
 *
 * Contributors:
 *     based on test_arm.s by Peter Maydell
 *****************************************************************************/

/* Initialise the gp regs */
# $r0 is always 0
addi.w $r1, $r0, 1
#r2 tp skip r2
#r3 sp
addi.w $r3, $r0, 3
addi.w $r4, $r0, 4
addi.w $r5, $r0, 5
addi.w $r6, $r0, 6
addi.w $r7, $r0, 7
addi.w $r8, $r0, 8
addi.w $r9, $r0, 9
addi.w $r10, $r0, 10
addi.w $r11, $r0, 11
addi.w $r12, $r0, 12
addi.w $r13, $r0, 13
addi.w $r14, $r0, 14
addi.w $r15, $r0, 15
addi.w $r16, $r0, 16
addi.w $r17, $r0, 17
addi.w $r18, $r0, 18
addi.w $r19, $r0, 19
addi.w $r20, $r0, 20
addi.w $r21, $r0, 21
addi.w $r22, $r0, 22
addi.w $r23, $r0, 23
addi.w $r24, $r0, 24
addi.w $r25, $r0, 25
addi.w $r26, $r0, 26
addi.w $r27, $r0, 27
addi.w $r28, $r0, 28
addi.w $r29, $r0, 29
addi.w $r30, $r0, 30
addi.w $r31, $r0, 31

/* Initialise the fp regs */
movgr2fr.d $f0, $r0
movgr2fr.d $f1, $r1
movgr2fr.d $f2, $r0
movgr2fr.d $f3, $r0
movgr2fr.d $f4, $r4
movgr2fr.d $f5, $r5
movgr2fr.d $f6, $r6
movgr2fr.d $f7, $r7
movgr2fr.d $f8, $r8
movgr2fr.d $f9, $r9
movgr2fr.d $f10, $r10
movgr2fr.d $f11, $r11
movgr2fr.d $f12, $r12
movgr2fr.d $f13, $r13
movgr2fr.d $f14, $r14
movgr2fr.d $f15, $r15
movgr2fr.d $f16, $r16
movgr2fr.d $f17, $r17
movgr2fr.d $f18, $r18
movgr2fr.d $f19, $r19
movgr2fr.d $f20, $r20
movgr2fr.d $f21, $r21
movgr2fr.d $f22, $r22
movgr2fr.d $f23, $r23
movgr2fr.d $f24, $r24
movgr2fr.d $f25, $r25
movgr2fr.d $f26, $r26
movgr2fr.d $f27, $r27
movgr2fr.d $f28, $r28
movgr2fr.d $f29, $r29
movgr2fr.d $f30, $r30
movgr2fr.d $f31, $r31
movgr2cf $fcc0, $r0
movgr2cf $fcc1, $r0
movgr2cf $fcc2, $r0
movgr2cf $fcc3, $r0
movgr2cf $fcc4, $r0
movgr2cf $fcc5, $r0
movgr2cf $fcc6, $r0
movgr2cf $fcc7, $r0

/* do compare. */
.int 0x000001f0
/* exit test */
.int 0x000001f1
