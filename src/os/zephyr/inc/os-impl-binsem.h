/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef OS_IMPL_BINSEM_H
#define OS_IMPL_BINSEM_H

#include "osconfig.h"
#include "common_types.h"
#include <zephyr/kernel.h>

typedef struct
{
    struct k_mutex   lock;
    struct k_condvar changed;
    osal_id_t        object_id;
    bool             initialized;
    bool             active;
    uint32           current_value;
    uint32           flush_request;
} OS_impl_binsem_internal_record_t;

extern OS_impl_binsem_internal_record_t OS_impl_bin_sem_table[OS_MAX_BIN_SEMAPHORES];

#endif /* OS_IMPL_BINSEM_H */
