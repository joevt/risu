/******************************************************************************
 * Copyright (c) 2013 Linaro Limited
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * which accompanies this distribution, and is available at
 * http://www.eclipse.org/legal/epl-v10.html
 *
 * Contributors:
 *     Claudio Fontana (Linaro) - initial implementation
 *     based on Peter Maydell's risu_arm.c
 *****************************************************************************/

#ifndef RISU_REGINFO_AARCH64_H
#define RISU_REGINFO_AARCH64_H

#include <signal.h>

/* The kernel headers set this based on future arch extensions.
   The current arch maximum is 16.  Save space below.  */
#undef SVE_VQ_MAX
#define SVE_VQ_MAX 16

#define ROUND_UP(SIZE, POW2)    (((SIZE) + (POW2) - 1) & -(POW2))
#define RISU_SVE_REGS_SIZE(VQ)  ROUND_UP(SVE_SIG_REGS_SIZE(VQ), 16)
#define RISU_SIMD_REGS_SIZE     (32 * 16)

struct reginfo {
    uint64_t fault_address;
    uint64_t regs[31];
    uint64_t sp;
    uint64_t pc;
    uint32_t flags;
    uint32_t faulting_insn;

    /* FP/SIMD */
    uint32_t fpsr;
    uint32_t fpcr;
    uint16_t sve_vl;
    uint16_t reserved;

    char extra[RISU_SVE_REGS_SIZE(SVE_VQ_MAX)]
        __attribute__((aligned(16)));
};

static inline uint64_t *reginfo_vreg(struct reginfo *ri, int i)
{
    return (uint64_t *)&ri->extra[i * 16];
}

static inline uint64_t *reginfo_zreg(struct reginfo *ri, int vq, int i)
{
    return (uint64_t *)&ri->extra[SVE_SIG_ZREG_OFFSET(vq, i) -
                                  SVE_SIG_REGS_OFFSET];
}

static inline uint16_t *reginfo_preg(struct reginfo *ri, int vq, int i)
{
    return (uint16_t *)&ri->extra[SVE_SIG_PREG_OFFSET(vq, i) -
                                  SVE_SIG_REGS_OFFSET];
}

#endif /* RISU_REGINFO_AARCH64_H */
