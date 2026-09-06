/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/clock.h>

#include "os-impl-binsem.h"
#include "os-shared-binsem.h"
#include "os-shared-idmap.h"

OS_impl_binsem_internal_record_t OS_impl_bin_sem_table[OS_MAX_BIN_SEMAPHORES];

/*
 * The public API only calls Give/Take/Flush/TimedWait/GetInfo with
 * OS_LOCK_MODE_NONE, so the per-semaphore lock and condition variable
 * below are the only serialization these operations get; the shared
 * global table lock is not held across them.
 */

int32 OS_BinSemCreate_Impl(const OS_object_token_t *token, uint32 sem_initial_value, uint32 options)
{
    OS_impl_binsem_internal_record_t *impl;

    /* Matches the historical binary semaphore behavior: an out-of-range
     * initial value is silently normalized rather than rejected. */
    if (sem_initial_value > 1)
    {
        sem_initial_value = 1;
    }

    impl = OS_OBJECT_TABLE_GET(OS_impl_bin_sem_table, *token);

    if (k_mutex_init(&impl->lock) != 0 || k_condvar_init(&impl->changed) != 0)
    {
        return OS_SEM_FAILURE;
    }

    impl->current_value = sem_initial_value;
    impl->flush_request = 0;

    return OS_SUCCESS;
}

int32 OS_BinSemDelete_Impl(const OS_object_token_t *token)
{
    /* Zephyr's k_mutex/k_condvar have no destroy call and no way to detect
     * a thread still waiting on them, unlike pthread_cond_destroy() on the
     * POSIX port. Deleting a semaphore that a task is still blocked in
     * Take() on is not made safe here; that protocol is a later gate. */
    (void)token;

    return OS_SUCCESS;
}

int32 OS_BinSemGive_Impl(const OS_object_token_t *token)
{
    OS_impl_binsem_internal_record_t *impl;

    impl = OS_OBJECT_TABLE_GET(OS_impl_bin_sem_table, *token);

    if (k_mutex_lock(&impl->lock, K_FOREVER) != 0)
    {
        return OS_SEM_FAILURE;
    }

    /* Binary semaphores are always set to "1" when given. */
    impl->current_value = 1;
    k_condvar_signal(&impl->changed);

    k_mutex_unlock(&impl->lock);

    return OS_SUCCESS;
}

int32 OS_BinSemFlush_Impl(const OS_object_token_t *token)
{
    OS_impl_binsem_internal_record_t *impl;

    impl = OS_OBJECT_TABLE_GET(OS_impl_bin_sem_table, *token);

    if (k_mutex_lock(&impl->lock, K_FOREVER) != 0)
    {
        return OS_SEM_FAILURE;
    }

    /* Bump the flush generation so any concurrent Take() sees it changed
     * and returns without consuming the semaphore, then wake everyone. */
    ++impl->flush_request;
    k_condvar_broadcast(&impl->changed);

    k_mutex_unlock(&impl->lock);

    return OS_SUCCESS;
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

    impl = OS_OBJECT_TABLE_GET(OS_impl_bin_sem_table, *token);

    if (k_mutex_lock(&impl->lock, K_FOREVER) != 0)
    {
        return OS_SEM_FAILURE;
    }

    return_code = OS_SUCCESS;

    /* A concurrent Flush() is detected by comparing against this snapshot,
     * not by the value alone, so a Give() racing a Flush() cannot be
     * mistaken for the flush releasing this waiter. */
    flush_count = impl->flush_request;

    while (impl->current_value == 0 && impl->flush_request == flush_count)
    {
        if (k_condvar_wait(&impl->changed, &impl->lock, sys_timepoint_timeout(deadline)) == -EAGAIN)
        {
            return_code = OS_SEM_TIMEOUT;
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
    return OS_Zephyr_BinSemTake_Impl(token, sys_timepoint_calc(K_FOREVER));
}

int32 OS_BinSemTimedWait_Impl(const OS_object_token_t *token, uint32 msecs)
{
    return OS_Zephyr_BinSemTake_Impl(token, sys_timepoint_calc(K_MSEC(msecs)));
}

int32 OS_BinSemGetInfo_Impl(const OS_object_token_t *token, OS_bin_sem_prop_t *bin_prop)
{
    OS_impl_binsem_internal_record_t *impl;

    impl = OS_OBJECT_TABLE_GET(OS_impl_bin_sem_table, *token);

    bin_prop->value = impl->current_value;

    return OS_SUCCESS;
}
