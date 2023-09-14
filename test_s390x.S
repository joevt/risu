/*****************************************************************************
 * Copyright 2023 Red Hat Inc.
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * which accompanies this distribution, and is available at
 * http://www.eclipse.org/legal/epl-v10.html
 *
 * Contributors:
 *     Thomas Huth - initial implementation
 *****************************************************************************/

    /* Initialise the general purpose registers */
    lgfi %r0, 0
    lgfi %r1, 0x1111111
    lgfi %r2, 0x2222222
    lgfi %r3, 0x3333333
    lgfi %r4, 0x4444444
    lgfi %r5, 0x5555555
    lgfi %r6, 0x6666666
    lgfi %r7, 0x7777777
    lgfi %r8, 0x8888888
    lgfi %r9, 0x9999999
    lgfi %r10, 0xaaaaaaa
    lgfi %r11, 0xbbbbbbb
    lgfi %r12, 0xccccccc
    lgfi %r13, 0xddddddd
    lgfi %r14, 0xeeeeeee
    lgfi %r15, 0xfffffff

    /* Initialize floating point registers */
    ldgr %f0,%r0
    ldgr %f1,%r1
    ldgr %f2,%r2
    ldgr %f3,%r3
    ldgr %f4,%r4
    ldgr %f5,%r5
    ldgr %f6,%r6
    ldgr %f7,%r7
    ldgr %f8,%r8
    ldgr %f9,%r9
    ldgr %f10,%r10
    ldgr %f11,%r11
    ldgr %f12,%r12
    ldgr %f13,%r13
    ldgr %f14,%r14
    ldgr %f15,%r15

    /* do compare */
    .int 0x835a0f00
    nop

    /* exit test */
    .int 0x835a0f01
