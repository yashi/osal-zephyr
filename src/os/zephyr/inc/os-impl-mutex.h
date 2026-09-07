/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef OS_IMPL_MUTEX_H
#define OS_IMPL_MUTEX_H

#include "osconfig.h"
#include "common_types.h"
#include <zephyr/kernel.h>

typedef struct
{
    struct k_mutex lock;
    osal_id_t      object_id;
    uint32         depth;
    bool           initialized;
    bool           active;
} OS_impl_mutex_internal_record_t;

extern OS_impl_mutex_internal_record_t OS_impl_mutex_table[OS_MAX_MUTEXES];

#endif /* OS_IMPL_MUTEX_H */
