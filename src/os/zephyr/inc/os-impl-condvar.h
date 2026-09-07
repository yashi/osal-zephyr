/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef OS_IMPL_CONDVAR_H
#define OS_IMPL_CONDVAR_H

#include <zephyr/kernel.h>

#include "osconfig.h"
#include "common_types.h"

typedef struct
{
    struct k_mutex   lock;
    struct k_condvar changed;
    struct k_mutex   state_lock;
    osal_id_t        object_id;
    uint32           depth;
    uint32           waiters;
    bool             initialized;
    bool             active;
} OS_impl_condvar_internal_record_t;

extern OS_impl_condvar_internal_record_t OS_impl_condvar_table[OS_MAX_CONDVARS];

#endif /* OS_IMPL_CONDVAR_H */
