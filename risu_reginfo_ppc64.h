/******************************************************************************
 * Copyright (c) IBM Corp, 2016
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * which accompanies this distribution, and is available at
 * http://www.eclipse.org/legal/epl-v10.html
 *
 * Contributors:
 *     Jose Ricardo Ziviani - initial implementation
 *     based on Claudio Fontana's risu_reginfo_aarch64
 *     based on Peter Maydell's risu_arm.c
 *****************************************************************************/

#ifndef RISU_REGINFO_PPC64_H
#define RISU_REGINFO_PPC64_H

#if __LP64__
    typedef uint64_t reg_t;
    #define PRIx "16" PRIx64
#else
    typedef uint32_t reg_t;
    #define PRIx "8" PRIx32
#endif

struct reginfo {
    uint32_t faulting_insn;
    uint32_t prev_insn;
    reg_t nip;
    gregset_t gregs;
    uint64_t fpregs[32];
    reg_t fpscr;
#ifdef VRREGS
    vrregset_t vrregs;
#endif
#ifdef SAVESTACK
    uint8_t stack[256];
#endif
};

#endif /* RISU_REGINFO_PPC64_H */
