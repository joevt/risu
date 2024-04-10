/******************************************************************************
 * Copyright (c) IBM Corp, 2016
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * which accompanies this distribution, and is available at
 * http://www.eclipse.org/legal/epl-v10.html
 *
 * Contributors:
 *     Jose Ricardo Ziviani - initial implementation
 *     based on Claudio Fontana's risu_aarch64.c
 *     based on Peter Maydell's risu_arm.c
 *****************************************************************************/

#include <sys/user.h>

#include "risu.h"
#ifdef RISU_DPPC
    #include "ppcemu.h"
#endif

void advance_pc(void *vuc)
{
#if defined(RISU_DPPC)
    //We don't need to change the pc because ppctest-risu treats illegal ops as normal ops.
    //ppc_state.pc += 4;
#elif defined(__APPLE__)
    ucontext_t *uc = (ucontext_t *) vuc;
    uc->uc_mcontext->ss.srr0 += 4;
#else
    ucontext_t *uc = (ucontext_t *) vuc;
    uc->uc_mcontext.regs->nip += 4;
#endif
}

arch_ptr_t get_uc_pc(void *vuc, void *siaddr)
{
#if defined(RISU_DPPC)
    return ppc_state.pc;
#elif defined(__APPLE__)
    ucontext_t *uc = (ucontext_t *) vuc;
    return uc->uc_mcontext->ss.srr0;
#else
    ucontext_t *uc = (ucontext_t *) vuc;
    return uc->uc_mcontext.regs->nip;
#endif
}

void set_ucontext_paramreg(void *vuc, arch_ptr_t value)
{
#if defined(RISU_DPPC)
    ppc_state.gpr[0] = (uint32_t)value;
#elif defined(__APPLE__)
    ucontext_t *uc = vuc;
    uc->uc_mcontext->ss.r0 = value;
#else
    ucontext_t *uc = vuc;
    uc->uc_mcontext.gp_regs[0] = value;
#endif
}

arch_ptr_t get_reginfo_paramreg(struct reginfo *ri)
{
    return ri->gregs[0];
}

RisuOp get_risuop(struct reginfo *ri)
{
    uint32_t insn = ri->faulting_insn;
    uint32_t op = insn & 0xf;
    uint32_t key = insn & ~0xf;
    uint32_t risukey = 0x00005af0;
    return (RisuOp)((key != risukey) ? OP_SIGILL : op);
}

arch_ptr_t get_pc(struct reginfo *ri)
{
#if defined(RISU_DPPC)
   return ppc_state.pc;
#else
   return ri->nip;
#endif
}

bool get_arch_big_endian()
{
#if defined(RISU_DPPC) || defined(__BIG_ENDIAN__)
    return true;
#else
    return false;
#endif
}

uint32_t arch_to_host_32(uint32_t val)
{
#if (defined(RISU_DPPC) || defined(__BIG_ENDIAN__)) == defined(__BIG_ENDIAN__)
    return val;
#else
    return BYTESWAP_32(val);
#endif
}

#if defined(RISU_DPPC)
void ppc_exception_handler_risu(Except_Type exception_type, uint32_t srr1_bits) {
    arch_siginfo_t si;
    memset(&si, 0, sizeof(si));
    si.si_addr = (void*)(size_t)ppc_state.pc;
    if (exception_type == Except_Type::EXC_PROGRAM)
        sig_handler(SIGILL, &si, NULL);
    else {
        sig_handler(SIGBUS, &si, NULL);
    }
}
#endif
