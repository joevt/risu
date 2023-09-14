/******************************************************************************
 * Copyright 2023 Red Hat Inc.
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * which accompanies this distribution, and is available at
 * http://www.eclipse.org/legal/epl-v10.html
 *
 * Contributors:
 *     Thomas Huth - initial implementation
 *****************************************************************************/

#include <sys/user.h>

#include "risu.h"

void advance_pc(void *vuc)
{
    /*
     * Note: The PSW address already points to the next instruction
     * after we get a SIGILL, so we must not advance it here!
     */
}

void set_ucontext_paramreg(void *vuc, uint64_t value)
{
    ucontext_t *uc = vuc;
    uc->uc_mcontext.gregs[0] = value;
}

uint64_t get_reginfo_paramreg(struct reginfo *ri)
{
    return ri->gprs[0];
}

RisuOp get_risuop(struct reginfo *ri)
{
    uint32_t insn = ri->faulting_insn;
    uint32_t op = insn & 0xff;
    uint32_t key = insn & ~0xff;

    if (ri->faulting_insn_len == 4 && key == 0x835a0f00) {
        return op;
    }

    return OP_SIGILL;
}

uintptr_t get_pc(struct reginfo *ri)
{
   return ri->pc_offset;
}
