/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef OS_IMPL_COUNTSEM_H
#define OS_IMPL_COUNTSEM_H

#include "osconfig.h"
#include "common_types.h"
#include <zephyr/kernel.h>

typedef struct
{
    struct k_mutex lock;
    struct k_sem   sem;
    bool           initialized;
} OS_impl_countsem_internal_record_t;

extern OS_impl_countsem_internal_record_t OS_impl_count_sem_table[OS_MAX_COUNT_SEMAPHORES];

#endif /* OS_IMPL_COUNTSEM_H */
