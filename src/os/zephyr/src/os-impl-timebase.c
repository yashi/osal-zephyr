/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <limits.h>
#include <stdint.h>
#include <zephyr/kernel.h>

#include "os-impl-tasks.h"
#include "os-impl-timebase.h"
#include "os-shared-idmap.h"
#include "os-shared-task.h"
#include "os-shared-timebase.h"

#if defined(CONFIG_SYS_CLOCK_EXISTS)

BUILD_ASSERT(CONFIG_CFS_OSAL_TIMEBASE_PRIORITY < CONFIG_NUM_PREEMPT_PRIORITIES);

typedef struct
{
    struct k_mutex    lock;
    struct k_sem      wake;
    struct k_sem      ready;
    struct k_timer    timer;
    struct k_thread   thread;
    struct k_spinlock ticks_lock;
    osal_id_t         id;
    OS_TimerSync_t    external_sync;
    uint64_t          pending;
    uint64_t          fraction;
    uint64_t          first_ticks;
    uint64_t          interval_ticks;
    uint32            delivered;
    uint32            before_delivery;
    int32             registration;
    bool              initialized;
    bool              active;
    bool              closing;
    bool              external_busy;
    bool              handoff;
    bool              running;
    bool              first;
} OS_Zephyr_timebase_t;

static OS_Zephyr_timebase_t OS_timebases[OS_MAX_TIMEBASES];
K_KERNEL_STACK_ARRAY_DEFINE(OS_timebase_stacks, OS_MAX_TIMEBASES, CONFIG_CFS_OSAL_TIMEBASE_STACK_SIZE);

/* Stop/reconfigure must quiesce the ISR before resetting its state.
 * k_timer_stop() alone does not wait for an in-flight expiry callback. */
static void OS_Zephyr_TimeBaseExpiry(struct k_timer *timer)
{
    OS_Zephyr_timebase_t *impl = CONTAINER_OF(timer, OS_Zephyr_timebase_t, timer);
    k_spinlock_key_t      key;
    uint64_t              numerator;

    key = k_spin_lock(&impl->ticks_lock);
    if (impl->running)
    {
        numerator = (impl->first ? impl->first_ticks : impl->interval_ticks) * 1000000 + impl->fraction;
        impl->pending += numerator / CONFIG_SYS_CLOCK_TICKS_PER_SEC;
        impl->fraction = numerator % CONFIG_SYS_CLOCK_TICKS_PER_SEC;
        impl->first = false;
        if (impl->interval_ticks == 0)
        {
            impl->running = false;
        }
        k_sem_give(&impl->wake);
    }
    k_spin_unlock(&impl->ticks_lock, key);
}

static uint32 OS_Zephyr_TimeBaseSync(osal_id_t id)
{
    OS_Zephyr_timebase_t *impl = NULL;
    k_spinlock_key_t      key;
    uint32                i;
    uint32                elapsed;

    /* The helper owns its permanent slot through native join. Public ID
     * lookup would fail here when deletion reserves the ID before waking us. */
    for (i = 0; i < OS_MAX_TIMEBASES; ++i)
    {
        if (k_current_get() == &OS_timebases[i].thread)
        {
            impl = &OS_timebases[i];
            break;
        }
    }
    __ASSERT_NO_MSG(impl != NULL && OS_ObjectIdEqual(impl->id, id));

    for (;;)
    {
        k_mutex_lock(&impl->lock, K_FOREVER);
        if (impl->closing)
        {
            elapsed = 0;
            break;
        }

        key = k_spin_lock(&impl->ticks_lock);
        /* The shared callback wait times are signed. A long backlog is
         * delivered in bounded pieces instead of wrapping subtraction. */
        elapsed = impl->pending > 999999999 ? 999999999 : (uint32)impl->pending;
        impl->pending -= elapsed;
        k_spin_unlock(&impl->ticks_lock, key);
        if (elapsed != 0)
        {
            break;
        }

        if (impl->external_sync != NULL)
        {
            impl->external_busy = true;
            k_mutex_unlock(&impl->lock);
            elapsed = impl->external_sync(id);
            k_mutex_lock(&impl->lock, K_FOREVER);
            impl->external_busy = false;
            key = k_spin_lock(&impl->ticks_lock);
            impl->pending += elapsed;
            k_spin_unlock(&impl->ticks_lock, key);
            if (elapsed == 0)
            {
                /* Let the shared helper apply its zero-tick spin limiter. */
                break;
            }
            k_mutex_unlock(&impl->lock);
        }
        else
        {
            k_mutex_unlock(&impl->lock);
            k_sem_take(&impl->wake, K_FOREVER);
        }
    }

    /* Hand the mutex to the shared loop. Set cannot replace the schedule
     * between returning a tick and processing it under TimeBaseLock_Impl. */
    impl->handoff = true;
    impl->delivered = elapsed;
    impl->before_delivery = OS_timebase_table[i].freerun_time;
    return elapsed;
}

static void OS_Zephyr_TimeBaseEntry(void *arg, void *unused2, void *unused3)
{
    OS_Zephyr_timebase_t *impl = arg;
    bool                  closing;

    ARG_UNUSED(unused2);
    ARG_UNUSED(unused3);
    impl->registration = OS_TaskRegister_Impl(impl->id);
    k_sem_give(&impl->ready);
    if (impl->registration == OS_SUCCESS)
    {
        for (;;)
        {
            k_mutex_lock(&impl->lock, K_FOREVER);
            closing = impl->closing;
            k_mutex_unlock(&impl->lock);
            if (closing)
            {
                break;
            }
            OS_TimeBase_CallbackThread(impl->id);
            /* EXCLUSIVE temporarily replaces active_id even when deletion
             * fails (e.g. an attached timer retains a reference). Restart
             * after rollback; Unlock restores an unprocessed tick. */
            k_mutex_lock(&impl->lock, K_FOREVER);
            closing = impl->closing;
            k_mutex_unlock(&impl->lock);
            if (closing)
            {
                break;
            }
            k_sleep(K_MSEC(1));
        }
    }
    OS_Zephyr_TaskUnregister();
}

int32 OS_Zephyr_TimeBaseAPI_Impl_Init(void)
{
    uint32 i;
    for (i = 0; i < OS_MAX_TIMEBASES; ++i)
    {
        if (!OS_timebases[i].initialized)
        {
            k_mutex_init(&OS_timebases[i].lock);
            k_sem_init(&OS_timebases[i].wake, 0, 1);
            k_sem_init(&OS_timebases[i].ready, 0, 1);
            k_timer_init(&OS_timebases[i].timer, OS_Zephyr_TimeBaseExpiry, NULL);
            OS_timebases[i].initialized = true;
        }
    }
    return OS_SUCCESS;
}

int32 OS_TimeBaseCreate_Impl(const OS_object_token_t *token)
{
    OS_Zephyr_timebase_t          *impl = OS_OBJECT_TABLE_GET(OS_timebases, *token);
    OS_timebase_internal_record_t *shared = OS_OBJECT_TABLE_GET(OS_timebase_table, *token);
    size_t                         index = impl - OS_timebases;

    OS_Zephyr_TaskEnter();
    if (IS_ENABLED(CONFIG_USERSPACE))
    {
        return OS_Zephyr_TaskLeaveResult(OS_ERR_NOT_IMPLEMENTED);
    }
    k_mutex_lock(&impl->lock, K_FOREVER);
    if (impl->active)
    {
        k_mutex_unlock(&impl->lock);
        return OS_Zephyr_TaskLeaveResult(OS_TIMER_ERR_INTERNAL);
    }
    impl->id = OS_ObjectIdFromToken(token);
    impl->external_sync = shared->external_sync;
    impl->pending = 0;
    impl->fraction = 0;
    impl->closing = false;
    impl->external_busy = false;
    impl->handoff = false;
    impl->running = false;
    impl->active = true;
    k_sem_reset(&impl->wake);
    k_sem_reset(&impl->ready);
    shared->external_sync = OS_Zephyr_TimeBaseSync;
    if (impl->external_sync == NULL)
    {
        shared->accuracy_usec = DIV_ROUND_UP(1000000, CONFIG_SYS_CLOCK_TICKS_PER_SEC);
    }
    k_thread_create(&impl->thread, OS_timebase_stacks[index], K_KERNEL_STACK_SIZEOF(OS_timebase_stacks[index]),
                    OS_Zephyr_TimeBaseEntry, impl, NULL, NULL, CONFIG_CFS_OSAL_TIMEBASE_PRIORITY, 0, K_NO_WAIT);
    k_sem_take(&impl->ready, K_FOREVER);
    if (impl->registration != OS_SUCCESS)
    {
        if (k_thread_join(&impl->thread, K_FOREVER) != 0)
        {
            k_panic();
        }
        impl->active = false;
        shared->external_sync = impl->external_sync;
        k_mutex_unlock(&impl->lock);
        return OS_Zephyr_TaskLeaveResult(OS_TIMER_ERR_INTERNAL);
    }
    k_mutex_unlock(&impl->lock);
    return OS_Zephyr_TaskLeaveResult(OS_SUCCESS);
}

void OS_TimeBaseLock_Impl(const OS_object_token_t *token)
{
    OS_Zephyr_timebase_t *impl = OS_OBJECT_TABLE_GET(OS_timebases, *token);
    OS_Zephyr_TaskEnter();
    if (k_current_get() != &impl->thread || !impl->handoff)
    {
        k_mutex_lock(&impl->lock, K_FOREVER);
    }
}

void OS_TimeBaseUnlock_Impl(const OS_object_token_t *token)
{
    OS_Zephyr_timebase_t          *impl = OS_OBJECT_TABLE_GET(OS_timebases, *token);
    OS_timebase_internal_record_t *shared = OS_OBJECT_TABLE_GET(OS_timebase_table, *token);
    k_spinlock_key_t               key;

    if (k_current_get() == &impl->thread && impl->handoff)
    {
        /* A reserved ID makes the shared loop exit before adding the tick.
         * Preserve it for rollback. Unsigned subtraction handles freerun wrap. */
        if ((uint32)(shared->freerun_time - impl->before_delivery) == 0)
        {
            key = k_spin_lock(&impl->ticks_lock);
            impl->pending += impl->delivered;
            k_spin_unlock(&impl->ticks_lock, key);
        }
        impl->handoff = false;
    }
    k_mutex_unlock(&impl->lock);
    OS_Zephyr_TaskLeave();
}

int32 OS_TimeBaseSet_Impl(const OS_object_token_t *token, uint32 start_time, uint32 interval_time)
{
    OS_Zephyr_timebase_t *impl = OS_OBJECT_TABLE_GET(OS_timebases, *token);
    uint64_t              first = k_us_to_ticks_ceil64(start_time);
    uint64_t              interval = k_us_to_ticks_ceil64(interval_time);
    k_spinlock_key_t      key;

    /* Shared Set already owns the handler lock and the public token. */
    if (impl->external_sync != NULL)
    {
        return OS_SUCCESS;
    }
    if (!IS_ENABLED(CONFIG_TIMEOUT_64BIT) && (first > INT32_MAX || interval > INT32_MAX))
    {
        return OS_ERR_NOT_IMPLEMENTED;
    }
    if (k_timer_cleanup(&impl->timer) != 0)
    {
        return OS_TIMER_ERR_INTERNAL;
    }
    key = k_spin_lock(&impl->ticks_lock);
    impl->pending = 0;
    impl->fraction = 0;
    impl->first_ticks = first;
    impl->interval_ticks = interval;
    impl->first = true;
    impl->running = start_time != 0;
    k_spin_unlock(&impl->ticks_lock, key);
    k_sem_reset(&impl->wake);
    if (start_time != 0)
    {
        k_timer_start(&impl->timer, K_TICKS(first), K_TICKS(interval));
    }
    return OS_SUCCESS;
}

int32 OS_TimeBaseDelete_Impl(const OS_object_token_t *token)
{
    OS_Zephyr_timebase_t *impl = OS_OBJECT_TABLE_GET(OS_timebases, *token);
    OS_Zephyr_TaskEnter();
    /* Successful shared EXCLUSIVE admission excludes attached callbacks:
     * each owns a timebase reference. Wait for the short internal handoff
     * so dedicated OS_TimerDelete cannot lose its timebase to a busy error. */
    if (k_mutex_lock(&impl->lock, K_FOREVER) != 0)
    {
        return OS_Zephyr_TaskLeaveResult(OS_ERROR);
    }
    if (impl->external_busy)
    {
        /* Do not abort arbitrary BSP code or wait forever for its event. */
        k_mutex_unlock(&impl->lock);
        return OS_Zephyr_TaskLeaveResult(OS_ERROR);
    }
    if (k_timer_cleanup(&impl->timer) != 0)
    {
        k_mutex_unlock(&impl->lock);
        return OS_Zephyr_TaskLeaveResult(OS_TIMER_ERR_INTERNAL);
    }
    impl->closing = true;
    k_sem_give(&impl->wake);
    k_mutex_unlock(&impl->lock);
    /* EXCLUSIVE has already reserved active_id. Internal sync wakes the
     * shared loop to observe it and exit with every lock released. */
    if (k_thread_join(&impl->thread, K_FOREVER) != 0)
    {
        k_panic();
    }
    impl->active = false;
    return OS_Zephyr_TaskLeaveResult(OS_SUCCESS);
}

int32 OS_TimeBaseGetInfo_Impl(const OS_object_token_t *token, OS_timebase_prop_t *prop)
{
    ARG_UNUSED(token);
    ARG_UNUSED(prop);
    return OS_SUCCESS;
}

#else

int32 OS_Zephyr_TimeBaseAPI_Impl_Init(void)
{
    return OS_SUCCESS;
}
int32 OS_TimeBaseCreate_Impl(const OS_object_token_t *token)
{
    ARG_UNUSED(token);
    return OS_ERR_NOT_IMPLEMENTED;
}
int32 OS_TimeBaseSet_Impl(const OS_object_token_t *token, uint32 start, uint32 interval)
{
    ARG_UNUSED(token);
    ARG_UNUSED(start);
    ARG_UNUSED(interval);
    return OS_ERR_NOT_IMPLEMENTED;
}
int32 OS_TimeBaseDelete_Impl(const OS_object_token_t *token)
{
    ARG_UNUSED(token);
    return OS_ERR_NOT_IMPLEMENTED;
}
void OS_TimeBaseLock_Impl(const OS_object_token_t *token)
{
    ARG_UNUSED(token);
}
void OS_TimeBaseUnlock_Impl(const OS_object_token_t *token)
{
    ARG_UNUSED(token);
}
int32 OS_TimeBaseGetInfo_Impl(const OS_object_token_t *token, OS_timebase_prop_t *prop)
{
    ARG_UNUSED(token);
    ARG_UNUSED(prop);
    return OS_ERR_NOT_IMPLEMENTED;
}
#endif
