/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/kernel/obj_core.h>
#include <zephyr/sys/clock.h>

#include "os-impl-countsem.h"
#include "os-shared-countsem.h"
#include "os-shared-idmap.h"

OS_impl_countsem_internal_record_t OS_impl_count_sem_table[OS_MAX_COUNT_SEMAPHORES];

/* NONE tokens do not pin a shared slot. The permanent mutex protects ID
 * validation and admission. An admitted native Take retains a reservation
 * until its final access, so Delete cannot reset/reuse a late kernel waiter.
 * All operations are thread-context APIs, including Give. */
static bool OS_Zephyr_CountSemMatches(const OS_impl_countsem_internal_record_t *impl,
                                    const OS_object_token_t *token)
{
    return impl->active && OS_ObjectIdEqual(impl->object_id, OS_ObjectIdFromToken(token));
}

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
    if (k_mutex_lock(&impl->lock, K_FOREVER) != 0)
    {
        return OS_SEM_FAILURE;
    }
    if (impl->active || atomic_get(&impl->users) != 0 ||
        k_sem_init(&impl->sem, sem_initial_value, INT32_MAX) != 0)
    {
        k_mutex_unlock(&impl->lock);
        return OS_SEM_FAILURE;
    }
    impl->object_id = OS_ObjectIdFromToken(token);
    impl->active    = true;
    k_mutex_unlock(&impl->lock);
    return OS_SUCCESS;
}

int32 OS_CountSemDelete_Impl(const OS_object_token_t *token)
{
    OS_impl_countsem_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_count_sem_table, *token);

    /* The shared EXCLUSIVE transaction reserves the public ID. Never wait
     * for a giver or an admitted Take to finish; failure restores that ID. */
    if (k_mutex_lock(&impl->lock, K_NO_WAIT) != 0)
    {
        return OS_SEM_FAILURE;
    }
    if (!OS_Zephyr_CountSemMatches(impl, token))
    {
        k_mutex_unlock(&impl->lock);
        return OS_ERR_INVALID_ID;
    }
    if (atomic_get(&impl->users) != 0)
    {
        k_mutex_unlock(&impl->lock);
        return OS_SEM_FAILURE;
    }

    /* No native operation remains or can enter for this ID. Unlink before
     * Create reinitializes the semaphore, avoiding duplicate registry nodes.
     * The state mutex remains initialized across every logical generation. */
#ifdef CONFIG_OBJ_CORE_SEM
    k_obj_core_unlink(K_OBJ_CORE(&impl->sem));
#endif
    impl->active = false;
    k_mutex_unlock(&impl->lock);
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
    if (!OS_Zephyr_CountSemMatches(impl, token))
    {
        status = OS_ERR_INVALID_ID;
    }
    else if (k_sem_count_get(&impl->sem) == INT32_MAX)
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

static int32 OS_Zephyr_CountSemTake(const OS_object_token_t *token, k_timepoint_t deadline)
{
    OS_impl_countsem_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_count_sem_table, *token);
    int                               status;

    status = k_mutex_lock(&impl->lock, sys_timepoint_timeout(deadline));
    if (status != 0)
    {
        return status == -EAGAIN || status == -EBUSY ? OS_SEM_TIMEOUT : OS_SEM_FAILURE;
    }
    if (!OS_Zephyr_CountSemMatches(impl, token))
    {
        k_mutex_unlock(&impl->lock);
        return OS_ERR_INVALID_ID;
    }
    if (atomic_get(&impl->users) == LONG_MAX)
    {
        k_mutex_unlock(&impl->lock);
        return OS_SEM_FAILURE;
    }
    atomic_inc(&impl->users);
    k_mutex_unlock(&impl->lock);

    status = k_sem_take(&impl->sem, sys_timepoint_timeout(deadline));
    /* This is the last access to the slot. No cleanup mutex acquisition may
     * extend a finite wait. Delete can reuse the semaphore after this atomic
     * release because the real native operation has completely returned.
     * Forced termination before release would strand a reservation; that
     * requires the still-deferred ownership-aware task retirement protocol. */
    atomic_dec(&impl->users);
    if (status == 0)
    {
        return OS_SUCCESS;
    }
    return status == -EAGAIN || status == -EBUSY ? OS_SEM_TIMEOUT : OS_SEM_FAILURE;
}

int32 OS_CountSemTake_Impl(const OS_object_token_t *token)
{
    return OS_Zephyr_CountSemTake(token, sys_timepoint_calc(K_FOREVER));
}

int32 OS_CountSemTimedWait_Impl(const OS_object_token_t *token, uint32 msecs)
{
    /* A clockless kernel turns positive timeouts into infinite waits.
     * A 32-bit tick conversion can wrap a large millisecond argument.
     * Refuse unsupported finite waits rather than silently changing them. */
    if (msecs != 0 && !IS_ENABLED(CONFIG_SYS_CLOCK_EXISTS))
    {
        return OS_ERR_NOT_IMPLEMENTED;
    }
    if (!IS_ENABLED(CONFIG_TIMEOUT_64BIT) && k_ms_to_ticks_ceil64(msecs) > INT32_MAX)
    {
        return OS_ERR_NOT_IMPLEMENTED;
    }
    return OS_Zephyr_CountSemTake(token, sys_timepoint_calc(K_MSEC(msecs)));
}

int32 OS_CountSemGetInfo_Impl(const OS_object_token_t *token, OS_count_sem_prop_t *count_prop)
{
    OS_impl_countsem_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_count_sem_table, *token);

    if (k_mutex_lock(&impl->lock, K_FOREVER) != 0)
    {
        return OS_SEM_FAILURE;
    }
    if (!OS_Zephyr_CountSemMatches(impl, token))
    {
        k_mutex_unlock(&impl->lock);
        return OS_ERR_INVALID_ID;
    }
    count_prop->value = (int32)k_sem_count_get(&impl->sem);
    k_mutex_unlock(&impl->lock);
    return OS_SUCCESS;
}
