/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>

#include "os-impl-mutex.h"
#include "os-shared-mutex.h"
#include "os-shared-idmap.h"

OS_impl_mutex_internal_record_t OS_impl_mutex_table[OS_MAX_MUTEXES];

int32 OS_MutSemCreate_Impl(const OS_object_token_t *token, uint32 options)
{
    OS_impl_mutex_internal_record_t *impl;

    impl = OS_OBJECT_TABLE_GET(OS_impl_mutex_table, *token);

    /* Zephyr's k_mutex is always recursive and always does priority
     * inheritance, so no attribute setup is needed here, unlike the
     * POSIX port's explicit pthread_mutexattr_t configuration. */
    if (k_mutex_init(&impl->lock) != 0)
    {
        return OS_SEM_FAILURE;
    }

    return OS_SUCCESS;
}

int32 OS_MutSemDelete_Impl(const OS_object_token_t *token)
{
    /* Zephyr's k_mutex has no destroy call and no way to detect a thread
     * still holding or waiting on it, unlike pthread_mutex_destroy() on
     * the POSIX port. Deleting a mutex while held or contended is not
     * made safe here; that protocol is a later gate. */
    (void)token;

    return OS_SUCCESS;
}

int32 OS_MutSemGive_Impl(const OS_object_token_t *token)
{
    OS_impl_mutex_internal_record_t *impl;

    impl = OS_OBJECT_TABLE_GET(OS_impl_mutex_table, *token);

    if (k_mutex_unlock(&impl->lock) != 0)
    {
        return OS_SEM_FAILURE;
    }

    return OS_SUCCESS;
}

int32 OS_MutSemTake_Impl(const OS_object_token_t *token)
{
    OS_impl_mutex_internal_record_t *impl;

    impl = OS_OBJECT_TABLE_GET(OS_impl_mutex_table, *token);

    if (k_mutex_lock(&impl->lock, K_FOREVER) != 0)
    {
        return OS_SEM_FAILURE;
    }

    return OS_SUCCESS;
}

int32 OS_MutSemGetInfo_Impl(const OS_object_token_t *token, OS_mut_sem_prop_t *mut_prop)
{
    /* The shared layer fills in name/creator; there is nothing
     * Zephyr-specific to report. */
    (void)token;
    (void)mut_prop;

    return OS_SUCCESS;
}
