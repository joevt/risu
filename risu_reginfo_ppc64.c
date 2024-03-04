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
#include <sys/user.h>

#ifdef DPPC
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

static uint32_t ccr_mask    = 0xFFFFFFFF; /* Bit mask of CCR bits to compare. */
static uint32_t fpscr_mask  = 0xFFFFFFFF; /* Bit mask of FPSCR bits to compare. */
static uint32_t fpregs_mask = 0xFFFFFFFF; /* Bit mask of FP registers to compare. */
static uint32_t vrregs_mask = 0xFFFFFFFF; /* Bit mask of FP registers to compare. */

static const struct option extra_opts[] = {
    {"ccr_mask"   , required_argument, NULL, FIRST_ARCH_OPT + 0 },
    {"fpscr_mask" , required_argument, NULL, FIRST_ARCH_OPT + 1 },
    {"fpregs_mask", required_argument, NULL, FIRST_ARCH_OPT + 2 },
    {"vrregs_mask", required_argument, NULL, FIRST_ARCH_OPT + 3 },
    {0, 0, 0, 0}
};

const struct option * const arch_long_opts = &extra_opts[0];
const char * const arch_extra_help =
    "  --ccr_mask=MASK    Mask of CCR bits to compare\n"
    "  --fpscr_mask=MASK  Mask of FPSCR bits to compare\n"
    "  --fpregs_mask=MASK Mask of fpregs to compare\n"
    "  --vrregs_mask=MASK Mask of vrregs to compare\n";

void process_arch_opt(int opt, const char *arg)
{
    assert(opt >= FIRST_ARCH_OPT && opt <= FIRST_ARCH_OPT + 3);
    uint32_t val = (uint32_t)strtoul(arg, 0, 16);
    switch (opt - FIRST_ARCH_OPT) {
        case 0: ccr_mask    = val; break;
        case 1: fpscr_mask  = val; break;
        case 2: fpregs_mask = val; break;
        case 3: vrregs_mask = val; break;
    }
}

arch_ptr_t get_arch_start_address() {
#ifdef DPPC
    return DPPC_RISU_ROM_START;
#else
    return (arch_ptr_t)image_start_address;
#endif
}

void * get_arch_memory(arch_ptr_t arch_memblock) {
#ifdef DPPC
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
#ifdef DPPC
    mem_ctrl = new MemCtrlBase;

    // configure CPU clocks
    uint64_t bus_freq      = 50000000ULL;
    uint64_t timebase_freq = bus_freq / 4;

    // initialize virtual CPU and request MPC750 CPU aka G3
    ppc_cpu_init(mem_ctrl, PPC_VER::MPC750, timebase_freq);

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
#ifdef DPPC
    uint8_t *stack_bytes = (uint8_t *)get_arch_memory(DPPC_RISU_STACK + 0x3C);
    for (i = 0; i < 32768; i++)
        stack_bytes[i] = i;

    power_on = true;
    ppc_exec();
#else
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

#ifdef __APPLE__
size_t mcontext_min_size = -1;
size_t mcontext_max_size = 0;
#endif

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
#ifdef NO_SIGNAL
    void *uc = vuc;
#else
    ucontext_t *uc = (ucontext_t *)vuc;
#endif

#ifndef __APPLE__
#else
    if (uc->uc_mcsize < mcontext_min_size) {
        mcontext_min_size = uc->uc_mcsize;
    }
    if (uc->uc_mcsize > mcontext_max_size) {
        mcontext_max_size = uc->uc_mcsize;
    }
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

#if defined(DPPC)
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

#if defined(DPPC)
    memcpy(ri->fpregs, ppc_state.fpr, 32 * sizeof(double));
    ri->fpscr = ppc_state.fpscr;
#elif defined(__APPLE__)
    memcpy(ri->fpregs, uc->uc_mcontext->fs.fpregs, 32 * sizeof(double));
    ri->fpscr = uc->uc_mcontext->fs.fpscr;
#else
    memcpy(ri->fpregs, uc->uc_mcontext.fp_regs, 32 * sizeof(double));
    ri->fpscr = uc->uc_mcontext.fp_regs[32];
#endif

#ifdef VRREGS
#if defined(DPPC)
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
#if defined(DPPC)
    ppc_state.cr = ri->gregs[risu_CCR];
#elif defined(__APPLE__)
    ucontext_t *uc = (ucontext_t *)vuc;
    uc->uc_mcontext->ss.cr = ri->gregs[risu_CCR];
#else
    ucontext_t *uc = (ucontext_t *)vuc;
    uc->uc_mcontext.gp_regs[CR] = ri->gregs[risu_CCR];
#endif

#if defined(DPPC)
    memcpy(ppc_state.fpr, ri->fpregs, 32 * sizeof(double));
    ppc_state.fpscr = ri->fpscr;
#elif defined(__APPLE__)
    memcpy(uc->uc_mcontext->fs.fpregs, ri->fpregs, 32 * sizeof(double));
    uc->uc_mcontext->fs.fpscr = ri->fpscr;
#else
    memcpy(uc->uc_mcontext.fp_regs, ri->fpregs, 32 * sizeof(double));
    uc->uc_mcontext.fp_regs[32] = ri->fpscr;
#endif

#ifdef VRREGS
#if defined(DPPC)
#elif defined(__APPLE__)
    memcpy(uc->uc_mcontext->vs.save_vscr, ri->vrregs.vscr,
           sizeof(ri->vrregs.vscr[0]) * 4);
#else
    uc->uc_mcontext.v_regs->vscr = ri->vrregs.vscr;
#endif
#endif
}

/* reginfo_is_eq: compare the reginfo structs, returns nonzero if equal */
int reginfo_is_eq(struct reginfo *m, struct reginfo *a)
{
    uint32_t gregs_mask = ~((1 << (31-1)) | (1 << (31-13))); /* ignore r1 and r13 */
    if (get_risuop(a) == OP_SIGILL && (a->next_insn & (0xe << 26)) == (0xe << 26)) {
        gregs_mask &= ~(1 << (31-((a->next_insn >> 21) & 31)));
    }

    int i;
    for (i = 0; i < 32; i++) {
        if (m->gregs[i] != a->gregs[i]) {
            if ((1 << (31-i)) & ~gregs_mask) {
                /* a->gregs[i] = m->fpregs[i]; */
            } else {
                return 0;
            }
        }
    }

    if (m->gregs[risu_XER] != a->gregs[risu_XER]) {
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
        if (
            (m->gregs[risu_CCR] & mask) == (a->gregs[risu_CCR] & mask)
        ) {
            a->gregs[risu_CCR] = m->gregs[risu_CCR];
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
            if ((1 << (31-i)) & ~fpregs_mask) {
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
#if !defined(__APPLE__) && !defined(DPPC)
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
#if defined(__APPLE__) && !defined(DPPC)
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
        if (i == 1 || i == 13) {
            continue;
        }

        if (m->gregs[i] != a->gregs[i]) {
            fprintf(f, "Mismatch: r%d ", i);
            fprintf(f, "m: [%0" PRIx "] != a: [%0" PRIx "]\n",
                    m->gregs[i], a->gregs[i]);
        }
    }

    if (m->gregs[risu_XER] != a->gregs[risu_XER]) {
        fprintf(f, "Mismatch: xer ");
        fprintf(f, "m: [%0" PRIx "] != a: [%0" PRIx "]\n", m->gregs[risu_XER], a->gregs[risu_XER]);
    }

    if (m->gregs[risu_CCR] != a->gregs[risu_CCR]) {
        fprintf(f, "Mismatch: ccr ");
        fprintf(f, "m: [%0" PRIx "] != a: [%0" PRIx "]\n", m->gregs[risu_CCR], a->gregs[risu_CCR]);
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

#if defined(DPPC)
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
#if defined(DPPC)
    reginfo_swap(ri);
#endif
}

void reginfo_arch_to_host(struct reginfo *ri) {
#if defined(DPPC)
    reginfo_swap(ri);
#endif
}
