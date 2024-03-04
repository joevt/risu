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

#if defined(__LP64__) && !defined(DPPC)
    typedef uint64_t arch_ptr_t;
    typedef uint64_t reg_t;
    #define PRIx "16" PRIx64
    #define PRIxARCHPTR PRIx32
#else
    typedef uint32_t arch_ptr_t;
    typedef uint32_t reg_t;
    #define PRIx "8" PRIx32
    #define PRIxARCHPTR PRIx32
#endif

enum {
    risu_NIP = 32,
    risu_MSR,
    risu_CTR,
    risu_LNK,
    risu_XER,
    risu_CCR,
    risu_MQ,
    risu_DAR,
    risu_DSISR,
    risu_NGREG,
};

typedef reg_t risu_gregset_t[risu_NGREG];

typedef struct {
    uint32_t vrregs[32][4];
    uint32_t vscr[4];
    uint32_t vrsave;
    uint32_t vrsave2;
    uint32_t save_vrvalid;
} risu_vrregset_t;

struct reginfo {
    uint32_t second_prev_insn;
    uint32_t prev_insn;
    uint32_t faulting_insn;
    uint32_t next_insn;
    uint32_t nip;
    risu_gregset_t gregs;
    uint64_t fpregs[32] __attribute__((packed));
    reg_t fpscr;
#ifdef VRREGS
    risu_vrregset_t vrregs;
#endif
#ifdef SAVESTACK
    uint8_t stack[256];
#endif
};

#endif /* RISU_REGINFO_PPC64_H */
