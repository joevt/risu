/******************************************************************************
 * Copyright (c) 2022 Loongson Technology Corporation Limited
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * which accompanies this distribution, and is available at
 * http://www.eclipse.org/legal/epl-v10.html
 *
 * Contributors:
 *     based on Peter Maydell's risu_reginfo_arm.h
 *****************************************************************************/

#ifndef RISU_REGINFO_LOONGARCH64_H
#define RISU_REGINFO_LOONGARCH64_H

struct reginfo {
    uint64_t regs[32];
    uint64_t pc;
    uint64_t fcc;
    uint32_t flags;
    uint32_t fcsr;
    uint32_t faulting_insn;
    uint64_t fpregs[32];
};

#endif /* RISU_REGINFO_LOONGARCH64_H */
