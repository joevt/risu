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
    image_start();
}

int reginfo_size(struct reginfo *ri)
{
    return sizeof(*ri);
}

/* reginfo_init: initialize with a ucontext */
void reginfo_init(struct reginfo *ri, ucontext_t *uc, void *siaddr)
{
    int i;

    memset(ri, 0, sizeof(*ri));

    ri->faulting_insn = *((uint32_t *) uc->uc_mcontext.regs->nip);
    ri->nip = uc->uc_mcontext.regs->nip - image_start_address;

    for (i = 0; i < NGREG; i++) {
        ri->gregs[i] = uc->uc_mcontext.gp_regs[i];
    }

    memcpy(ri->fpregs, uc->uc_mcontext.fp_regs, 32 * sizeof(double));
    ri->fpscr = uc->uc_mcontext.fp_regs[32];

#ifdef VRREGS
    memcpy(ri->vrregs.vrregs, uc->uc_mcontext.v_regs->vrregs,
           sizeof(ri->vrregs.vrregs[0]) * 32);
    ri->vrregs.vscr = uc->uc_mcontext.v_regs->vscr;
    ri->vrregs.vrsave = uc->uc_mcontext.v_regs->vrsave;
#endif

#ifdef SAVESTACK
    memcpy(ri->stack, (uint8_t*)ri->gregs[1] - (sizeof(ri->stack) / 2), sizeof(ri->stack));
#endif
}

/* reginfo_update: update the context */
void reginfo_update(struct reginfo *ri, ucontext_t *uc, void *siaddr)
{
}

/* reginfo_is_eq: compare the reginfo structs, returns nonzero if equal */
int reginfo_is_eq(struct reginfo *m, struct reginfo *a)
{
    int i;
    for (i = 0; i < 32; i++) {
        if (i == 1 || i == 13) {
            continue;
        }

        if (m->gregs[i] != a->gregs[i]) {
            return 0;
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

    fprintf(f, "previous insn @ 0x%0" PRIx " : 0x%08x\n", ri->nip - 4, ri->prev_insn);
    fprintf(f, "faulting insn @ 0x%0" PRIx " : 0x%08x\n", ri->nip, ri->faulting_insn);
    fprintf(f, "\n");

    for (i = 0; i < 16; i++) {
        fprintf(f, "\tr%d%s : %0" PRIx "\tr%d : %0" PRIx "\n", i, i < 10 ? " " : "", ri->gregs[i],
                i + 16, ri->gregs[i + 16]);
    }

    fprintf(f, "\n");
    fprintf(f, "\tnip    : %0" PRIx "\n", ri->gregs[32]);
    fprintf(f, "\tmsr    : %0" PRIx "\n", ri->gregs[33]);
    fprintf(f, "\torig r3: %0" PRIx "\n", ri->gregs[34]);
    fprintf(f, "\tctr    : %0" PRIx "\n", ri->gregs[35]);
    fprintf(f, "\tlnk    : %0" PRIx "\n", ri->gregs[36]);
    fprintf(f, "\txer    : %0" PRIx "\n", ri->gregs[37]);
    fprintf(f, "\tccr    : %0" PRIx "\n", ri->gregs[38]);
    fprintf(f, "\tmq     : %0" PRIx "\n", ri->gregs[39]);
    fprintf(f, "\ttrap   : %0" PRIx "\n", ri->gregs[40]);
    fprintf(f, "\tdar    : %0" PRIx "\n", ri->gregs[41]);
    fprintf(f, "\tdsisr  : %0" PRIx "\n", ri->gregs[42]);
    fprintf(f, "\tresult : %0" PRIx "\n", ri->gregs[43]);
    fprintf(f, "\tdscr   : %0" PRIx "\n\n", ri->gregs[44]);

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
