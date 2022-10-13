/******************************************************************************
 * Copyright (c) 2022 Loongson Technology Corporation Limited
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * which accompanies this distribution, and is available at
 * http://www.eclipse.org/legal/epl-v10.html
 *
 * Contributors:
 *     based on Peter Maydell's risu_arm.c
 *****************************************************************************/

#include <asm/types.h>
#include <signal.h>
#include <asm/ucontext.h>

#include "risu.h"

void advance_pc(void *vuc)
{
    struct ucontext *uc = vuc;
    uc->uc_mcontext.sc_pc += 4;
}

void set_ucontext_paramreg(void *vuc, uint64_t value)
{
    struct ucontext *uc = vuc;
    uc->uc_mcontext.sc_regs[4] = value;
}

uint64_t get_reginfo_paramreg(struct reginfo *ri)
{
    return ri->regs[4];
}

RisuOp get_risuop(struct reginfo *ri)
{
    /* Return the risuop we have been asked to do
     * (or OP_SIGILL if this was a SIGILL for a non-risuop insn)
     */
    uint32_t insn = ri->faulting_insn;
    uint32_t op = insn & 0xf;
    uint32_t key = insn & ~0xf;
    uint32_t risukey = 0x000001f0;
    return (key != risukey) ? OP_SIGILL : op;
}

uintptr_t get_pc(struct reginfo *ri)
{
   return ri->pc;
}
