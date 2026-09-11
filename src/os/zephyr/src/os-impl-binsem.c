/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/clock.h>

#include "os-impl-binsem.h"
#include "os-impl-tasks.h"
#include "os-shared-binsem.h"
#include "os-shared-idmap.h"

OS_impl_binsem_internal_record_t OS_impl_bin_sem_table[OS_MAX_BIN_SEMAPHORES];

/*
 * Give/Take/Flush/TimedWait use unpinned shared tokens. A token can outlive
 * deletion and slot reuse before entering this provider. Keep each slot's
 * kernel objects for the lifetime of the port, and validate the full ID
 * under its mutex before accessing state, including after every wait.
 * No reference or waiter count is retained across a kernel wait.
 */
static bool OS_Zephyr_BinSemMatches(const OS_impl_binsem_internal_record_t *impl,
                                  const OS_object_token_t *token)
{
    return impl->active && OS_ObjectIdEqual(impl->object_id, OS_ObjectIdFromToken(token));
}

int32 OS_BinSemCreate_Impl(const OS_object_token_t *token, uint32 sem_initial_value, uint32 options)
{
    OS_impl_binsem_internal_record_t *impl;

    OS_Zephyr_TaskEnter();

    /* Matches the historical binary semaphore behavior: an out-of-range
     * initial value is silently normalized rather than rejected. */
    if (sem_initial_value > 1)
    {
        sem_initial_value = 1;
    }

    impl = OS_OBJECT_TABLE_GET(OS_impl_bin_sem_table, *token);

    /* Shared allocation serializes the first initialization of a slot.
     * Later creations must not reinitialize locks that old calls may use. */
    if (!impl->initialized)
    {
        if (k_mutex_init(&impl->lock) != 0 || k_condvar_init(&impl->changed) != 0)
        {
            return OS_Zephyr_TaskLeaveResult(OS_SEM_FAILURE);
        }
        impl->initialized = true;
    }

    if (k_mutex_lock(&impl->lock, K_FOREVER) != 0)
    {
        return OS_Zephyr_TaskLeaveResult(OS_SEM_FAILURE);
    }
    if (impl->active)
    {
        k_mutex_unlock(&impl->lock);
        return OS_Zephyr_TaskLeaveResult(OS_SEM_FAILURE);
    }

    impl->object_id     = OS_ObjectIdFromToken(token);
    impl->active        = true;
    impl->current_value = sem_initial_value;
    impl->flush_request = 0;
    k_mutex_unlock(&impl->lock);

    return OS_Zephyr_TaskLeaveResult(OS_SUCCESS);
}

int32 OS_BinSemDelete_Impl(const OS_object_token_t *token)
{
    OS_impl_binsem_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_bin_sem_table, *token);

    OS_Zephyr_TaskEnter();

    /* A failed deletion leaves the old generation usable. A sleeping
     * waiter has released this lock and may be invalidated safely. */
    if (k_mutex_lock(&impl->lock, K_NO_WAIT) != 0)
    {
        return OS_Zephyr_TaskLeaveResult(OS_SEM_FAILURE);
    }
    if (!OS_Zephyr_BinSemMatches(impl, token))
    {
        k_mutex_unlock(&impl->lock);
        return OS_Zephyr_TaskLeaveResult(OS_ERR_INVALID_ID);
    }

    impl->active = false;
    k_condvar_broadcast(&impl->changed);
    k_mutex_unlock(&impl->lock);

    return OS_Zephyr_TaskLeaveResult(OS_SUCCESS);
}

int32 OS_BinSemGive_Impl(const OS_object_token_t *token)
{
    OS_impl_binsem_internal_record_t *impl;

    OS_Zephyr_TaskEnter();

    impl = OS_OBJECT_TABLE_GET(OS_impl_bin_sem_table, *token);

    if (k_mutex_lock(&impl->lock, K_FOREVER) != 0)
    {
        return OS_Zephyr_TaskLeaveResult(OS_SEM_FAILURE);
    }
    if (!OS_Zephyr_BinSemMatches(impl, token))
    {
        k_mutex_unlock(&impl->lock);
        return OS_Zephyr_TaskLeaveResult(OS_ERR_INVALID_ID);
    }

    /* Binary semaphores are always set to "1" when given. */
    impl->current_value = 1;
    /* Delete broadcasts before reuse, and old waiters cannot requeue
     * after their ID check fails. This signal therefore serves the
     * current generation only. */
    k_condvar_signal(&impl->changed);

    k_mutex_unlock(&impl->lock);

    return OS_Zephyr_TaskLeaveResult(OS_SUCCESS);
}

int32 OS_BinSemFlush_Impl(const OS_object_token_t *token)
{
    OS_impl_binsem_internal_record_t *impl;

    OS_Zephyr_TaskEnter();

    impl = OS_OBJECT_TABLE_GET(OS_impl_bin_sem_table, *token);

    if (k_mutex_lock(&impl->lock, K_FOREVER) != 0)
    {
        return OS_Zephyr_TaskLeaveResult(OS_SEM_FAILURE);
    }
    if (!OS_Zephyr_BinSemMatches(impl, token))
    {
        k_mutex_unlock(&impl->lock);
        return OS_Zephyr_TaskLeaveResult(OS_ERR_INVALID_ID);
    }

    /* Bump the flush generation so any concurrent Take() sees it changed
     * and returns without consuming the semaphore, then wake everyone. */
    ++impl->flush_request;
    k_condvar_broadcast(&impl->changed);

    k_mutex_unlock(&impl->lock);

    return OS_Zephyr_TaskLeaveResult(OS_SUCCESS);
}

/*
 * Shared Take/TimedWait helper. "timeout" is a deadline computed by the
 * caller via sys_timepoint_calc() so that a spurious wakeup re-derives the
 * remaining budget instead of restarting a fresh K_MSEC() window each loop
 * iteration.
 */
static int32 OS_Zephyr_BinSemTake_Impl(const OS_object_token_t *token, k_timepoint_t deadline)
{
    OS_impl_binsem_internal_record_t *impl;
    uint32                             flush_count;
    int32                              return_code;
    int                                status;

    impl = OS_OBJECT_TABLE_GET(OS_impl_bin_sem_table, *token);

    status = k_mutex_lock(&impl->lock, sys_timepoint_timeout(deadline));
    if (status != 0)
    {
        return status == -EAGAIN || status == -EBUSY ? OS_SEM_TIMEOUT : OS_SEM_FAILURE;
    }
    if (!OS_Zephyr_BinSemMatches(impl, token))
    {
        k_mutex_unlock(&impl->lock);
        return OS_ERR_INVALID_ID;
    }

    return_code = OS_SUCCESS;

    /* A concurrent Flush() is detected by comparing against this snapshot,
     * not by the value alone, so a Give() racing a Flush() cannot be
     * mistaken for the flush releasing this waiter. */
    flush_count = impl->flush_request;

    while (impl->current_value == 0 && impl->flush_request == flush_count)
    {
        status = k_condvar_wait(&impl->changed, &impl->lock, sys_timepoint_timeout(deadline));
        /* The permanent lock is reacquired even when the old object has
         * been deleted and this slot already belongs to another ID. */
        if (!OS_Zephyr_BinSemMatches(impl, token))
        {
            return_code = OS_ERR_INVALID_ID;
            break;
        }
        if (status != 0)
        {
            return_code = status == -EAGAIN ? OS_SEM_TIMEOUT : OS_SEM_FAILURE;
            break;
        }
    }

    /* A flush releases every waiter without consuming the semaphore. */
    if (return_code == OS_SUCCESS && impl->flush_request == flush_count)
    {
        impl->current_value = 0;
    }

    k_mutex_unlock(&impl->lock);

    return return_code;
}

int32 OS_BinSemTake_Impl(const OS_object_token_t *token)
{
    OS_Zephyr_TaskEnter();

    return OS_Zephyr_TaskLeaveResult(OS_Zephyr_BinSemTake_Impl(token, sys_timepoint_calc(K_FOREVER)));
}

int32 OS_BinSemTimedWait_Impl(const OS_object_token_t *token, uint32 msecs)
{
    OS_Zephyr_TaskEnter();

    return OS_Zephyr_TaskLeaveResult(OS_Zephyr_BinSemTake_Impl(token, sys_timepoint_calc(K_MSEC(msecs))));
}

int32 OS_BinSemGetInfo_Impl(const OS_object_token_t *token, OS_bin_sem_prop_t *bin_prop)
{
    OS_impl_binsem_internal_record_t *impl;

    OS_Zephyr_TaskEnter();

    impl = OS_OBJECT_TABLE_GET(OS_impl_bin_sem_table, *token);

    if (k_mutex_lock(&impl->lock, K_FOREVER) != 0)
    {
        return OS_Zephyr_TaskLeaveResult(OS_SEM_FAILURE);
    }
    if (!OS_Zephyr_BinSemMatches(impl, token))
    {
        k_mutex_unlock(&impl->lock);
        return OS_Zephyr_TaskLeaveResult(OS_ERR_INVALID_ID);
    }
    bin_prop->value = impl->current_value;
    k_mutex_unlock(&impl->lock);

    return OS_Zephyr_TaskLeaveResult(OS_SUCCESS);
}
