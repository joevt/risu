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

#ifndef RISU_REGINFO_S390X_H
#define RISU_REGINFO_S390X_H

struct reginfo {
    uint64_t psw_mask;
    uint64_t pc_offset;
    uint64_t faulting_insn;
    int faulting_insn_len;
    uint32_t fpc;
    uint64_t gprs[16];
    uint64_t fprs[16];
};

#endif /* RISU_REGINFO_S390X_H */
