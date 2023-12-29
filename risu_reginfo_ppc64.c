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

#include <stdio.h>
#include <signal.h>
#include <ucontext.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/user.h>

#include "risu.h"
#include "risu_reginfo_ppc64.h"

#define XER 37
#define CCR 38

static int ccr_mask    = 0xFFFFFFFF; /* Bit mask of CCR bits to compare. */
static int fpscr_mask  = 0xFFFFFFFF; /* Bit mask of FPSCR bits to compare. */
static int fpregs_mask = 0xFFFFFFFF; /* Bit mask of FP registers to compare. */
static int vrregs_mask = 0xFFFFFFFF; /* Bit mask of FP registers to compare. */

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
    int val = strtoul(arg, 0, 16);
    switch (opt - FIRST_ARCH_OPT) {
        case 0: ccr_mask    = val; break;
        case 1: fpscr_mask  = val; break;
        case 2: fpregs_mask = val; break;
        case 3: vrregs_mask = val; break;
    }
}

void arch_init(void)
{
}

void do_image()
{
    uint8_t stack_bytes[32768];
    for (int i = 0; i < sizeof(stack_bytes); i++)
        stack_bytes[i] = i;
    image_start();
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
void reginfo_init(struct reginfo *ri, ucontext_t *uc, void *siaddr)
{
#ifndef __APPLE__
    int i;
#else
    if (uc->uc_mcsize < mcontext_min_size) {
        mcontext_min_size = uc->uc_mcsize;
    }
    if (uc->uc_mcsize > mcontext_max_size) {
        mcontext_max_size = uc->uc_mcsize;
    }
#endif

    memset(ri, 0, sizeof(*ri));

    uintptr_t pc = get_uc_pc(uc, siaddr);
    uintptr_t ibegin = image_start_address;
    uintptr_t iend = image_start_address + image_size;
    ri->second_prev_insn = (pc - 8) >= ibegin && (pc - 8) < iend ? *((uint32_t *) (pc - 8)) : 0;
    ri->prev_insn        = (pc - 4) >= ibegin && (pc - 4) < iend ? *((uint32_t *) (pc - 4)) : 0;
    ri->faulting_insn    = (pc + 0) >= ibegin && (pc + 0) < iend ? *((uint32_t *) (pc + 0)) : 0;
    ri->next_insn        = (pc + 4) >= ibegin && (pc + 4) < iend ? *((uint32_t *) (pc + 4)) : 0;
    ri->nip = pc - ibegin;

#ifndef __APPLE__
    for (i = 0; i < NGREG; i++) {
        ri->gregs[i] = uc->uc_mcontext.gp_regs[i];
    }
#else
    ri->gregs[ 0] = uc->uc_mcontext->ss.r0;
    ri->gregs[ 1] = uc->uc_mcontext->ss.r1;
    ri->gregs[ 2] = uc->uc_mcontext->ss.r2;
    ri->gregs[ 3] = uc->uc_mcontext->ss.r3;
    ri->gregs[ 4] = uc->uc_mcontext->ss.r4;
    ri->gregs[ 5] = uc->uc_mcontext->ss.r5;
    ri->gregs[ 6] = uc->uc_mcontext->ss.r6;
    ri->gregs[ 7] = uc->uc_mcontext->ss.r7;
    ri->gregs[ 8] = uc->uc_mcontext->ss.r8;
    ri->gregs[ 9] = uc->uc_mcontext->ss.r9;
    ri->gregs[10] = uc->uc_mcontext->ss.r10;
    ri->gregs[11] = uc->uc_mcontext->ss.r11;
    ri->gregs[12] = uc->uc_mcontext->ss.r12;
    ri->gregs[13] = uc->uc_mcontext->ss.r13;
    ri->gregs[14] = uc->uc_mcontext->ss.r14;
    ri->gregs[15] = uc->uc_mcontext->ss.r15;
    ri->gregs[16] = uc->uc_mcontext->ss.r16;
    ri->gregs[17] = uc->uc_mcontext->ss.r17;
    ri->gregs[18] = uc->uc_mcontext->ss.r18;
    ri->gregs[19] = uc->uc_mcontext->ss.r19;
    ri->gregs[20] = uc->uc_mcontext->ss.r20;
    ri->gregs[21] = uc->uc_mcontext->ss.r21;
    ri->gregs[22] = uc->uc_mcontext->ss.r22;
    ri->gregs[23] = uc->uc_mcontext->ss.r23;
    ri->gregs[24] = uc->uc_mcontext->ss.r24;
    ri->gregs[25] = uc->uc_mcontext->ss.r25;
    ri->gregs[26] = uc->uc_mcontext->ss.r26;
    ri->gregs[27] = uc->uc_mcontext->ss.r27;
    ri->gregs[28] = uc->uc_mcontext->ss.r28;
    ri->gregs[29] = uc->uc_mcontext->ss.r29;
    ri->gregs[30] = uc->uc_mcontext->ss.r30;
    ri->gregs[31] = uc->uc_mcontext->ss.r31;
    ri->gregs[32] = pc; /* NIP */
    ri->gregs[33] = uc->uc_mcontext->ss.srr1; /* MSR */
    ri->gregs[34] = 0; /* orig r3 */
    ri->gregs[35] = uc->uc_mcontext->ss.ctr;
    ri->gregs[36] = uc->uc_mcontext->ss.lr;
    ri->gregs[37] = uc->uc_mcontext->ss.xer;
    ri->gregs[38] = uc->uc_mcontext->ss.cr;
    ri->gregs[39] = uc->uc_mcontext->ss.mq;
    ri->gregs[40] = 0; /* TRAP */
    ri->gregs[41] = uc->uc_mcontext->es.dar; /* DAR */
    ri->gregs[42] = uc->uc_mcontext->es.dsisr; /* DSISR */
    ri->gregs[43] = 0; /* RESULT */
    ri->gregs[44] = 0; /* DSCR */
#endif

#ifndef __APPLE__
    memcpy(ri->fpregs, uc->uc_mcontext.fp_regs, 32 * sizeof(double));
    ri->fpscr = uc->uc_mcontext.fp_regs[32];
#else
    memcpy(ri->fpregs, uc->uc_mcontext->fs.fpregs, 32 * sizeof(double));
    ri->fpscr = uc->uc_mcontext->fs.fpscr;
#endif

#ifdef VRREGS
#ifndef __APPLE__
    memcpy(ri->vrregs.vrregs, uc->uc_mcontext.v_regs->vrregs,
           sizeof(ri->vrregs.vrregs[0]) * 32);
    ri->vrregs.vscr = uc->uc_mcontext.v_regs->vscr;
    ri->vrregs.vrsave = uc->uc_mcontext.v_regs->vrsave;
#else
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
#endif
#endif

#ifdef SAVESTACK
    memcpy(ri->stack, (uint8_t*)ri->gregs[1] - (sizeof(ri->stack) / 2), sizeof(ri->stack));
#endif
}

/* reginfo_update: update the context */
void reginfo_update(struct reginfo *ri, ucontext_t *uc, void *siaddr)
{
#ifndef __APPLE__
    uc->uc_mcontext.gp_regs[38] = ri->gregs[38];
#else
    uc->uc_mcontext->ss.cr = ri->gregs[38];
#endif

#ifndef __APPLE__
    memcpy(uc->uc_mcontext.fp_regs, ri->fpregs, 32 * sizeof(double));
    uc->uc_mcontext.fp_regs[32] = ri->fpscr;
#else
    memcpy(uc->uc_mcontext->fs.fpregs, ri->fpregs, 32 * sizeof(double));
    uc->uc_mcontext->fs.fpscr = ri->fpscr;
#endif

#ifdef VRREGS
#ifndef __APPLE__
    uc->uc_mcontext.v_regs->vscr = ri->vrregs.vscr;
#else
    memcpy(uc->uc_mcontext->vs.save_vscr, ri->vrregs.vscr,
           sizeof(ri->vrregs.vscr[0]) * 4);
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

    if (m->gregs[XER] != a->gregs[XER]) {
        return 0;
    }

    if (m->gregs[CCR] != a->gregs[CCR]) {
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
            (m->gregs[CCR] & mask) == (a->gregs[CCR] & mask)
        ) {
            a->gregs[CCR] = m->gregs[CCR];
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

    fprintf(f, "2nd prev insn @ 0x%0" PRIx " : 0x%08x\n", ri->nip - 8, ri->second_prev_insn);
    fprintf(f, "previous insn @ 0x%0" PRIx " : 0x%08x\n", ri->nip - 4, ri->prev_insn);
    fprintf(f, "faulting insn @ 0x%0" PRIx " : 0x%08x\n", ri->nip + 0, ri->faulting_insn);
    fprintf(f, "    next insn @ 0x%0" PRIx " : 0x%08x\n", ri->nip + 4, ri->next_insn);
    fprintf(f, "\n");

    for (i = 0; i < 16; i++) {
        fprintf(f, "\tr%d%s : %0" PRIx "\tr%d : %0" PRIx "\n", i, i < 10 ? " " : "", ri->gregs[i],
                i + 16, ri->gregs[i + 16]);
    }

    fprintf(f, "\n");
    fprintf(f, "\tnip    : %0" PRIx "\n", ri->gregs[32]);
    fprintf(f, "\tmsr    : %0" PRIx "\n", ri->gregs[33]);
#ifndef __APPLE__
    fprintf(f, "\torig r3: %0" PRIx "\n", ri->gregs[34]);
#endif
    fprintf(f, "\tctr    : %0" PRIx "\n", ri->gregs[35]);
    fprintf(f, "\tlnk    : %0" PRIx "\n", ri->gregs[36]);
    fprintf(f, "\txer    : %0" PRIx "\n", ri->gregs[37]);
    fprintf(f, "\tccr    : %0" PRIx "\n", ri->gregs[38]);
    fprintf(f, "\tmq     : %0" PRIx "\n", ri->gregs[39]);
#ifndef __APPLE__
    fprintf(f, "\ttrap   : %0" PRIx "\n", ri->gregs[40]);
    fprintf(f, "\tdar    : %0" PRIx "\n", ri->gregs[41]);
    fprintf(f, "\tdsisr  : %0" PRIx "\n", ri->gregs[42]);
    fprintf(f, "\tresult : %0" PRIx "\n", ri->gregs[43]);
    fprintf(f, "\tdscr   : %0" PRIx "\n\n", ri->gregs[44]);
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
#ifdef __APPLE__
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
            fprintf(f, "Mismatch: Register r%d ", i);
            fprintf(f, "master: [%0" PRIx "] - apprentice: [%0" PRIx "]\n",
                    m->gregs[i], a->gregs[i]);
        }
    }

    if (m->gregs[XER] != a->gregs[XER]) {
        fprintf(f, "Mismatch: XER ");
        fprintf(f, "m: [%0" PRIx "] != a: [%0" PRIx "]\n", m->gregs[XER], a->gregs[XER]);
    }

    if (m->gregs[CCR] != a->gregs[CCR]) {
        fprintf(f, "Mismatch: Cond. Register ");
        fprintf(f, "m: [%0" PRIx "] != a: [%0" PRIx "]\n", m->gregs[CCR], a->gregs[CCR]);
    }

    for (i = 0; i < 32; i++) {
        if (m->fpregs[i] != a->fpregs[i]) {
            fprintf(f, "Mismatch: Register f%d ", i);
            fprintf(f, "m: [%016" PRIx64 "] != a: [%016" PRIx64 "]\n",
                    m->fpregs[i], a->fpregs[i]);
        }
    }

#ifdef VRREGS
    for (i = 0; i < 32; i++) {
        if (m->vrregs.vrregs[i][0] != a->vrregs.vrregs[i][0] ||
            m->vrregs.vrregs[i][1] != a->vrregs.vrregs[i][1] ||
            m->vrregs.vrregs[i][2] != a->vrregs.vrregs[i][2] ||
            m->vrregs.vrregs[i][3] != a->vrregs.vrregs[i][3]) {

            fprintf(f, "Mismatch: Register vr%d ", i);
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
