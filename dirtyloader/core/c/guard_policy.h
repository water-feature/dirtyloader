/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DIRTYLOADER_GUARD_POLICY_H
#define DIRTYLOADER_GUARD_POLICY_H

#include <stdint.h>

#define G_SKIP 0x50494B53u   /* "SKIP" */
#define G_GOOD 0x444F4F47u   /* "GOOD" */
#define G_ARMD 0x444D5241u   /* legacy startup marker */

enum guard_skip_reason { SKIP_NONE, SKIP_CRASH, SKIP_GUARD, SKIP_HOST = 4 };

static inline enum guard_skip_reason guard_skip_reason(uint32_t marker, int mod_fault, int host_skip)
{
    if (mod_fault) return SKIP_CRASH;
    if (host_skip) return SKIP_HOST;
    return marker == G_SKIP ? SKIP_GUARD : SKIP_NONE;
}

#endif
