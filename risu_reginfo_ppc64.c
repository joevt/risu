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

#include "risu.h"
#include <stdio.h>
#ifdef NO_SIGNAL
#else
#include <signal.h>
#include <ucontext.h>
#endif
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <assert.h>
#ifdef RISU_MACOS9
#include <MachineExceptions.h>
#else
#include <sys/user.h>
#endif
#include <float.h>
#include <cinttypes>

#ifdef RISU_DPPC
    #include "risu_reginfo_dppc.h"
    #include "../../ppcemu.h"
    enum {
        DPPC_RISU_ROM_START = 0xf0000000,
        DPPC_RISU_RAM_START = 0xb0000000,
        DPPC_RISU_STACK     = 0xbfff79b0,
        DPPC_RISU_RAM_SIZE  = 0x20000000,
    };
    MemCtrlBase *mem_ctrl = NULL;
#else
    #include "risu_reginfo_ppc64.h"
#endif
#include "endianswap.h"

#if !defined(RISU_DPPC) && !defined(__APPLE__)
    static uint32_t gregs_mask = ~((1 << (31-1)) | (1 << (31-13))); /* Bit mask of GP registers to compare. */ /* ignore r1 and r13 */
#else
    static uint32_t gregs_mask = ~(1 << (31-1)); /* Bit mask of GP registers to compare. */ /* ignore r1 */
#endif
static uint32_t ccr_mask    = 0xFFFFFFFF; /* Bit mask of CCR bits to compare. */
static uint32_t xer_mask    = 0xFFFFFFFF; /* Bit mask of XER bits to compare. */
static uint32_t mq_mask     = 0xFFFFFFFF; /* Bit mask of MQ bits to compare. */
static uint32_t fpscr_mask  = 0xFFFFFFFF; /* Bit mask of FPSCR bits to compare. */
static uint32_t fpregs_mask = 0xFFFFFFFF; /* Bit mask of FP registers to compare. */
static uint32_t vrregs_mask = 0xFFFFFFFF; /* Bit mask of FP registers to compare. */
static uint32_t fp_opts     = 0; /* option bits. 1 = ignore NaN sign */
static uint32_t cpu_opts    = 0;

enum {
    fp_opt_ignore_QNaN_signs            = 0x00000001,
    fp_opt_ignore_QNaN_values           = 0x00000002,
    fp_opt_ignore_QNaN_diffs            = 0x00000004,
    fp_opt_ignore_QNaN_load_float       = 0x00000008,
    fp_opt_ignore_NaN_operand           = 0x00000010,
    fp_opt_ignore_opposite_inf_operands = 0x00000020,
    fp_opt_ignore_inf_x_0_operands      = 0x00000040,
    fp_opt_ignore_div_zero              = 0x00000080,
    fp_opt_ignore_underflow             = 0x00000100,
    fp_opt_ignore_overflow              = 0x00000200,
    fp_opt_ignore_round                 = 0x00000400,
    fp_opt_ignore_round_s               = 0x00000800,
    fp_opt_ignore_rsqrt_negative        = 0x00001000,
    fp_opt_ignore_rsqrt_negative_zero   = 0x00002000,
    fp_opt_ignore_rsqrt_zero            = 0x00004000,
    fp_opt_ignore_qnan_from_inf         = 0x00008000,
    fp_opt_ignore_qnan_from_unknown     = 0x00010000,
    fp_opt_ignore_inf_from_unknown      = 0x00020000,
    fp_opt_ignore_zero_signs            = 0x00040000,
    fp_opt_ignore_inf_div_inf           = 0x00080000,
    fp_opt_ignore_QNaN_SNaN             = 0x00100000,
    fp_opt_ignore_NaN_unknown_operands  = 0x00200000,
};

enum {
    cpu_opt_include_601 = 0x00000001,
    cpu_opt_use_601     = 0x00000002,
};

static const struct option extra_opts[] = {
    {"ccr_mask"   , required_argument, NULL, FIRST_ARCH_OPT + 0 },
    {"fpscr_mask" , required_argument, NULL, FIRST_ARCH_OPT + 1 },
    {"fpregs_mask", required_argument, NULL, FIRST_ARCH_OPT + 2 },
    {"vrregs_mask", required_argument, NULL, FIRST_ARCH_OPT + 3 },
    {"fp_opts",     required_argument, NULL, FIRST_ARCH_OPT + 4 },
    {"gregs_mask" , required_argument, NULL, FIRST_ARCH_OPT + 5 },
    {"xer_mask"   , required_argument, NULL, FIRST_ARCH_OPT + 6 },
    {"mq_mask"    , required_argument, NULL, FIRST_ARCH_OPT + 7 },
    {"cpu_opts"   , required_argument, NULL, FIRST_ARCH_OPT + 8 },
    {0, 0, 0, 0}
};

const struct option * const arch_long_opts = &extra_opts[0];
const char * const arch_extra_help =
    "  --gregs_mask=MASK  Mask of gregs to compare\n"
    "  --fpregs_mask=MASK Mask of fpregs to compare\n"
    "  --vrregs_mask=MASK Mask of vrregs to compare\n"
    "  --ccr_mask=MASK    Mask of CCR bits to compare\n"
    "  --xer_mask=MASK    Mask of XER bits to compare\n"
    "  --mq_mask=MASK     Mask of MQ bits to compare\n"
    "  --fpscr_mask=MASK  Mask of FPSCR bits to compare\n"
    "  --fp_opts=OPTS     Options for comparing fpregs\n"
    "  --cpu_opts=OPTS    CPU options\n"
    ;

void process_arch_opt(int opt, const char *arg)
{
    assert(opt >= FIRST_ARCH_OPT && opt <= FIRST_ARCH_OPT + 8);
    uint32_t val = (uint32_t)strtoul(arg, 0, 16);
    switch (opt - FIRST_ARCH_OPT) {
        case 0: ccr_mask    = val; break;
        case 1: fpscr_mask  = val; break;
        case 2: fpregs_mask = val; break;
        case 3: vrregs_mask = val; break;
        case 4: fp_opts     = val; break;
        case 5: gregs_mask  = val; break;
        case 6: xer_mask    = val; break;
        case 7: mq_mask     = val; break;
        case 8: cpu_opts    = val; break;
    }
}

arch_ptr_t get_arch_start_address() {
#ifdef RISU_DPPC
    return DPPC_RISU_ROM_START;
#else
    return (arch_ptr_t)image_start_address;
#endif
}

void * get_arch_memory(arch_ptr_t arch_memblock) {
#ifdef RISU_DPPC
    AddressMapEntry* ref_entry;
    ref_entry = mem_ctrl->find_range(arch_memblock);
    if (!ref_entry)
        return NULL;

    uint32_t load_offset = arch_memblock - ref_entry->start;

    return ref_entry->mem_ptr + load_offset;
#else
    return (void *)arch_memblock;
#endif
}

void arch_init(void)
{
#ifdef RISU_DPPC
    mem_ctrl = new MemCtrlBase;

    // configure CPU clocks
    uint64_t bus_freq      = 50000000ULL;
    uint64_t timebase_freq = bus_freq / 4;

    // Initialize virtual CPU and request MPC750 CPU aka G3.
    // Add include_601 for running on Mac OS 9 which emulates MPC601 instructions.
    ppc_cpu_init(mem_ctrl, (cpu_opts & cpu_opt_use_601) ? PPC_VER::MPC601 : PPC_VER::MPC750, cpu_opts & cpu_opt_include_601, timebase_freq);

    mem_ctrl->add_ram_region(DPPC_RISU_RAM_START, DPPC_RISU_RAM_SIZE);
    mem_ctrl->add_rom_region(DPPC_RISU_ROM_START, (uint32_t)image_size);
    mem_ctrl->set_data(DPPC_RISU_ROM_START, (uint8_t*)image_start_address, (uint32_t)image_size);
    ppc_state.pc = DPPC_RISU_ROM_START;
    ppc_state.gpr[1] = DPPC_RISU_STACK;
#endif
}

void do_image()
{
    int i;
#ifdef RISU_DPPC
    uint8_t *stack_bytes = (uint8_t *)get_arch_memory(DPPC_RISU_STACK + 0x3C);
    for (i = 0; i < 32768; i++)
        stack_bytes[i] = i;

    power_on = true;
    ppc_exec();
#elif defined(RISU_MACOS9)
    uint8_t stack_bytes[32768];
    for (i = 0; i < sizeof(stack_bytes); i++)
        stack_bytes[i] = i;

    register uint8_t *stack = stack_bytes - 0x3C;
    register uintptr_t start = image_start_address;
    asm {
        addi r1, stack, 0
        mtctr start
        bctr
    }
#else
    // RISU_DPPC and RISU_MACOS9 assume that the image is executed with r1 (stack pointer)
    // pointing to stack_bytes - 0x3C. This is true for Mac OS X 10.4.11.
    uint8_t stack_bytes[32768];
    for (i = 0; i < sizeof(stack_bytes); i++)
        stack_bytes[i] = i;
    image_start();
#endif
}

int reginfo_size(struct reginfo *ri)
{
    return sizeof(*ri);
}

#if defined(__APPLE__) && defined(VRREGS)
static void savevec(void *vrregs, void *vscr, void *vrsave)
{
    assert((((int)vrregs & 15) | ((int)vscr & 15)) == 0);
    asm(
        "\n\t"
        "stvx v0, 0, %[addr]\n\t"
        "mfvscr v0\n\t"
        "stvx v0, 0, %[vscr]\n\t"
        "lvx  v0, 0, %[addr]\n\taddi %[addr], %[addr], 16\n\t"
        "stvx v1, 0, %[addr]\n\taddi %[addr], %[addr], 16\n\t"
        "stvx v2, 0, %[addr]\n\taddi %[addr], %[addr], 16\n\t"
        "stvx v3, 0, %[addr]\n\taddi %[addr], %[addr], 16\n\t"
        "stvx v4, 0, %[addr]\n\taddi %[addr], %[addr], 16\n\t"
        "stvx v5, 0, %[addr]\n\taddi %[addr], %[addr], 16\n\t"
        "stvx v6, 0, %[addr]\n\taddi %[addr], %[addr], 16\n\t"
        "stvx v7, 0, %[addr]\n\taddi %[addr], %[addr], 16\n\t"
        "stvx v8, 0, %[addr]\n\taddi %[addr], %[addr], 16\n\t"
        "stvx v9, 0, %[addr]\n\taddi %[addr], %[addr], 16\n\t"
        "stvx v10, 0, %[addr]\n\taddi %[addr], %[addr], 16\n\t"
        "stvx v11, 0, %[addr]\n\taddi %[addr], %[addr], 16\n\t"
        "stvx v12, 0, %[addr]\n\taddi %[addr], %[addr], 16\n\t"
        "stvx v13, 0, %[addr]\n\taddi %[addr], %[addr], 16\n\t"
        "stvx v14, 0, %[addr]\n\taddi %[addr], %[addr], 16\n\t"
        "stvx v15, 0, %[addr]\n\taddi %[addr], %[addr], 16\n\t"
        "stvx v16, 0, %[addr]\n\taddi %[addr], %[addr], 16\n\t"
        "stvx v17, 0, %[addr]\n\taddi %[addr], %[addr], 16\n\t"
        "stvx v18, 0, %[addr]\n\taddi %[addr], %[addr], 16\n\t"
        "stvx v19, 0, %[addr]\n\taddi %[addr], %[addr], 16\n\t"
        "stvx v20, 0, %[addr]\n\taddi %[addr], %[addr], 16\n\t"
        "stvx v21, 0, %[addr]\n\taddi %[addr], %[addr], 16\n\t"
        "stvx v22, 0, %[addr]\n\taddi %[addr], %[addr], 16\n\t"
        "stvx v23, 0, %[addr]\n\taddi %[addr], %[addr], 16\n\t"
        "stvx v24, 0, %[addr]\n\taddi %[addr], %[addr], 16\n\t"
        "stvx v25, 0, %[addr]\n\taddi %[addr], %[addr], 16\n\t"
        "stvx v26, 0, %[addr]\n\taddi %[addr], %[addr], 16\n\t"
        "stvx v27, 0, %[addr]\n\taddi %[addr], %[addr], 16\n\t"
        "stvx v28, 0, %[addr]\n\taddi %[addr], %[addr], 16\n\t"
        "stvx v29, 0, %[addr]\n\taddi %[addr], %[addr], 16\n\t"
        "stvx v30, 0, %[addr]\n\taddi %[addr], %[addr], 16\n\t"
        "stvx v31, 0, %[addr]\n\taddi %[addr], %[addr], 16\n\t"
        "mfspr %[addr], 256\n\t" /* vrsave */
        "stw %[addr], 0(%[vrsave])\n\t"
        : [addr] "+b" (vrregs) /* can be r1-r31 */
        : [vrsave] "b" (vrsave) /* can be r1-r31 */
        , [vscr] "b" (vscr) /* can be r1-r31 */
    );
}
#endif

/* reginfo_init: initialize with a ucontext */
void reginfo_init(struct reginfo *ri, void *vuc, void *siaddr)
{
#if defined(RISU_MACOS9)
    ExceptionInformation *uc = (ExceptionInformation *) vuc;
#elif defined(NO_SIGNAL)
    void *uc = vuc;
#else
    ucontext_t *uc = (ucontext_t *)vuc;
#endif

    int i;

    memset(ri, 0, sizeof(*ri));

    arch_ptr_t pc = get_uc_pc(uc, siaddr);
    arch_ptr_t ibegin = get_arch_start_address();
    arch_ptr_t iend = (arch_ptr_t)(get_arch_start_address() + image_size);
    uint8_t * pc_ptr = (uint8_t *)get_arch_memory(pc);
    ri->second_prev_insn = arch_to_host_32((pc - 8) >= ibegin && (pc - 8) < iend ? *((uint32_t *) (pc_ptr - 8)) : 0);
    ri->prev_insn        = arch_to_host_32((pc - 4) >= ibegin && (pc - 4) < iend ? *((uint32_t *) (pc_ptr - 4)) : 0);
    ri->faulting_insn    = arch_to_host_32((pc + 0) >= ibegin && (pc + 0) < iend ? *((uint32_t *) (pc_ptr + 0)) : 0);
    ri->next_insn        = arch_to_host_32((pc + 4) >= ibegin && (pc + 4) < iend ? *((uint32_t *) (pc_ptr + 4)) : 0);
    ri->nip = (uint32_t)(pc - ibegin);

#if defined(RISU_DPPC)
    for (i = 0; i < 32; i++) {
        ri->gregs[i] = ppc_state.gpr[i];
    }
    ri->gregs[risu_NIP  ] = pc;
    ri->gregs[risu_MSR  ] = ppc_state.msr;
    ri->gregs[risu_CTR  ] = ppc_state.spr[SPR::CTR];
    ri->gregs[risu_LNK  ] = ppc_state.spr[SPR::LR];
    ri->gregs[risu_XER  ] = ppc_state.spr[SPR::XER];
    ri->gregs[risu_CCR  ] = ppc_state.cr;
    ri->gregs[risu_MQ   ] = ppc_state.spr[SPR::MQ];
    ri->gregs[risu_DAR  ] = ppc_state.spr[SPR::DAR];
    ri->gregs[risu_DSISR] = ppc_state.spr[SPR::DSISR];
#elif defined(RISU_MACOS9)
    for (i = 0; i < 32; i++) {
        ri->gregs[i] = (&uc->registerImage->R0)[i].lo;
    }
    ri->gregs[risu_NIP  ] = pc;
    ri->gregs[risu_MSR  ] = uc->machineState->MSR;
    ri->gregs[risu_CTR  ] = uc->machineState->CTR.lo;
    ri->gregs[risu_LNK  ] = uc->machineState->LR.lo;
    ri->gregs[risu_XER  ] = uc->machineState->XER;
    ri->gregs[risu_CCR  ] = uc->machineState->CR;
    ri->gregs[risu_MQ   ] = uc->machineState->MQ;
    ri->gregs[risu_DAR  ] = uc->machineState->DAR.lo;
    ri->gregs[risu_DSISR] = uc->machineState->DSISR;
#elif defined(__APPLE__)
    for (i = 0; i < 32; i++) {
        if (sizeof(uc->uc_mcontext->ss.r0) == 8)
            ri->gregs[i] = ((uint64_t*)(&uc->uc_mcontext->ss.r0))[i];
        else
            ri->gregs[i] = ((uint32_t*)(&uc->uc_mcontext->ss.r0))[i];
    }
    ri->gregs[risu_NIP  ] = pc;
    ri->gregs[risu_MSR  ] = uc->uc_mcontext->ss.srr1; /* MSR */
    ri->gregs[risu_CTR  ] = uc->uc_mcontext->ss.ctr;
    ri->gregs[risu_LNK  ] = uc->uc_mcontext->ss.lr;
    ri->gregs[risu_XER  ] = uc->uc_mcontext->ss.xer;
    ri->gregs[risu_CCR  ] = uc->uc_mcontext->ss.cr;
    ri->gregs[risu_MQ   ] = uc->uc_mcontext->ss.mq;
    ri->gregs[risu_DAR  ] = uc->uc_mcontext->es.dar; /* DAR */
    ri->gregs[risu_DSISR] = uc->uc_mcontext->es.dsisr; /* DSISR */
#else
    for (i = 0; i < 32; i++) {
        ri->gregs[i] = uc->uc_mcontext.gp_regs[i];
    }
    ri->gregs[risu_NIP  ] = pc;
    ri->gregs[risu_MSR  ] = uc->uc_mcontext.gp_regs[MSR  ];
    ri->gregs[risu_CTR  ] = uc->uc_mcontext.gp_regs[CTR  ];
    ri->gregs[risu_LNK  ] = uc->uc_mcontext.gp_regs[LNK  ];
    ri->gregs[risu_XER  ] = uc->uc_mcontext.gp_regs[XER  ];
    ri->gregs[risu_CCR  ] = uc->uc_mcontext.gp_regs[CCR  ];
    ri->gregs[risu_MQ   ] = uc->uc_mcontext.gp_regs[MQ   ];
    ri->gregs[risu_DAR  ] = uc->uc_mcontext.gp_regs[DAR  ];
    ri->gregs[risu_DSISR] = uc->uc_mcontext.gp_regs[DSISR];
#endif

#if defined(RISU_DPPC)
    memcpy(ri->fpregs, ppc_state.fpr, 32 * sizeof(double));
    ri->fpscr = ppc_state.fpscr;
#elif defined(RISU_MACOS9)
    memcpy(ri->fpregs, uc->FPUImage->Registers, 32 * sizeof(double));
    ri->fpscr = uc->FPUImage->FPSCR;
#elif defined(__APPLE__)
    memcpy(ri->fpregs, uc->uc_mcontext->fs.fpregs, 32 * sizeof(double));
    ri->fpscr = uc->uc_mcontext->fs.fpscr;
#else
    memcpy(ri->fpregs, uc->uc_mcontext.fp_regs, 32 * sizeof(double));
    ri->fpscr = uc->uc_mcontext.fp_regs[32];
#endif

#ifdef VRREGS
#if defined(RISU_DPPC)
#elif defined(RISU_MACOS9)
#elif defined(__APPLE__)
    if (uc->uc_mcsize >= (sizeof(struct mcontext))) {
        memcpy(ri->vrregs.vrregs, uc->uc_mcontext->vs.save_vr,
               sizeof(ri->vrregs.vrregs[0]) * 32);
        memcpy(ri->vrregs.vscr, uc->uc_mcontext->vs.save_vscr,
               sizeof(ri->vrregs.vscr[0]) * 4);
        ri->vrregs.save_vrvalid = uc->uc_mcontext->vs.save_vrvalid;
        ri->vrregs.vrsave2 = 0;
    }
    else if (vrregs_mask) {
        static vrregset_t vrregs __attribute__((aligned(16)));
        savevec(&vrregs.vrregs, &vrregs.vscr, &vrregs.vrsave2);
        memcpy(&ri->vrregs, &vrregs, sizeof(vrregs));
        ri->vrregs.save_vrvalid = 0;
    }
    ri->vrregs.vrsave = uc->uc_mcontext->ss.vrsave;
#else
    memcpy(ri->vrregs.vrregs, uc->uc_mcontext.v_regs->vrregs,
           sizeof(ri->vrregs.vrregs[0]) * 32);
    ri->vrregs.vscr = uc->uc_mcontext.v_regs->vscr;
    ri->vrregs.vrsave = uc->uc_mcontext.v_regs->vrsave;
#endif
#endif

#ifdef SAVESTACK
    uint8_t *r1_ptr = (uint8_t *)get_arch_memory(ri->gregs[1]);
    memcpy(ri->stack, r1_ptr - (sizeof(ri->stack) / 2), sizeof(ri->stack));
#endif
}

/* reginfo_update: update the context */
void reginfo_update(struct reginfo *ri, void *vuc, void *siaddr)
{
    #pragma unused(siaddr)
#if defined(RISU_DPPC)
    ppc_state.cr = ri->gregs[risu_CCR];
    ppc_state.spr[SPR::XER] = ri->gregs[risu_XER];
    ppc_state.spr[SPR::MQ] = ri->gregs[risu_MQ];
#elif defined(RISU_MACOS9)
    ExceptionInformation *uc = (ExceptionInformation *) vuc;
    uc->machineState->CR = ri->gregs[risu_CCR];
    uc->machineState->XER = ri->gregs[risu_XER];
    uc->machineState->MQ = ri->gregs[risu_MQ];
#elif defined(__APPLE__)
    ucontext_t *uc = (ucontext_t *)vuc;
    uc->uc_mcontext->ss.cr = ri->gregs[risu_CCR];
    uc->uc_mcontext->ss.xer = ri->gregs[risu_XER];
    uc->uc_mcontext->ss.mq = ri->gregs[risu_MQ];
#else
    ucontext_t *uc = (ucontext_t *)vuc;
    uc->uc_mcontext.gp_regs[CR] = ri->gregs[risu_CCR];
    uc->uc_mcontext.gp_regs[XER] = ri->gregs[risu_XER];
    uc->uc_mcontext.gp_regs[MQ] = ri->gregs[risu_MQ];
#endif

    int i;
    for (i = 0; i < 32; i++) {
        if ((1 << (31-i)) & ~gregs_mask) {
            continue;
        }
#if defined(RISU_DPPC)
        ppc_state.gpr[i] = ri->gregs[i];
#elif defined(RISU_MACOS9)
    (&uc->registerImage->R0)[i].lo = ri->gregs[i];
#elif defined(__APPLE__)
        if (sizeof(uc->uc_mcontext->ss.r0) == 8)
            ((uint64_t*)(&uc->uc_mcontext->ss.r0))[i] = ri->gregs[i];
        else
            ((uint32_t*)(&uc->uc_mcontext->ss.r0))[i] = ri->gregs[i];
#else
        uc->uc_mcontext.gp_regs[i] = ri->gregs[i];
#endif
    }

#if defined(RISU_DPPC)
    memcpy(ppc_state.fpr, ri->fpregs, 32 * sizeof(double));
    ppc_state.fpscr = ri->fpscr;
#elif defined(RISU_MACOS9)
    memcpy(uc->FPUImage->Registers, ri->fpregs, 32 * sizeof(double));
    uc->FPUImage->FPSCR = ri->fpscr;
#elif defined(__APPLE__)
    memcpy(uc->uc_mcontext->fs.fpregs, ri->fpregs, 32 * sizeof(double));
    uc->uc_mcontext->fs.fpscr = ri->fpscr;
#else
    memcpy(uc->uc_mcontext.fp_regs, ri->fpregs, 32 * sizeof(double));
    uc->uc_mcontext.fp_regs[32] = ri->fpscr;
#endif

#ifdef VRREGS
#if defined(RISU_DPPC)
#elif defined(RISU_MACOS9)
#elif defined(__APPLE__)
    memcpy(uc->uc_mcontext->vs.save_vscr, ri->vrregs.vscr,
           sizeof(ri->vrregs.vscr[0]) * 4);
#else
    uc->uc_mcontext.v_regs->vscr = ri->vrregs.vscr;
#endif
#endif
}

bool denormalized(uint64_t n) {
    return (((n >> 52) & 0x7ff) == 0) && ((n & ~(1LL<<63)) != 0); // exponent is zero and mantissa is not zero
}

bool zero(uint64_t n) {
    return (n & ~(1LL<<63)) == 0;
}

uint64_t signz(uint64_t n) {
    return (n & (1LL<<63));
}

uint64_t signinf(uint64_t n) {
    return ((n & (1LL<<63)) | 0x7ff0000000000000ULL);
}

bool negative(uint64_t n) {
    return (n >> 63);
}

bool infinite(uint64_t n) {
    return (n & ~(1LL<<63)) == 0x7ff0000000000000ULL;
}

bool is_nan(uint64_t n) {
    return (n & ~(1LL<<63)) > 0x7ff0000000000000ULL;
}

bool isqnan(uint64_t n) {
    return (n & ~(1LL<<63)) == 0x7ff8000000000000ULL;
}

bool anyqnan(uint64_t n) {
    return (n & ~(1LL<<63)) >= 0x7ff8000000000000ULL;
}

bool neganyqnan(uint64_t n) {
    return n >= 0xfff8000000000000ULL;
}

bool snan(uint64_t n) {
    return is_nan(n) && !anyqnan(n);
}

bool qsnan(uint64_t n) {
    return (n & ~(1LL<<63)) > 0x7ff8000000000000ULL;
}

int64_t ABS(int64_t x) {
    return (((x) < 0) ? -(x) : (x));
}

int exponent(uint64_t n) {
    if (zero(n))
        return -4096;
    int exp = (int)((n >> 52) & 0x7ff);
    if (exp == 0x7ff)
        return 4096;
    if (exp == 0) {
        uint64_t x;
        for (x = n & ((1LL << 52) - 1); x < (1LL << 52); x <<= 1, exp--) {}
    }
    return exp - 1023;
}

uint64_t mantissa(uint64_t n) {
    return (n & ((1ULL<<52)-1)) + ((((n >> 52) & 0x7ff) == 0) ? (1ULL<<52) : 0);
}

uint64_t normalize(uint64_t n) {
    if (!denormalized(n))
        return n;

    int exp = 1536 + 1; // underflow adds 1536
    uint64_t x;
    for (x = n & ((1LL << 52) - 1); x < (1LL << 52); x <<= 1, exp--) {}
    return (n & (1LL<<63)) | ((uint64_t)exp << 52) | (x & ((1LL << 52) - 1));
}

/* reginfo_is_eq: compare the reginfo structs, returns nonzero if equal */
int reginfo_is_eq(struct reginfo *m, struct reginfo *a)
{
    uint32_t local_gregs_mask = gregs_mask;
    if (get_risuop(a) == OP_SIGILL) {
        if (
            (a->next_insn & 0xfc000000) == 0x38000000 || // addi or li (used by risugen_ppc)
            (a->next_insn & 0xfc0007ff) == 0x7c000050 || // subf       (used by risugen_ppc64)
            0
        ) {
            local_gregs_mask &= ~(1 << (31-((a->next_insn >> 21) & 31)));
        }
    }

    int rt = (m->prev_insn >> 21) & 31;
    int ra = (m->prev_insn >> 16) & 31;
    int rb = (m->prev_insn >> 11) & 31;
    int rc = (m->prev_insn >>  6) & 31;

    int i;
    for (i = 0; i < 32; i++) {
        if (m->gregs[i] != a->gregs[i]) {
            if ((1 << (31-i)) & ~local_gregs_mask) {
                /* a->gregs[i] = m->gregs[i]; */
            } else if (
                (
                    ((m->prev_insn & 0xfc1fffff) == 0x7c0802a6) && // mflr
                    rt == i &&
                    m->gregs[i] - m->gregs[risu_NIP] == a->gregs[i] - a->gregs[risu_NIP] // allow pc relative matching
                ) || (
                    ((m->prev_insn & 0xfc0003fe) == 0x7c000296) && // div[o][.]
                    rt == i &&
                    (
                        ra == rt || rb == rt || !m->gregs[rb] ||
                        ((((int64_t((uint64_t(m->gregs[ra]) << 32) | m->gregs[risu_MQ]) / int32_t(m->gregs[rb])) >> 31) + 1) & ~1)
                    )
                ) || (
                    ((m->prev_insn & 0xfc0003fe) == 0x7c0002d6) && // divs[o][.]
                    rt == i &&
                    (
                        ra == rt || rb == rt || !m->gregs[rb] ||
                        ((((int64_t(int32_t(m->gregs[ra])) / int32_t(m->gregs[rb])) >> 31) + 1) & ~1)
                    )
                ) || (
                    ((m->second_prev_insn & 0xfc0007fe) == 0x7c00022a) && // lscbx[.]
                    (m->gregs[risu_XER] & 3) &&
                    i == (((m->second_prev_insn >> 21) + ((m->gregs[risu_XER] & 0x7F) - 1) / 4) & 31) &&
                    !(uint32_t(m->gregs[i] ^ a->gregs[i]) >> (32 - 8 * (m->gregs[risu_XER] & 3)))
                )
            ) {
                a->gregs[i] = m->gregs[i];
            } else {
                return 0;
            }
        }
    }

    if (m->gregs[risu_XER] != a->gregs[risu_XER]) {
        if ((m->gregs[risu_XER] & xer_mask) == (a->gregs[risu_XER] & xer_mask)) {
            a->gregs[risu_XER] = m->gregs[risu_XER];
        } else {
            return 0;
        }
    }

    if (m->gregs[risu_LNK] - m->gregs[risu_NIP] != a->gregs[risu_LNK] - a->gregs[risu_NIP]) {
        return 0;
    }

    if (m->gregs[risu_CTR] != a->gregs[risu_CTR]) {
        return 0;
    }

    if (m->gregs[risu_CCR] != a->gregs[risu_CCR]) {
        uint32_t mask = ccr_mask;
        if ((m->prev_insn & 0xfc6007bf) == 0xfc000000) { /* fcmpo or fcmpu */
            int crf = (m->prev_insn >> 21) & 0x1c;
            int cr1 = 4;
            /* copy mask of cr1 to crf */
            mask = (mask & ~(15 << (28-crf))) | (((mask >> (28-cr1)) & 15) << (28-crf));
            mask &= ccr_mask;
        }
        else if ((m->prev_insn & 0xfc63ffff) == 0xfc000080) { /* mcrfs */
            int bf  = (m->prev_insn >> 21) & 0x1c;
            int bfa = (m->prev_insn >> 16) & 0x1c;
            /* copy mask of bfa to bf */
            mask = (mask & ~(15 << (28-bf))) | (((fpscr_mask >> (28-bfa)) & 15) << (28-bf));
            mask &= ccr_mask;
        }
        else if (
            ((m->prev_insn & 0xfc0003ff) == 0x7c000297) && // div[o].
            (
                ra == rt || rb == rt || !m->gregs[rb] ||
                ((( (int64_t((uint64_t(m->gregs[ra]) << 32) | m->gregs[risu_MQ]) / int32_t(m->gregs[rb])) >> 31) + 1) & ~1)
            )
        ) {
            mask = 0x1fffffff;
            mask &= ccr_mask;
        }
        if (
            (m->gregs[risu_CCR] & mask) == (a->gregs[risu_CCR] & mask)
        ) {
            a->gregs[risu_CCR] = m->gregs[risu_CCR];
        } else {
            return 0;
        }
    }

    if (m->gregs[risu_MQ] != a->gregs[risu_MQ]) {
        if (
              (
                  (m->gregs[risu_MQ] & mq_mask) == (a->gregs[risu_MQ] & mq_mask)
              ) ||
              (
                  ((m->prev_insn & 0xfc0003fe) == 0x7c000296) && // div[o][.]
                  (
                      ra == rt || rb == rt || !m->gregs[rb] ||
                      ((( (int64_t((uint64_t(m->gregs[ra]) << 32) | m->gregs[risu_MQ]) / int32_t(m->gregs[rb])) >> 31) + 1) & ~1)
                  )
              ) ||
              (
                  signal_count <= 1 // some test images don't set MQ so we'll let the first mismatch slide.
              )
        ) {
            a->gregs[risu_MQ] = m->gregs[risu_MQ];
        } else {
            return 0;
        }
    }

    if (m->fpscr != a->fpscr) {
        if ((m->fpscr & fpscr_mask) == (a->fpscr & fpscr_mask)) {
            a->fpscr = m->fpscr;
        } else {
            return 0;
        }
    }

    for (i = 0; i < 32; i++) {
        if (m->fpregs[i] != a->fpregs[i]) {
            if (
                ((1 << (31-i)) & ~fpregs_mask) ||
                (
                    (fp_opts & fp_opt_ignore_QNaN_signs) &&
                    anyqnan(a->fpregs[i]) &&
                    anyqnan(m->fpregs[i]) &&
                    ((a->fpregs[i] & ~(1ULL<<63)) == (m->fpregs[i] & ~(1ULL<<63)))
                ) ||
                (
                    (fp_opts & fp_opt_ignore_QNaN_values) &&
                    neganyqnan(a->fpregs[i]) &&
                    neganyqnan(m->fpregs[i])
                ) ||
                (
                    (fp_opts & fp_opt_ignore_QNaN_diffs) &&
                    anyqnan(a->fpregs[i]) &&
                    anyqnan(m->fpregs[i])
                ) ||
                (
                    (fp_opts & fp_opt_ignore_QNaN_load_float) &&
                    (
                        ((m->second_prev_insn & 0xfc0007fe) == 0x7c00046e) || /* lfsux */
                        ((m->second_prev_insn & 0xfc000000) == 0xc4000000) || /* lfsu */
                        ((m->second_prev_insn & 0xfc0007ff) == 0x7c00042e) || /* lfsx */
                        ((m->second_prev_insn & 0xfc000000) == 0xc0000000) || /* lfs */
                        0
                    ) &&
                    (((m->second_prev_insn >> 21) & 31) == i) &&
                    (m->fpregs[i] & 0xfff7ffffffffffffULL) == (a->fpregs[i] & 0xfff7ffffffffffffULL) &&
                    (m->fpregs[i] & 0x0008000000000000ULL) == 0
                ) ||
                (
                    (fp_opts & fp_opt_ignore_NaN_operand) &&
                    (
                        ((m->prev_insn & 0xfc0007fe) == 0xfc00002a) || /* fadd */
                        ((m->prev_insn & 0xfc0007fe) == 0xec00002a) || /* fadds */
                        ((m->prev_insn & 0xfc0007fe) == 0xfc000024) || /* fdiv */
                        ((m->prev_insn & 0xfc0007fe) == 0xec000024) || /* fdivs */
                        ((m->prev_insn & 0xfc0007fe) == 0xfc000028) || /* fsub */
                        ((m->prev_insn & 0xfc0007fe) == 0xec000028) || /* fsubs */
                        0
                    ) &&
                    (rt == i) &&
                    (ra != i) &&
                    (rb != i) &&
                    (
                        is_nan(m->fpregs[ra]) ||
                        is_nan(m->fpregs[rb]) ||
                        0
                    )
                ) ||
                (
                    (fp_opts & fp_opt_ignore_NaN_operand) &&
                    (
                        ((m->prev_insn & 0xfc1f07fe) == 0xfc000034) || /* frsqrte */
                        ((m->prev_insn & 0xfc1f07fe) == 0xfc000018) || /* frsp */
                        0
                    ) &&
                    (rt == i) &&
                    (rb != i) &&
                    (
                        is_nan(m->fpregs[rb]) ||
                        0
                    )
                ) ||
                (
                    (fp_opts & fp_opt_ignore_NaN_operand) &&
                    (
                        ((m->prev_insn & 0xfc00f83e) == 0xfc000032) || /* fmul */
                        ((m->prev_insn & 0xfc00f83e) == 0xec000032) || /* fmuls */
                        0
                    ) &&
                    (rt == i) &&
                    (ra != i) &&
                    (rc != i) &&
                    (
                        is_nan(m->fpregs[ra]) ||
                        is_nan(m->fpregs[rc]) ||
                        0
                    )
                ) ||
                (
                    (fp_opts & fp_opt_ignore_NaN_operand) &&
                    (
                        ((m->prev_insn & 0xfc00003e) == 0xfc00003a) || /* fmadd */
                        ((m->prev_insn & 0xfc00003e) == 0xfc000038) || /* fmsub */
                        ((m->prev_insn & 0xfc00003e) == 0xfc00003e) || /* fnmadd */
                        ((m->prev_insn & 0xfc00003e) == 0xfc00003c) || /* fnmsub */
                        ((m->prev_insn & 0xfc00003e) == 0xec00003a) || /* fmadds */
                        ((m->prev_insn & 0xfc00003e) == 0xec000038) || /* fmsubs */
                        ((m->prev_insn & 0xfc00003e) == 0xec00003e) || /* fnmadds */
                        ((m->prev_insn & 0xfc00003e) == 0xec00003c) || /* fnmsubs */
                        0
                    ) &&
                    (rt == i) &&
                    (ra != i) &&
                    (rb != i) &&
                    (rc != i) &&
                    (
                        is_nan(m->fpregs[ra]) ||
                        is_nan(m->fpregs[rb]) ||
                        is_nan(m->fpregs[rc]) ||
                        0
                    )
                ) ||
                (
                    (fp_opts & fp_opt_ignore_opposite_inf_operands) &&
                    (
                        ((m->prev_insn & 0xfc0007fe) == 0xfc00002a) || /* fadd */
                        ((m->prev_insn & 0xfc0007fe) == 0xec00002a) || /* fadds */
                        0
                    ) &&
                    (rt == i) &&
                    (ra != i) &&
                    (rb != i) &&
                    infinite(m->fpregs[ra]) &&
                    infinite(m->fpregs[rb]) &&
                    negative(m->fpregs[ra]) != negative(m->fpregs[rb])
                ) ||
                (
                    (fp_opts & fp_opt_ignore_opposite_inf_operands) &&
                    (
                        ((m->prev_insn & 0xfc0007fe) == 0xfc000028) || /* fsub */
                        ((m->prev_insn & 0xfc0007fe) == 0xec000028) || /* fsubs */
                        0
                    ) &&
                    (rt == i) &&
                    (ra != i) &&
                    (rb != i) &&
                    infinite(m->fpregs[ra]) &&
                    infinite(m->fpregs[rb]) &&
                    negative(m->fpregs[ra]) == negative(m->fpregs[rb])
                ) ||
                (
                    (fp_opts & fp_opt_ignore_opposite_inf_operands) &&
                    (rt == i) &&
                    (ra != i) &&
                    (rb != i) &&
                    (rc != i) &&
                    (exponent(m->fpregs[ra]) + exponent(m->fpregs[rc])) >= 2048 &&
                    infinite(m->fpregs[rb]) &&
                    (
                        (
                            (
                                ((m->prev_insn & 0xfc00003e) == 0xfc000038) || /* fmsub */
                                ((m->prev_insn & 0xfc00003e) == 0xec000038) || /* fmsubs */
                                ((m->prev_insn & 0xfc00003e) == 0xfc00003c) || /* fnmsub */
                                ((m->prev_insn & 0xfc00003e) == 0xec00003c) || /* fnmsubs */
                                0
                            ) &&
                            (negative(m->fpregs[ra]) != negative(m->fpregs[rc])) == negative(m->fpregs[rb])
                        ) ||
                        (
                            (
                                ((m->prev_insn & 0xfc00003e) == 0xfc00003a) || /* fmadd */
                                ((m->prev_insn & 0xfc00003e) == 0xfc00003e) || /* fnmadd */
                                ((m->prev_insn & 0xfc00003e) == 0xec00003a) || /* fmadds */
                                ((m->prev_insn & 0xfc00003e) == 0xec00003e) || /* fnmadds */
                                0
                            ) &&
                            (negative(m->fpregs[ra]) != negative(m->fpregs[rc])) != negative(m->fpregs[rb])
                        ) ||
                        0
                    )
                ) ||
                (
                    (fp_opts & fp_opt_ignore_inf_x_0_operands) &&
                    (
                        ((m->prev_insn & 0xfc00f83e) == 0xfc000032) || /* fmul */
                        ((m->prev_insn & 0xfc00003e) == 0xfc00003a) || /* fmadd */
                        ((m->prev_insn & 0xfc00003e) == 0xfc000038) || /* fmsub */
                        ((m->prev_insn & 0xfc00003e) == 0xfc00003e) || /* fnmadd */
                        ((m->prev_insn & 0xfc00003e) == 0xfc00003c) || /* fnmsub */
                        ((m->prev_insn & 0xfc00f83e) == 0xec000032) || /* fmuls */
                        ((m->prev_insn & 0xfc00003e) == 0xec00003a) || /* fmadds */
                        ((m->prev_insn & 0xfc00003e) == 0xec000038) || /* fmsubs */
                        ((m->prev_insn & 0xfc00003e) == 0xec00003e) || /* fnmadds */
                        ((m->prev_insn & 0xfc00003e) == 0xec00003c) || /* fnmsubs */
                        0
                    ) &&
                    (rt == i) &&
                    (ra != i) &&
                    (rc != i) &&
                    (
                        (
                            infinite(m->fpregs[ra]) &&
                            zero(m->fpregs[rc])
                        ) ||
                        (
                            zero(m->fpregs[ra]) &&
                            infinite(m->fpregs[rc])
                        )
                    )
                ) ||
                (
                    (fp_opts & fp_opt_ignore_div_zero) && // includes 0/0 though different results may be expected
                    (
                        ((m->prev_insn & 0xfc0007fe) == 0xec000024) || /* fdivs */
                        ((m->prev_insn & 0xfc0007fe) == 0xfc000024) || /* fdiv */
                        ((m->prev_insn & 0xfc1f07fe) == 0xec000030) || /* fres */
                        0
                    ) &&
                    (rt == i) &&
                    (rb != i) &&
                    zero(m->fpregs[rb])
                ) ||
                (
                    (fp_opts & fp_opt_ignore_underflow) &&
                    (
                        (
                            (
                                ((m->prev_insn & 0xfc1f07fe) == 0xfc000018) || /* frsp */
                                ((m->prev_insn & 0xfc00f83e) == 0xec000032) || /* fmuls */
                                ((m->prev_insn & 0xfc00003e) == 0xec00003c) || /* fnmsubs */
                                ((m->prev_insn & 0xfc00003e) == 0xec00003a) || /* fmadds */
                                ((m->prev_insn & 0xfc0007fe) == 0xec000024) || /* fdivs */
                                ((m->prev_insn & 0xfc00003e) == 0xec000038) || /* fmsubs */
                                ((m->prev_insn & 0xfc00003e) == 0xec00003e) || /* fnmadds */
                                ((m->prev_insn & 0xfc0007fe) == 0xec00002a) || /* fadds */
                                ((m->prev_insn & 0xfc0007fe) == 0xec000028) || /* fsubs */
                                ((m->prev_insn & 0xfc1f07fe) == 0xec000030) || /* fres */
                                0
                            ) &&
                            (rt == i) &&
                            (fabs(*(double*)(&m->fpregs[i])) < FLT_MIN) && // m is smaller than float
                            negative(a->fpregs[i]) == negative(m->fpregs[i]) && // same sign
                            exponent(a->fpregs[i]) < -126 // a is tiny
                        ) ||
                        (
                            (
                                ((m->prev_insn & 0xfc00003e) == 0xec000038) || /* fmsubs */
                                ((m->prev_insn & 0xfc00003e) == 0xec00003e) || /* fnmadds */
                                ((m->prev_insn & 0xfc00003e) == 0xec00003c) || /* fnmsubs */
                                ((m->prev_insn & 0xfc00f83e) == 0xec000032) || /* fmuls */
                                ((m->prev_insn & 0xfc00003e) == 0xec00003a) || /* fmadds */
                                
                                0
                            ) &&
                            (rt == i) &&
                            negative(a->fpregs[i]) == negative(m->fpregs[i]) && // same sign
                            exponent(a->fpregs[i]) < -126 &&
                            (
                                (
                                    (ra != i) &&
                                    (rc != i) &&
                                    (exponent(m->fpregs[ra]) + exponent(m->fpregs[rc])) < -126 // multiply underflow
                                ) ||
                                (ra == i) || (rc == i)
                            )
                        ) ||
                        (
                            (
                                ((m->prev_insn & 0xfc0007fe) == 0xec000024) || /* fdivs */
                                0
                            ) &&
                            (rt == i) &&
                            negative(a->fpregs[i]) == negative(m->fpregs[i]) && // same sign
                            exponent(a->fpregs[i]) < -126 &&
                            (
                                (
                                    (ra != i) &&
                                    (rb != i) &&
                                    (exponent(m->fpregs[ra]) - exponent(m->fpregs[rb])) <= -126 // divide underflow
                                ) ||
                                (ra == i) || (rb == i)
                            )
                        ) ||
                        (
                            (
                                ((m->prev_insn & 0xfc1f07fe) == 0xfc000018) || /* frsp */
                                0
                            ) &&
                            (rt == i) &&
                            (rb != i) &&
                            negative(a->fpregs[i]) == negative(m->fpregs[i]) && // same sign
                            exponent(a->fpregs[i]) < -126 &&
                            exponent(m->fpregs[rb]) < -126
                            // denormalized 36f0000000000000 is converted to 0 in G4 but not intel
                        ) ||
                        (
                            (
                                ((m->prev_insn & 0xfc1f07fe) == 0xfc000018) || /* frsp */
                                0
                            ) &&
                            (rt == i) &&
                            (rb == i) &&
                            negative(a->fpregs[i]) == negative(m->fpregs[i]) && // same sign
                            exponent(a->fpregs[i]) < -126 &&
                            exponent(m->fpregs[i]) < -126 + 192
                            // underflow exception adds 192 to exponent on PPC but not Intel
                        ) ||
                        (
                            (
                                ((m->prev_insn & 0xfc00003e) == 0xfc00003a) || /* fmadd */
                                ((m->prev_insn & 0xfc00003e) == 0xfc00003e) || /* fnmadd */
                                ((m->prev_insn & 0xfc00003e) == 0xfc00003c) || /* fnmsub */
                                ((m->prev_insn & 0xfc0007fe) == 0xfc000028) || /* fsub */
                                ((m->prev_insn & 0xfc0007fe) == 0xfc00002a) || /* fadd */
                                0
                            ) &&
                            (rt == i) &&
                            m->fpregs[i] == signz(a->fpregs[i]) && // m is zero
                            (exponent(a->fpregs[i]) < -1022) // a is tiny
                        ) ||
                        (
                            (
                                ((m->prev_insn & 0xfc0007fe) == 0xec00002a) || /* fadds */
                                ((m->prev_insn & 0xfc0007fe) == 0xec000028) || /* fsubs */
                                0
                            ) &&
                            (rt == i) &&
                            m->fpregs[i] == signz(a->fpregs[i]) && // m is zero
                            (exponent(a->fpregs[i]) < -126) // a is tiny
                        ) ||
                        (
                            (
                                ((m->prev_insn & 0xfc0007fe) == 0xec00002a) || /* fadds */
                                ((m->prev_insn & 0xfc0007fe) == 0xec000028) || /* fsubs */
                                0
                            ) &&
                            (rt == i) &&
                            negative(a->fpregs[i]) == negative(m->fpregs[i]) && // same sign
                            exponent(a->fpregs[i]) < -126 &&
                            ((ra == i) || exponent(m->fpregs[ra]) < -126) &&
                            ((rb == i) || exponent(m->fpregs[rb]) < -126)
                        ) ||
                        (
                            (
                                ((m->prev_insn & 0xfc0007fe) == 0xec00002a) || /* fadds */
                                ((m->prev_insn & 0xfc0007fe) == 0xec000028) || /* fsubs */
                                0
                            ) &&
                            (rt == i) &&
                            negative(a->fpregs[i]) == negative(m->fpregs[i]) && // same sign
                            exponent(a->fpregs[i]) < -126 &&
                            ABS(exponent(m->fpregs[i]) - exponent(a->fpregs[i]) - 192) <= 1
                        ) ||
                        (
                            (
                                ((m->prev_insn & 0xfc00f83e) == 0xfc000032) || /* fmul */
                                ((m->prev_insn & 0xfc00003e) == 0xfc00003a) || /* fmadd */
                                ((m->prev_insn & 0xfc00003e) == 0xfc000038) || /* fmsub */
                                ((m->prev_insn & 0xfc00003e) == 0xfc00003e) || /* fnmadd */
                                ((m->prev_insn & 0xfc00003e) == 0xfc00003c) || /* fnmsub */
                                0
                            ) &&
                            (rt == i) &&
                            (ra != i) &&
                            (rc != i) &&
                            exponent(a->fpregs[i]) < -126 && // a is zero or denormalized
                            (exponent(m->fpregs[ra]) + exponent(m->fpregs[rc]) < -1022)
                        ) ||
                        (
                            (
                                ((m->prev_insn & 0xfc00f83e) == 0xfc000032) || /* fmul */
                                ((m->prev_insn & 0xfc00003e) == 0xfc00003a) || /* fmadd */
                                ((m->prev_insn & 0xfc00003e) == 0xfc000038) || /* fmsub */
                                ((m->prev_insn & 0xfc00003e) == 0xfc00003e) || /* fnmadd */
                                ((m->prev_insn & 0xfc00003e) == 0xfc00003c) || /* fnmsub */
                                0
                            ) &&
                            (rt == i) &&
                            (ra != i) &&
                            (rc != i) &&
                            m->fpregs[i] == signz(a->fpregs[i]) && // m is zero
                            denormalized(a->fpregs[i]) &&
                            (exponent(m->fpregs[ra]) + exponent(m->fpregs[rc]) < -1022)
                        ) ||
                        (
                            ((m->prev_insn & 0xfc00f83e) == 0xfc000032) && /* fmul */
                            (rt == i) && (
                                (ra == i) || (rc == i)
                            ) &&
                            exponent(a->fpregs[i]) < -126 && // a is tiny
                            (exponent(m->fpregs[ra]) < 0x200)
                        ) ||
                        (
                            ((m->prev_insn & 0xfc0007fe) == 0xfc000024) && /* fdiv */
                            (rt == i) &&
                            (ra != i) &&
                            (rb != i) &&
                            negative(a->fpregs[i]) == negative(m->fpregs[i]) && // same sign
                            denormalized(a->fpregs[i]) &&
                            (exponent(m->fpregs[ra]) - exponent(m->fpregs[rb]) < -1022)
                        ) ||
                        (
                            ((m->prev_insn & 0xfc0007fe) == 0xfc000024) && /* fdiv */
                            (rt == i) &&
                            (ra != i) &&
                            (rb != i) &&
                            a->fpregs[i] == signz(m->fpregs[i]) && // a is zero
                            (exponent(m->fpregs[ra]) - exponent(m->fpregs[rb]) < -1022)
                        ) ||
                        (
                            ((m->prev_insn & 0xfc0007fe) == 0xfc000024) && /* fdiv */
                            (rt == i) &&
                            (ra != i) &&
                            (rb == i) &&
                            a->fpregs[i] == signz(m->fpregs[i]) && // a is zero
                            exponent(a->fpregs[ra]) < -126
                        ) ||
                        (
                            ((m->prev_insn & 0xfc0007fe) == 0xfc000024) && /* fdiv */
                            (rt == i) &&
                            (ra == i) &&
                            (rb != i) &&
                            a->fpregs[i] == signz(m->fpregs[i]) // a is zero
                        ) ||
                        (
                            ((m->prev_insn & 0xfc0007fe) == 0xfc000024) && /* fdiv */
                            (rt == i) &&
                            ( (ra == i) || (rb == i) ) &&
                            exponent(a->fpregs[i]) < -1022 && // a is tiny or zero
                            exponent(m->fpregs[i]) < -1022 + 1536 // underflow exception adds 1536 on PPC but not on Intel
                        ) ||
                        (
                            ((m->prev_insn & 0xfc0007fe) == 0xec000024) && /* fdivs */
                            (rt == i) &&
                            (ra != i) &&
                            (rb != i) &&
                            a->fpregs[i] == signz(m->fpregs[i]) && // a is zero
                            (exponent(m->fpregs[ra]) - exponent(m->fpregs[rb]) < -126)
                        ) ||
                        (
                            ((m->prev_insn & 0xfc0007fe) == 0xec000024) && /* fdivs */
                            (rt == i) &&
                            (ra != i) &&
                            (rb != i) &&
                            m->fpregs[i] == signz(a->fpregs[i]) && // m is zero
                            (exponent(m->fpregs[ra]) - exponent(m->fpregs[rb]) < -126)
                        ) ||
                        (
                            ((m->prev_insn & 0xfc0007fe) == 0xec000024) && /* fdivs */
                            (rt == i) &&
                            (ra != i) &&
                            (rb == i) &&
                            a->fpregs[i] == signz(m->fpregs[i]) && // a is zero
                            denormalized(a->fpregs[ra])
                        ) ||
                        (
                            ((m->prev_insn & 0xfc0007fe) == 0xec000024) && /* fdivs */
                            (rt == i) &&
                            (rb != i) &&
                            (ra == i) &&
                            a->fpregs[i] == signz(m->fpregs[i]) // a is zero
                        ) ||
                        (
                            ((m->prev_insn & 0xfc1f07fe) == 0xec000030) && /* fres */
                            (rt == i) &&
                            exponent(a->fpregs[i]) < -126 && // a is near zero
                            (
                                rb == i ||
                                (exponent(m->fpregs[rb]) >= 126)
                            )
                        ) ||
                        (
                            ((m->prev_insn & 0xfc1f07fe) == 0xec000030) && /* fres */
                            (rt == i) &&
                            (rb == i) &&
                            exponent(a->fpregs[i]) < -126 && // a is near zero
                            (exponent(m->fpregs[i]) == exponent(a->fpregs[i]) + 192)
                        ) ||
                        (
                            (
                                ((m->prev_insn & 0xfc0007fe) == 0xfc000028) || /* fsub */
                                ((m->prev_insn & 0xfc00003e) == 0xfc000038) || /* fmsub */
                                ((m->prev_insn & 0xfc00003e) == 0xfc00003e) || /* fnmadd */
                                ((m->prev_insn & 0xfc00003e) == 0xfc00003c) || /* fnmsub */
                                ((m->prev_insn & 0xfc00003e) == 0xfc00003a) || /* fmadd */
                                ((m->prev_insn & 0xfc00f83e) == 0xfc000032) || /* fmul */
                                ((m->prev_insn & 0xfc0007fe) == 0xfc00002a) || /* fadd */
                                ((m->prev_insn & 0xfc0007fe) == 0xfc000024) || /* fdiv */
                                0
                            ) &&
                            (rt == i) &&
                            negative(a->fpregs[i]) == negative(m->fpregs[i]) && // same sign
                            denormalized(a->fpregs[i]) &&
                            ABS((int64_t)(normalize(a->fpregs[i]) - m->fpregs[i])) <= 0x20000
                        ) ||
                        (
                            (
                                ((m->prev_insn & 0xfc00003e) == 0xfc000038) || /* fmsub */
                                0
                            ) &&
                            (rt == i) &&
                            m->fpregs[i] == signz(a->fpregs[i]) && // m is zero
                            denormalized(a->fpregs[i])
                        ) ||
                        (
                            ((m->prev_insn & 0xfc0007fe) == 0xfc00002a) && /* fadd */
                            (rt == i) && (
                                (ra == i) ||
                                (rb == i)
                            ) &&
                            m->fpregs[i] == signz(a->fpregs[i]) && // a is zero
                            denormalized(a->fpregs[i])
                        ) ||
                        (
                            (
                                ((m->prev_insn & 0xfc00003e) == 0xec00003c) || /* fnmsubs */
                                ((m->prev_insn & 0xfc0007fe) == 0xec000028) || /* fsubs */
                                ((m->prev_insn & 0xfc0007fe) == 0xec00002a) || /* fadds */
                                0
                            ) &&
                            (rt == i) &&
                            exponent(a->fpregs[i]) < -126 && // tiny
                            exponent(m->fpregs[i]) == exponent(a->fpregs[i]) + 192 &&
                            ABS((int64_t)(mantissa(a->fpregs[i]) - mantissa(m->fpregs[i]))) <= 0x20000000
                        ) ||
                        (
                            (
                                ((m->prev_insn & 0xfc00003e) == 0xec00003a) || /* fmadds */
                                0
                            ) &&
                            (rt == i) &&
                            (ra != i) &&
                            (rc != i) &&
                            (rb != i) &&
                            (
                                (
                                    zero(m->fpregs[ra]) &&
                                    denormalized(m->fpregs[rc])
                                ) || (
                                    zero(m->fpregs[rc]) &&
                                    denormalized(m->fpregs[ra])
                                )
                            ) &&
                            zero(m->fpregs[i]) &&
                            a->fpregs[i] == m->fpregs[rb]
                        ) ||
                        (
                            (
                                ((m->prev_insn & 0xfc00003e) == 0xec00003c) || /* fnmsubs */
                                ((m->prev_insn & 0xfc00003e) == 0xec00003a) || /* fmadds */
                                ((m->prev_insn & 0xfc00003e) == 0xec000038) || /* fmsubs */
                                ((m->prev_insn & 0xfc00003e) == 0xec00003e) || /* fnmadds */
                                0
                            ) &&
                            (rt == i) &&
                            (ra == i || exponent(a->fpregs[ra]) < -126) && // a is tiny
                            (rb == i || exponent(a->fpregs[rb]) < -126) && // b is tiny
                            (rc == i || exponent(a->fpregs[rc]) < -126)    // c is tiny
                        )
                    )
                ) ||
                (
                    (fp_opts & fp_opt_ignore_overflow) &&
                    (
                        (
                            (
                                ((m->prev_insn & 0xfc00003e) == 0xec00003a) || /* fmadds */
                                ((m->prev_insn & 0xfc00003e) == 0xec000038) || /* fmsubs */
                                ((m->prev_insn & 0xfc00003e) == 0xec00003e) || /* fnmadds */
                                ((m->prev_insn & 0xfc00003e) == 0xec00003c) || /* fnmsubs */
                                ((m->prev_insn & 0xfc00003e) == 0xfc00003a) || /* fmadd */
                                ((m->prev_insn & 0xfc00003e) == 0xfc000038) || /* fmsub */
                                ((m->prev_insn & 0xfc00003e) == 0xfc00003e) || /* fnmadd */
                                ((m->prev_insn & 0xfc00003e) == 0xfc00003c) || /* fnmsub */

                                0
                            ) &&
                            (rt == i) &&
                            infinite(m->fpregs[i]) &&
                            isqnan(a->fpregs[i]) &&
                            (
                                ra == i || rb == i || rc == i
                            )
                        ) ||
                        (
                            (
                                ((m->prev_insn & 0xfc00003e) == 0xec00003e) || /* fnmadds */
                                ((m->prev_insn & 0xfc00003e) == 0xec00003c) || /* fnmsubs */
                                ((m->prev_insn & 0xfc00003e) == 0xec000038) || /* fmsubs */
                                ((m->prev_insn & 0xfc00003e) == 0xec00003a) || /* fmadds */
                                ((m->prev_insn & 0xfc0007fe) == 0xec000024) || /* fdivs */
                                ((m->prev_insn & 0xfc0007fe) == 0xec00002a) || /* fadds */
                                ((m->prev_insn & 0xfc00f83e) == 0xec000032) || /* fmuls */
                                ((m->prev_insn & 0xfc1f07fe) == 0xec000030) || /* fres */
                                ((m->prev_insn & 0xfc0007fe) == 0xec000028) || /* fsubs */
                                0
                            ) &&
                            (rt == i) &&
                            (infinite(m->fpregs[i]) || (fabs(*(double*)(&m->fpregs[i])) >= FLT_MAX)) &&
                            exponent(a->fpregs[i]) > 126
                        ) ||
                        (
                            (
                                ((m->prev_insn & 0xfc0007fe) == 0xec000028) || /* fsubs */
                                ((m->prev_insn & 0xfc0007fe) == 0xec00002a) || /* fadds */
                                ((m->prev_insn & 0xfc00f83e) == 0xec000032) || /* fmuls */
                                ((m->prev_insn & 0xfc00003e) == 0xec00003a) || /* fmadds */
                                ((m->prev_insn & 0xfc00003e) == 0xec000038) || /* fmsubs */
                                ((m->prev_insn & 0xfc00003e) == 0xec00003e) || /* fnmadds */
                                ((m->prev_insn & 0xfc00003e) == 0xec00003c) || /* fnmsubs */
                                0
                            ) &&
                            (rt == i) &&
                            a->fpregs[i] == signinf(m->fpregs[i]) &&
                            (
                                (
                                    (ra != i) &&
                                    (rb != i) &&
                                    denormalized(a->fpregs[rb]) &&
                                    exponent(m->fpregs[ra]) > 126 &&
                                    exponent(m->fpregs[i]) == exponent(a->fpregs[ra]) - 192 &&
                                    ABS((int64_t)(mantissa(a->fpregs[ra]) - mantissa(m->fpregs[i]))) <= 0x20000000
                                ) ||
                                (
                                    ((ra != i) && exponent(m->fpregs[ra]) > 126) ||
                                    ((rb != i) && exponent(m->fpregs[rb]) > 126)
                                ) ||
                                (
                                    infinite(a->fpregs[i]) &&
                                    (ra == i || rb == i)
                                )
                            )
                        ) ||
                        (
                            (
                                ((m->prev_insn & 0xfc00f83e) == 0xec000032) || /* fmuls */
                                ((m->prev_insn & 0xfc00003e) == 0xec00003a) || /* fmadds */
                                ((m->prev_insn & 0xfc00003e) == 0xec000038) || /* fmsubs */
                                ((m->prev_insn & 0xfc00003e) == 0xec00003e) || /* fnmadds */
                                ((m->prev_insn & 0xfc00003e) == 0xec00003c) || /* fnmsubs */
                                0
                            ) &&
                            (rt == i) &&
                            (ra != i) &&
                            (rc != i) &&
                            a->fpregs[i] == signinf(m->fpregs[i]) &&
                            (exponent(m->fpregs[ra]) + exponent(m->fpregs[rc]) >= 126)
                        ) ||
                        (
                            (
                                ((m->prev_insn & 0xfc00003e) == 0xec00003a) || /* fmadds */
                                ((m->prev_insn & 0xfc00003e) == 0xec000038) || /* fmsubs */
                                ((m->prev_insn & 0xfc00003e) == 0xec00003e) || /* fnmadds */
                                ((m->prev_insn & 0xfc00003e) == 0xec00003c) || /* fnmsubs */
                                0
                            ) &&
                            (rt == i) &&
                            (rb != i) &&
                            negative(a->fpregs[i]) == negative(m->fpregs[i]) &&
                            exponent(a->fpregs[i]) >= 126 &&
                            exponent(m->fpregs[rb]) >= 126
                        ) ||
                        (
                            (
                                ((m->prev_insn & 0xfc00f83e) == 0xfc000032) || /* fmul */
                                ((m->prev_insn & 0xfc00003e) == 0xfc00003a) || /* fmadd */
                                ((m->prev_insn & 0xfc00003e) == 0xfc000038) || /* fmsub */
                                ((m->prev_insn & 0xfc00003e) == 0xfc00003e) || /* fnmadd */
                                ((m->prev_insn & 0xfc00003e) == 0xfc00003c) || /* fnmsub */
                                0
                            ) &&
                            (rt == i) &&
                            (ra != i) &&
                            (rc != i) &&
                            a->fpregs[i] == signinf(m->fpregs[i]) &&
                            (exponent(m->fpregs[ra]) + exponent(m->fpregs[rc]) >= 1023)
                        ) ||
                        (
                            (
                                ((m->prev_insn & 0xfc00003e) == 0xfc00003a) || /* fmadd */
                                ((m->prev_insn & 0xfc00003e) == 0xfc000038) || /* fmsub */
                                ((m->prev_insn & 0xfc00003e) == 0xfc00003e) || /* fnmadd */
                                ((m->prev_insn & 0xfc00003e) == 0xfc00003c) || /* fnmsub */
                                0
                            ) &&
                            a->fpregs[i] == signinf(m->fpregs[i]) &&
                            (rt == i) && (
                                (rb == i) || (ra == i)
                            )
                        ) ||
                        (
                            (
                                ((m->prev_insn & 0xfc00003e) == 0xfc00003e) || /* fnmadd */
                                0
                            ) &&
                            (rt == i) &&
                            (ra != i) &&
                            (rc == i) &&
                            a->fpregs[i] == signinf(m->fpregs[i]) &&
                            (exponent(m->fpregs[ra]) > 0)
                        ) ||
                        (
                            ((m->prev_insn & 0xfc0007fe) == 0xfc000024) && /* fdiv */
                            (rt == i) &&
                            (ra != i) &&
                            (rb != i) &&
                            a->fpregs[i] == signinf(m->fpregs[i]) &&
                            (exponent(m->fpregs[ra]) - exponent(m->fpregs[rb]) >= 1023)
                        ) ||
                        (
                            ((m->prev_insn & 0xfc0007fe) == 0xfc000024) && /* fdiv */
                            (rt == i) &&
                            ((rb == i) || (ra == i)) &&
                            a->fpregs[i] == signinf(m->fpregs[i])
                        ) ||
                        (
                            ((m->prev_insn & 0xfc0007fe) == 0xec000024) && /* fdivs */
                            (rt == i) &&
                            (ra != i) &&
                            (rb != i) &&
                            a->fpregs[i] == signinf(m->fpregs[i]) &&
                            (exponent(m->fpregs[ra]) - exponent(m->fpregs[rb]) >= 126)
                        ) ||
                        (
                            ((m->prev_insn & 0xfc1f07fe) == 0xec000030) && /* fres */
                            (rt == i) &&
                            a->fpregs[i] == signinf(m->fpregs[i]) && (
                                (rb == i) || (-exponent(m->fpregs[rb]) >= 126) // includes zero
                            )
                        ) ||
                        (
                            ((m->prev_insn & 0xfc1f07fe) == 0xfc000018) && /* frsp */
                            (rt == i) &&
                            (rb != i) &&
                            a->fpregs[i] == signinf(m->fpregs[i]) &&
                            (exponent(m->fpregs[rb]) >= 126)
                        ) ||
                        (
                            ((m->prev_insn & 0xfc1f07fe) == 0xfc000018) && /* frsp */
                            (rt == i) &&
                            (rb == i) &&
                            a->fpregs[i] == signinf(m->fpregs[i]) &&
                            infinite(a->fpregs[i])
                        )
                    )
                ) ||
                (
                    (fp_opts & fp_opt_ignore_round) && (
                        ((m->prev_insn & 0xfc00003e) == 0xfc00003e) || /* fnmadd */
                        ((m->prev_insn & 0xfc00003e) == 0xfc00003a) || /* fmadd */
                        ((m->prev_insn & 0xfc0007fe) == 0xfc000028) || /* fsub */
                        ((m->prev_insn & 0xfc00f83e) == 0xfc000032) || /* fmul */
                        ((m->prev_insn & 0xfc00003e) == 0xfc00003c) || /* fnmsub */
                        ((m->prev_insn & 0xfc00003e) == 0xfc000038) || /* fmsub */
                        ((m->prev_insn & 0xfc0007fe) == 0xfc000024) || /* fdiv */
                        ((m->prev_insn & 0xfc0007fe) == 0xfc00002a) || /* fadd */
                        0
                    ) &&
                    (rt == i) &&
                    !anyqnan(a->fpregs[i]) &&
                    !anyqnan(m->fpregs[i]) &&
                    (ABS((int64_t)(a->fpregs[i] - m->fpregs[i])) <= 1)
                ) ||
                (
                    (fp_opts & fp_opt_ignore_round) && (
                        ((m->prev_insn & 0xfc1f07fe) == 0xfc000034) || /* frsqrte */
                        ((m->prev_insn & 0xfc1f07fe) == 0xec000030) || /* fres */
                        0
                    ) &&
                    (rt == i) &&
                    !anyqnan(a->fpregs[i]) &&
                    !anyqnan(m->fpregs[i]) &&
                    (ABS((int64_t)(a->fpregs[i] - m->fpregs[i])) <= (1LL<<47))
                ) ||
                (
                    (fp_opts & fp_opt_ignore_round_s) && (
                        ((m->prev_insn & 0xfc00f83e) == 0xec000032) || /* fmuls */
                        ((m->prev_insn & 0xfc00003e) == 0xec00003c) || /* fnmsubs */
                        ((m->prev_insn & 0xfc0007fe) == 0xec000024) || /* fdivs */
                        ((m->prev_insn & 0xfc00003e) == 0xec00003a) || /* fmadds */
                        ((m->prev_insn & 0xfc0007fe) == 0xec00002a) || /* fadds */
                        ((m->prev_insn & 0xfc00003e) == 0xec00003e) || /* fnmadds */
                        ((m->prev_insn & 0xfc1f07fe) == 0xfc000018) || /* frsp */
                        ((m->prev_insn & 0xfc00003e) == 0xec000038) || /* fmsubs */
                        ((m->prev_insn & 0xfc0007fe) == 0xec000028) || /* fsubs */
                        0
                    ) &&
                    (rt == i) &&
                    !anyqnan(a->fpregs[i]) &&
                    !anyqnan(m->fpregs[i]) &&
                    (ABS((int64_t)(a->fpregs[i] - m->fpregs[i])) <= 0x800000000ULL)
                ) ||
                (
                    (fp_opts & fp_opt_ignore_round_s) && (
                        ((m->prev_insn & 0xfc00f83e) == 0xec000032) || /* fmuls */
                        ((m->prev_insn & 0xfc00003e) == 0xec00003c) || /* fnmsubs */
                        ((m->prev_insn & 0xfc0007fe) == 0xec000024) || /* fdivs */
                        ((m->prev_insn & 0xfc00003e) == 0xec00003a) || /* fmadds */
                        ((m->prev_insn & 0xfc0007fe) == 0xec00002a) || /* fadds */
                        ((m->prev_insn & 0xfc00003e) == 0xec00003e) || /* fnmadds */
                        ((m->prev_insn & 0xfc1f07fe) == 0xfc000018) || /* frsp */
                        ((m->prev_insn & 0xfc00003e) == 0xec000038) || /* fmsubs */
                        ((m->prev_insn & 0xfc0007fe) == 0xec000028) || /* fsubs */
                        0
                    ) &&
                    (rt == i) &&
                    !anyqnan(a->fpregs[i]) &&
                    !anyqnan(m->fpregs[i]) &&
                    exponent(a->fpregs[i]) < -126 &&
                    exponent(m->fpregs[i]) < -126 &&
                    (ABS((int64_t)(a->fpregs[i] - m->fpregs[i])) <= 0x800000000000ULL)
                ) ||
                (
                    (fp_opts & fp_opt_ignore_round_s) && (
                        ((m->prev_insn & 0xfc0007fe) == 0xec00002a) || /* fadds */
                        ((m->prev_insn & 0xfc0007fe) == 0xec000028) || /* fsubs */
                        0
                    ) &&
                    (rt == i) &&
                    (ra != i) &&
                    (rb != i) &&
                    !anyqnan(a->fpregs[i]) &&
                    !anyqnan(m->fpregs[i]) &&
                    exponent(m->fpregs[i]) < -126 &&
                    exponent(a->fpregs[i]) < -126 &&
                    exponent(a->fpregs[ra]) < -126 &&
                    exponent(m->fpregs[rb]) < -126
                ) ||
                (
                    (fp_opts & fp_opt_ignore_rsqrt_negative) && (
                        ((m->prev_insn & 0xfc1f07fe) == 0xfc000034) || /* frsqrte */
                        0
                    ) &&
                    (rt == i) &&
                    isqnan(a->fpregs[i]) && (
                        (rb == i) || negative(m->fpregs[rb])
                    )
                ) ||
                (
                    (fp_opts & fp_opt_ignore_rsqrt_negative_zero) && (
                        ((m->prev_insn & 0xfc1f07fe) == 0xfc000034) || /* frsqrte */
                        0
                    ) &&
                    (rt == i) &&
                    (rb != i) &&
                    m->fpregs[rb] == 0x8000000000000000ULL &&
                    a->fpregs[i] == 0xfff0000000000000ULL
                ) ||
                (
                    (fp_opts & fp_opt_ignore_rsqrt_zero) && (
                        ((m->prev_insn & 0xfc1f07fe) == 0xfc000034) || /* frsqrte */
                        0
                    ) &&
                    (rt == i) &&
                    (rb != i) &&
                    m->fpregs[rb] == 0 &&
                    a->fpregs[i] == 0x7ff0000000000000ULL
                ) ||
                (
                    (fp_opts & fp_opt_ignore_qnan_from_inf) && (
                        ((m->prev_insn & 0xfc00f83e) == 0xfc000032) || /* fmul */
                        ((m->prev_insn & 0xfc00003e) == 0xfc00003a) || /* fmadd */
                        ((m->prev_insn & 0xfc00003e) == 0xfc000038) || /* fmsub */
                        ((m->prev_insn & 0xfc00003e) == 0xfc00003e) || /* fnmadd */
                        ((m->prev_insn & 0xfc00003e) == 0xfc00003c) || /* fnmsub */
                        ((m->prev_insn & 0xfc00003e) == 0xec00003e) || /* fnmadds */
                        0
                    ) &&
                    (rt == i) &&
                    isqnan(a->fpregs[i]) && (
                        ((ra != i) && infinite(a->fpregs[ra])) ||
                        ((rc != i) && infinite(a->fpregs[rc]))
                    )
                ) ||
                (
                    (fp_opts & fp_opt_ignore_qnan_from_unknown) && (
                        ((m->prev_insn & 0xfc0007fe) == 0xfc000024) || /* fdiv */
                        0
                    ) &&
                    (rt == i) &&
                    isqnan(a->fpregs[i]) && (
                        (ra == i) || (rb == i)
                    )
                ) ||
                (
                    (fp_opts & fp_opt_ignore_inf_from_unknown) && (
                        ((m->prev_insn & 0xfc1f07fe) == 0xfc000034) || /* frsqrte */
                        ((m->prev_insn & 0xfc1f07fe) == 0xec000030) || /* fres */
                        0
                    ) &&
                    (rt == i) &&
                    infinite(a->fpregs[i]) &&
                    (rb == i)
                ) ||
                (
                    (fp_opts & fp_opt_ignore_inf_from_unknown) && (
                        ((m->prev_insn & 0xfc0007fe) == 0xec000024) || /* fdivs */
                        0
                    ) &&
                    (rt == i) &&
                    infinite(a->fpregs[i]) && (
                        (ra == i) || (rb == i)
                    )
                ) ||
                (
                    (fp_opts & fp_opt_ignore_inf_from_unknown) && (
                        ((m->prev_insn & 0xfc00f83e) == 0xfc000032) || /* fmul */
                        ((m->prev_insn & 0xfc00003e) == 0xfc00003a) || /* fmadd */
                        ((m->prev_insn & 0xfc00003e) == 0xfc000038) || /* fmsub */
                        ((m->prev_insn & 0xfc00003e) == 0xfc00003e) || /* fnmadd */
                        ((m->prev_insn & 0xfc00003e) == 0xfc00003c) || /* fnmsub */
                        ((m->prev_insn & 0xfc00f83e) == 0xec000032) || /* fmuls */
                        ((m->prev_insn & 0xfc00003e) == 0xec00003a) || /* fmadds */
                        ((m->prev_insn & 0xfc00003e) == 0xec000038) || /* fmsubs */
                        ((m->prev_insn & 0xfc00003e) == 0xec00003e) || /* fnmadds */
                        ((m->prev_insn & 0xfc00003e) == 0xec00003c) || /* fnmsubs */
                        0
                    ) &&
                    (rt == i) &&
                    infinite(a->fpregs[i]) && (
                        (ra == i) || (rc == i)
                    )
                ) ||
                (
                    (fp_opts & fp_opt_ignore_zero_signs) &&
                    zero(a->fpregs[i]) &&
                    zero(m->fpregs[i]) &&
                    ((a->fpregs[i] & ~(1ULL<<63)) == (m->fpregs[i] & ~(1ULL<<63)))
                ) ||
                (
                    (fp_opts & fp_opt_ignore_inf_div_inf) && (
                        ((m->prev_insn & 0xfc0007fe) == 0xfc000024) || /* fdiv */
                        ((m->prev_insn & 0xfc0007fe) == 0xec000024) || /* fdivs */
                        0
                    ) &&
                    (rt == i) &&
                    infinite(a->fpregs[ra]) &&
                    infinite(a->fpregs[rb])
                ) ||
                (
                    (fp_opts & fp_opt_ignore_QNaN_SNaN) && (
                        ((m->prev_insn & 0xfc0007fe) == 0xfc000024) || /* fdiv */
                        0
                    ) &&
                    (rt == i) &&
                    is_nan(a->fpregs[i]) &&
                    is_nan(m->fpregs[i])
                ) ||
                (
                    (fp_opts & fp_opt_ignore_NaN_unknown_operands) && (
                        ((m->prev_insn & 0xfc00003e) == 0xfc00003a) || /* fmadd */
                        ((m->prev_insn & 0xfc00003e) == 0xfc000038) || /* fmsub */
                        ((m->prev_insn & 0xfc00003e) == 0xfc00003e) || /* fnmadd */
                        ((m->prev_insn & 0xfc00003e) == 0xfc00003c) || /* fnmsub */
                        ((m->prev_insn & 0xfc00003e) == 0xec00003a) || /* fmadds */
                        ((m->prev_insn & 0xfc00003e) == 0xec000038) || /* fmsubs */
                        ((m->prev_insn & 0xfc00003e) == 0xec00003e) || /* fnmadds */
                        ((m->prev_insn & 0xfc00003e) == 0xec00003c) || /* fnmsubs */
                        0
                    ) &&
                    (rt == i) &&
                    is_nan(a->fpregs[i]) &&
                    ( ra== i || rb == i || rc == i)
                )
            ) {
                a->fpregs[i] = m->fpregs[i];
            } else {
                return 0;
            }
        }
    }

#ifdef VRREGS
    for (i = 0; i < 32; i++) {
        if (m->vrregs.vrregs[i][0] != a->vrregs.vrregs[i][0] ||
            m->vrregs.vrregs[i][1] != a->vrregs.vrregs[i][1] ||
            m->vrregs.vrregs[i][2] != a->vrregs.vrregs[i][2] ||
            m->vrregs.vrregs[i][3] != a->vrregs.vrregs[i][3]
        ) {
            if ((1 << (31-i)) & ~vrregs_mask) {
                a->vrregs.vrregs[i][0] = m->vrregs.vrregs[i][0];
                a->vrregs.vrregs[i][1] = m->vrregs.vrregs[i][1];
                a->vrregs.vrregs[i][2] = m->vrregs.vrregs[i][2];
                a->vrregs.vrregs[i][3] = m->vrregs.vrregs[i][3];
            } else {
                return 0;
            }
        }
    }
#endif
    return 1;
}

/* reginfo_dump: print state to a stream, returns nonzero on success */
int reginfo_dump(struct reginfo *ri, FILE * f)
{
    int i;

    fprintf(f, "2nd prev insn @ %0" PRIx " : %08x\n", ri->nip - 8, ri->second_prev_insn);
    fprintf(f, "previous insn @ %0" PRIx " : %08x\n", ri->nip - 4, ri->prev_insn);
    fprintf(f, "faulting insn @ %0" PRIx " : %08x\n", ri->nip + 0, ri->faulting_insn);
    fprintf(f, "    next insn @ %0" PRIx " : %08x\n", ri->nip + 4, ri->next_insn);
    fprintf(f, "\n");

    for (i = 0; i < 16; i++) {
        fprintf(f, "\tr%d%s : %0" PRIx "\tr%d : %0" PRIx "\n", i, i < 10 ? " " : "", ri->gregs[i],
                i + 16, ri->gregs[i + 16]);
    }

    fprintf(f, "\n");
    fprintf(f, "\tnip    : %0" PRIx "\n", ri->gregs[risu_NIP  ]);
    fprintf(f, "\tmsr    : %0" PRIx "\n", ri->gregs[risu_MSR  ]);
    fprintf(f, "\tctr    : %0" PRIx "\n", ri->gregs[risu_CTR  ]);
    fprintf(f, "\tlnk    : %0" PRIx "\n", ri->gregs[risu_LNK  ]);
    fprintf(f, "\txer    : %0" PRIx "\n", ri->gregs[risu_XER  ]);
    fprintf(f, "\tccr    : %0" PRIx "\n", ri->gregs[risu_CCR  ]);
    fprintf(f, "\tmq     : %0" PRIx "\n", ri->gregs[risu_MQ   ]);
#if !defined(__APPLE__) && !defined(RISU_DPPC)
    fprintf(f, "\tdar    : %0" PRIx "\n", ri->gregs[risu_DAR  ]);
    fprintf(f, "\tdsisr  : %0" PRIx "\n", ri->gregs[risu_DSISR]);
#endif

    for (i = 0; i < 16; i++) {
        fprintf(f, "\tf%d%s : %016" PRIx64 "\tf%d : %016" PRIx64 "\n", i, i < 10 ? " " : "", ri->fpregs[i],
                i + 16, ri->fpregs[i + 16]);
    }
    fprintf(f, "\tfpscr : %0" PRIx "\n\n", ri->fpscr);

#ifdef VRREGS
    for (i = 0; i < 32; i++) {
        fprintf(f, "\tvr%d%s : %08x, %08x, %08x, %08x\n", i, i < 10 ? " " : "",
                ri->vrregs.vrregs[i][0], ri->vrregs.vrregs[i][1],
                ri->vrregs.vrregs[i][2], ri->vrregs.vrregs[i][3]);
    }
    fprintf(f, "\tvscr : %08x, %08x, %08x, %08x\n",
            ri->vrregs.vscr[0], ri->vrregs.vscr[1],
            ri->vrregs.vscr[2], ri->vrregs.vscr[3]
    );
    fprintf(f, "\tvrsave : %08x\n", ri->vrregs.vrsave);
#if defined(__APPLE__) && !defined(RISU_DPPC)
    fprintf(f, "\tvrsave2 : %08x\n", ri->vrregs.vrsave2);
    fprintf(f, "\tsave_vrvalid : %08x\n\n", ri->vrregs.save_vrvalid);
#endif
#endif

    return !ferror(f);
}

int reginfo_dump_mismatch(struct reginfo *m, struct reginfo *a, FILE *f)
{
    int i;
    for (i = 0; i < 32; i++) {
        if ((1 << (31-i)) & ~gregs_mask) {
            continue;
        }

        if (m->gregs[i] != a->gregs[i]) {
            fprintf(f, "Mismatch: r%d ", i);
            fprintf(f, "m: [%0" PRIx "] != a: [%0" PRIx "]\n",
                    m->gregs[i], a->gregs[i]);
        }
    }

    if (m->gregs[risu_CTR] != a->gregs[risu_CTR]) {
        fprintf(f, "Mismatch: ctr ");
        fprintf(f, "m: [%0" PRIx "] != a: [%0" PRIx "]\n", m->gregs[risu_CTR], a->gregs[risu_CTR]);
    }

    if (m->gregs[risu_LNK] - m->gregs[risu_NIP] != a->gregs[risu_LNK] - a->gregs[risu_NIP]) {
        fprintf(f, "Mismatch: lr (relative to pc) ");
        fprintf(f, "m: [%0" PRIx "] != a: [%0" PRIx "]\n", m->gregs[risu_LNK] - m->gregs[risu_NIP], a->gregs[risu_LNK] - a->gregs[risu_NIP]);
    }

    if (m->gregs[risu_XER] != a->gregs[risu_XER]) {
        fprintf(f, "Mismatch: xer ");
        fprintf(f, "m: [%0" PRIx "] != a: [%0" PRIx "]\n", m->gregs[risu_XER], a->gregs[risu_XER]);
    }

    if (m->gregs[risu_CCR] != a->gregs[risu_CCR]) {
        fprintf(f, "Mismatch: ccr ");
        fprintf(f, "m: [%0" PRIx "] != a: [%0" PRIx "]\n", m->gregs[risu_CCR], a->gregs[risu_CCR]);
    }

    if (m->gregs[risu_MQ] != a->gregs[risu_MQ]) {
        fprintf(f, "Mismatch: mq ");
        fprintf(f, "m: [%0" PRIx "] != a: [%0" PRIx "]\n", m->gregs[risu_MQ], a->gregs[risu_MQ]);
    }

    for (i = 0; i < 32; i++) {
        if (m->fpregs[i] != a->fpregs[i]) {
            fprintf(f, "Mismatch: f%d ", i);
            fprintf(f, "m: [%016" PRIx64 "] != a: [%016" PRIx64 "]\n",
                    m->fpregs[i], a->fpregs[i]);
        }
    }

    if (m->fpscr != a->fpscr) {
        fprintf(f, "Mismatch: fpscr ");
        fprintf(f, "m: [%0" PRIx "] != a: [%0" PRIx "]\n", m->fpscr, a->fpscr);
    }

#ifdef VRREGS
    for (i = 0; i < 32; i++) {
        if (m->vrregs.vrregs[i][0] != a->vrregs.vrregs[i][0] ||
            m->vrregs.vrregs[i][1] != a->vrregs.vrregs[i][1] ||
            m->vrregs.vrregs[i][2] != a->vrregs.vrregs[i][2] ||
            m->vrregs.vrregs[i][3] != a->vrregs.vrregs[i][3]) {

            fprintf(f, "Mismatch: vr%d ", i);
            fprintf(f, "m: [%08x, %08x, %08x, %08x] != a: [%08x, %08x, %08x, %08x]\n",
                    m->vrregs.vrregs[i][0], m->vrregs.vrregs[i][1],
                    m->vrregs.vrregs[i][2], m->vrregs.vrregs[i][3],
                    a->vrregs.vrregs[i][0], a->vrregs.vrregs[i][1],
                    a->vrregs.vrregs[i][2], a->vrregs.vrregs[i][3]);
        }
    }
#endif
    return !ferror(f);
}

#if defined(RISU_DPPC)
static void reginfo_swap(struct reginfo *ri) {
    int i;

    ri->second_prev_insn = (uint32_t)BYTESWAP_32(ri->second_prev_insn);
    ri->prev_insn = (uint32_t)BYTESWAP_32(ri->prev_insn);
    ri->faulting_insn = (uint32_t)BYTESWAP_32(ri->faulting_insn);
    ri->next_insn = (uint32_t)BYTESWAP_32(ri->next_insn);
    ri->nip = (uint32_t)BYTESWAP_32(ri->nip);
    for (i = 0; i < risu_NGREG; i++) {
        if (sizeof(ri->gregs[i]) == 8)
            ri->gregs[i] = BYTESWAP_64(ri->gregs[i]);
        else
            ri->gregs[i] = (uint32_t)BYTESWAP_32(ri->gregs[i]);
    }
    for (i = 0; i < 32; i++) {
        ri->fpregs[i] = BYTESWAP_64(ri->fpregs[i]);
    }
    if (sizeof(ri->fpscr) == 8)
        ri->fpscr = BYTESWAP_64(ri->fpscr);
    else
        ri->fpscr = (uint32_t)BYTESWAP_32(ri->fpscr);
#ifdef VRREGS
    int j;
    for (i = 0; i < 32; i++) {
        for (j = 0; j < 4; j++) {
            ri->vrregs[i][j] = BYTESWAP_32(ri->vrregs[i][j]);
        }
    }
    for (i = 0; i < 4; i++) {
        ri->vscr[4] = BYTESWAP_32(ri->vscr[4]);
    }
    ri->vrsave = BYTESWAP_32(ri->vrsave);
    ri->vrsave2 = BYTESWAP_32(ri->vrsave2);
    ri->save_vrvalid = BYTESWAP_32(ri->save_vrvalid);
#endif
}
#endif

void reginfo_host_to_arch(struct reginfo *ri) {
#if defined(RISU_DPPC)
    reginfo_swap(ri);
#else
    #pragma unused(ri)
#endif
}

void reginfo_arch_to_host(struct reginfo *ri) {
#if defined(RISU_DPPC)
    reginfo_swap(ri);
#else
    #pragma unused(ri)
#endif
}
