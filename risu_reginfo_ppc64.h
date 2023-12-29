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

#ifdef __APPLE__
    #define NIP     32
    #define MSR     33
    #define origR3  34
    #define CTR     35
    #define LNK     36
    #define XER     37
    #define CCR     38
    #define MQ      39
    #define TRAP    40
    #define DAR     41
    #define DSISR   42
    #define RESULT  43
    #define DSCR    44
    #define NGREG   45

    typedef reg_t gregset_t[NGREG];

    typedef struct {
        uint32_t vrregs[32][4];
        uint32_t vscr[4];
        uint32_t vrsave;
        uint32_t vrsave2;
        uint32_t save_vrvalid;
    } vrregset_t;
#endif

struct reginfo {
    uint32_t second_prev_insn;
    uint32_t prev_insn;
    uint32_t faulting_insn;
    uint32_t next_insn;
    uint32_t nip;
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
