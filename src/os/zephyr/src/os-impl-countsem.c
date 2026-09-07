/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/kernel/obj_core.h>

#include "os-impl-countsem.h"
#include "os-shared-countsem.h"
#include "os-shared-idmap.h"

OS_impl_countsem_internal_record_t OS_impl_count_sem_table[OS_MAX_COUNT_SEMAPHORES];

int32 OS_CountSemCreate_Impl(const OS_object_token_t *token, uint32 sem_initial_value, uint32 options)
{
    OS_impl_countsem_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_count_sem_table, *token);

    ARG_UNUSED(options);

    /* GetInfo reports a signed 32-bit count. Never accept an initial value
     * that cannot be represented by that public property. */
    if (sem_initial_value > INT32_MAX)
    {
        return OS_INVALID_SEM_VALUE;
    }
    if (!impl->initialized)
    {
        if (k_mutex_init(&impl->lock) != 0)
        {
            return OS_SEM_FAILURE;
        }
        impl->initialized = true;
    }
    return k_sem_init(&impl->sem, sem_initial_value, INT32_MAX) == 0 ? OS_SUCCESS : OS_SEM_FAILURE;
}

int32 OS_CountSemDelete_Impl(const OS_object_token_t *token)
{
    OS_impl_countsem_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_count_sem_table, *token);

    ARG_UNUSED(impl);
    /* This initial provider requires quiescent deletion. Remove the native
     * registry entry before a later Create registers the storage again. */
#ifdef CONFIG_OBJ_CORE_SEM
    k_obj_core_unlink(K_OBJ_CORE(&impl->sem));
#endif
    return OS_SUCCESS;
}

int32 OS_CountSemGive_Impl(const OS_object_token_t *token)
{
    OS_impl_countsem_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_count_sem_table, *token);
    int32                             status;

    /* Serialize givers so the overflow check cannot race another increment.
     * Concurrent takers can only decrease the count. */
    if (k_mutex_lock(&impl->lock, K_FOREVER) != 0)
    {
        return OS_SEM_FAILURE;
    }
    if (k_sem_count_get(&impl->sem) == INT32_MAX)
    {
        status = OS_SEM_FAILURE;
    }
    else
    {
        k_sem_give(&impl->sem);
        status = OS_SUCCESS;
    }
    k_mutex_unlock(&impl->lock);
    return status;
}

static int32 OS_Zephyr_CountSemTake(const OS_object_token_t *token, k_timeout_t timeout)
{
    OS_impl_countsem_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_count_sem_table, *token);
    int                               status = k_sem_take(&impl->sem, timeout);

    if (status == 0)
    {
        return OS_SUCCESS;
    }
    return status == -EAGAIN || status == -EBUSY ? OS_SEM_TIMEOUT : OS_SEM_FAILURE;
}

int32 OS_CountSemTake_Impl(const OS_object_token_t *token)
{
    return OS_Zephyr_CountSemTake(token, K_FOREVER);
}

int32 OS_CountSemTimedWait_Impl(const OS_object_token_t *token, uint32 msecs)
{
    return OS_Zephyr_CountSemTake(token, K_MSEC(msecs));
}

int32 OS_CountSemGetInfo_Impl(const OS_object_token_t *token, OS_count_sem_prop_t *count_prop)
{
    OS_impl_countsem_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_count_sem_table, *token);

    count_prop->value = (int32)k_sem_count_get(&impl->sem);
    return OS_SUCCESS;
}
