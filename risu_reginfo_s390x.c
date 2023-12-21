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

#include <stdio.h>
#include <signal.h>
#include <asm/ucontext.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <sys/user.h>

#include "risu.h"
#include "risu_reginfo_s390x.h"


const struct option * const arch_long_opts;
const char * const arch_extra_help;

void process_arch_opt(int opt, const char *arg)
{
    abort();
}

void arch_init(void)
{
}

int reginfo_size(struct reginfo *ri)
{
    return sizeof(*ri);
}

/* reginfo_init: initialize with a ucontext */
void reginfo_init(struct reginfo *ri, ucontext_t *uc, void *siaddr)
{
    struct ucontext_extended *uce = (struct ucontext_extended *)uc;

    memset(ri, 0, sizeof(*ri));

    /*
     * We can get the size of the instruction by looking at the
     * first two bits of the instruction
     */
    switch (*(uint8_t *)siaddr >> 6) {
    case 0:
        ri->faulting_insn = *(uint16_t *)siaddr;
        ri->faulting_insn_len = 2;
        break;
    case 3:
        ri->faulting_insn = ((*(uint32_t *)siaddr) << 16)
                            | *(uint16_t *)(siaddr + 4);
        ri->faulting_insn_len = 6;
        break;
    default:
        ri->faulting_insn = *(uint32_t *)siaddr;
        ri->faulting_insn_len = 4;
    }

    ri->psw_mask = uce->uc_mcontext.regs.psw.mask;
    ri->pc_offset = (uintptr_t)siaddr - image_start_address;

    memcpy(ri->gprs, uce->uc_mcontext.regs.gprs, sizeof(ri->gprs));

    ri->fpc = uc->uc_mcontext.fpregs.fpc;
    memcpy(ri->fprs, &uc->uc_mcontext.fpregs.fprs, sizeof(ri->fprs));
}

/* reginfo_update: update a ucontext */
void reginfo_update(struct reginfo *ri, ucontext_t *uc, void *siaddr)
{
}

/* reginfo_is_eq: compare the reginfo structs, returns nonzero if equal */
int reginfo_is_eq(struct reginfo *m, struct reginfo *a)
{
    return m->pc_offset == a->pc_offset &&
           m->fpc == a->fpc &&
           memcmp(m->gprs, a->gprs, sizeof(m->gprs)) == 0 &&
           memcmp(&m->fprs, &a->fprs, sizeof(m->fprs)) == 0;
}

/* reginfo_dump: print state to a stream, returns nonzero on success */
int reginfo_dump(struct reginfo *ri, FILE * f)
{
    int i;

    fprintf(f, "  faulting insn 0x%" PRIx64 "\n", ri->faulting_insn);
    fprintf(f, "  PSW mask      0x%" PRIx64 "\n", ri->psw_mask);
    fprintf(f, "  PC offset     0x%" PRIx64 "\n\n", ri->pc_offset);

    for (i = 0; i < 16/2; i++) {
        fprintf(f, "\tr%d: %16lx\tr%02d: %16lx\n", i, ri->gprs[i],
                i + 8, ri->gprs[i + 8]);
    }
    fprintf(f, "\n");

    for (i = 0; i < 16/2; i++) {
        fprintf(f, "\tf%d: %16lx\tf%02d: %16lx\n",
                i, *(uint64_t *)&ri->fprs[i],
                i + 8, *(uint64_t *)&ri->fprs[i + 8]);
    }
    fprintf(f, "\tFPC: %8x\n\n", ri->fpc);

    return !ferror(f);
}

int reginfo_dump_mismatch(struct reginfo *m, struct reginfo *a, FILE *f)
{
    int i;

    if (m->pc_offset != a->pc_offset) {
        fprintf(f, "Mismatch: PC offset master: [%016lx] - PC offset apprentice: [%016lx]\n",
                m->pc_offset, a->pc_offset);
    }

    for (i = 0; i < 16; i++) {
        if (m->gprs[i] != a->gprs[i]) {
            fprintf(f, "Mismatch: r%d master: [%016lx] - r%d apprentice: [%016lx]\n",
                    i, m->gprs[i], i, a->gprs[i]);
        }
    }

    for (i = 0; i < 16; i++) {
        if (*(uint64_t *)&m->fprs[i] != *(uint64_t *)&a->fprs[i]) {
            fprintf(f, "Mismatch: f%d master: [%016lx] - f%d apprentice: [%016lx]\n",
                    i, *(uint64_t *)&m->fprs[i],
                    i, *(uint64_t *)&a->fprs[i]);
        }
    }

    if (m->fpc != a->fpc) {
        fprintf(f, "Mismatch: FPC master: [%08x] - FPC apprentice: [%08x]\n",
                m->fpc, a->fpc);
    }

    return !ferror(f);
}
