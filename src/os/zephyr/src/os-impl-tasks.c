/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr/kernel.h>

#include "os-impl-tasks.h"
#include "os-shared-idmap.h"
#include "os-shared-task.h"

typedef enum
{
    OS_ZEPHYR_TASK_FREE,
    OS_ZEPHYR_TASK_ACTIVE,
    OS_ZEPHYR_TASK_RETIRED
} OS_Zephyr_task_state_t;

typedef struct
{
    /* The FIFO linkage and semaphore outlive every native thread generation. */
    void                   *fifo_reserved;
    struct k_sem            reusable;
    struct k_thread         native_thread;
    struct k_thread        *thread;
    k_thread_stack_t       *stack;
    size_t                  stack_extent;
    osal_id_t               object_id;
    OS_Zephyr_task_state_t   state;
} OS_impl_task_internal_record_t;

typedef struct
{
    k_tid_t   thread;
    osal_id_t object_id;
} OS_Zephyr_helper_identity_t;

static OS_impl_task_internal_record_t OS_impl_task_table[OS_MAX_TASKS];
static OS_Zephyr_helper_identity_t    OS_impl_helper_table[OS_MAX_TIMEBASES];
static struct k_spinlock             OS_task_lock;
static bool                          OS_task_initialized;
static bool                          OS_reaper_started;
static struct k_thread               OS_reaper_thread;
K_KERNEL_STACK_DEFINE(OS_reaper_stack, CONFIG_CFS_OSAL_TASK_REAPER_STACK_SIZE);
K_FIFO_DEFINE(OS_retired_tasks);
K_MUTEX_DEFINE(OS_reaper_lock);

/* Caller holds OS_task_lock. Identity remains available through retirement,
 * until join proves that no code belonging to the generation can run. */
static OS_impl_task_internal_record_t *OS_Zephyr_TaskFind(k_tid_t thread)
{
    uint32 i;

    for (i = 0; i < OS_MAX_TASKS; ++i)
    {
        if (OS_impl_task_table[i].thread == thread)
        {
            return &OS_impl_task_table[i];
        }
    }
    return NULL;
}

/* Join is the safety condition: stack-free's live-thread check does not
 * recognize the reserved MPU guard prefix on all Zephyr architectures. */
static void OS_Zephyr_TaskReclaim(OS_impl_task_internal_record_t *impl)
{
    k_spinlock_key_t key;

    if (k_thread_join(impl->thread, K_FOREVER) != 0)
    {
        k_panic();
    }

    key = k_spin_lock(&OS_task_lock);
    /* Keep the stack claim and identity until this generation has joined.
     * Zephyr reinitializes the embedded thread object on its next create;
     * do not clear its native bookkeeping with memset. */
    impl->thread           = NULL;
    impl->stack            = NULL;
    impl->stack_extent      = 0;
    impl->object_id        = OS_OBJECT_ID_UNDEFINED;
    impl->state            = OS_ZEPHYR_TASK_FREE;
    k_spin_unlock(&OS_task_lock, key);
    k_sem_give(&impl->reusable);
}

static void OS_Zephyr_TaskReaper(void *arg1, void *arg2, void *arg3)
{
    OS_impl_task_internal_record_t *impl;

    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
    for (;;)
    {
        impl = k_fifo_get(&OS_retired_tasks, K_FOREVER);
        OS_Zephyr_TaskReclaim(impl);
    }
}

int32 OS_Zephyr_TaskAPI_Impl_Init(void)
{
    uint32 i;

    /* OS_API_Init serializes initialization and requires quiescent callers.
     * Do not reset live native synchronization objects on a later init. */
    if (!OS_task_initialized)
    {
        for (i = 0; i < OS_MAX_TASKS; ++i)
        {
            k_sem_init(&OS_impl_task_table[i].reusable, 1, 1);
        }
        OS_task_initialized = true;
    }
    return OS_SUCCESS;
}

static int OS_Zephyr_TaskPriority(osal_priority_t priority)
{
    /* Map both endpoints and preserve ordering. Several OSAL priorities may
     * share a native priority; cooperative and idle priorities are excluded. */
    return ((uint32)priority * (CONFIG_NUM_PREEMPT_PRIORITIES - 1)) / OS_MAX_TASK_PRIORITY;
}

static void OS_Zephyr_TaskEntry(void *arg1, void *arg2, void *arg3)
{
    OS_impl_task_internal_record_t *impl = arg1;

    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
    OS_TaskEntryPoint(impl->object_id);
}

/* Require the supplied size to describe the complete usable part of a
 * K_KERNEL_STACK_DEFINE object, as returned by K_KERNEL_STACK_SIZEOF.
 * Check every addition used by native stack rounding before evaluating it.
 * Zephyr has no supervisor-mode object registry that can prove a pointer's
 * declaration or actual capacity; those remain the caller's responsibility. */
static int32 OS_Zephyr_TaskStackGeometry(const OS_task_internal_record_t *task, size_t *extent)
{
    size_t rounded;

    if (task->stack_size == 0 || task->stack_size > SIZE_MAX - (ARCH_STACK_PTR_ALIGN - 1))
    {
        return OS_ERR_INVALID_SIZE;
    }
    rounded = ROUND_UP(task->stack_size, ARCH_STACK_PTR_ALIGN);
    if (rounded > SIZE_MAX - K_KERNEL_STACK_RESERVED)
    {
        return OS_ERR_INVALID_SIZE;
    }
    rounded += K_KERNEL_STACK_RESERVED;
    if (rounded > SIZE_MAX - (Z_KERNEL_STACK_OBJ_ALIGN - 1))
    {
        return OS_ERR_INVALID_SIZE;
    }
    *extent = K_KERNEL_STACK_LEN(task->stack_size);

    if (task->stack_pointer != NULL)
    {
        if (task->stack_size != *extent - K_KERNEL_STACK_RESERVED)
        {
            return OS_ERR_INVALID_SIZE;
        }
        if ((uintptr_t)task->stack_pointer % Z_KERNEL_STACK_OBJ_ALIGN != 0 ||
            (uintptr_t)task->stack_pointer > UINTPTR_MAX - *extent)
        {
            return OS_INVALID_POINTER;
        }
    }
    return OS_SUCCESS;
}

/* Caller holds OS_task_lock. Claims include native guard/padding bytes and
 * cover creating, active, closing and retired generations in every slot. */
static bool OS_Zephyr_TaskStackAvailable(k_thread_stack_t *stack, size_t extent)
{
    uintptr_t start = (uintptr_t)stack;
    uint32    i;

    for (i = 0; i < OS_MAX_TASKS; ++i)
    {
        if (OS_impl_task_table[i].stack != NULL &&
            start < (uintptr_t)OS_impl_task_table[i].stack + OS_impl_task_table[i].stack_extent &&
            (uintptr_t)OS_impl_task_table[i].stack < start + extent)
        {
            return false;
        }
    }
    return true;
}

int32 OS_TaskCreate_Impl(const OS_object_token_t *token, uint32 flags)
{
    OS_impl_task_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_task_table, *token);
    OS_task_internal_record_t      *task = OS_OBJECT_TABLE_GET(OS_task_table, *token);
    struct k_thread                *thread;
    k_thread_stack_t               *stack;
    k_spinlock_key_t                key;
    size_t                          extent;
    int32                           status;
    bool                            available;
    uint32                          options = 0;

    /* User-mode object permissions and ownership are outside this provider. */
    if (IS_ENABLED(CONFIG_USERSPACE) || (flags & ~OS_FP_ENABLED) != 0 ||
        task->stack_pointer == NULL)
    {
        return OS_ERR_NOT_IMPLEMENTED;
    }
    if ((flags & OS_FP_ENABLED) != 0)
    {
        if (!IS_ENABLED(CONFIG_FPU) || !IS_ENABLED(CONFIG_FPU_SHARING))
        {
            return OS_ERR_NOT_IMPLEMENTED;
        }
        options |= K_FP_REGS;
        if (IS_ENABLED(CONFIG_X86_SSE))
        {
            options |= K_SSE_REGS;
        }
    }

    status = OS_Zephyr_TaskStackGeometry(task, &extent);
    if (status != OS_SUCCESS)
    {
        return status;
    }
    stack = task->stack_pointer;
    if (stack != NULL)
    {
        /* Reject an occupied stack promptly, including a retired generation
         * in this same slot. Waiting for that owner may depend on the caller
         * returning from create. Recheck when reserving after the slot wait. */
        key       = k_spin_lock(&OS_task_lock);
        available = OS_Zephyr_TaskStackAvailable(stack, extent);
        k_spin_unlock(&OS_task_lock, key);
        if (!available)
        {
            return OS_ERROR;
        }
    }

    /* Shared create reserves the slot. A previous self-exiting generation
     * may have freed its public ID but still be on its stack. Reclamation
     * never acquires shared table locks. */
    k_sem_take(&impl->reusable, K_FOREVER);
    if (stack == NULL)
    {
        k_sem_give(&impl->reusable);
        return OS_ERROR;
    }

    /* Reserve before native creation can touch the stack. Different shared
     * slot reservations alone cannot serialize claims on the same storage. */
    key       = k_spin_lock(&OS_task_lock);
    available = OS_Zephyr_TaskStackAvailable(stack, extent);
    if (available)
    {
        impl->stack        = stack;
        impl->stack_extent = extent;
    }
    k_spin_unlock(&OS_task_lock, key);
    if (!available)
    {
        k_sem_give(&impl->reusable);
        return OS_ERROR;
    }

    /* Start the dedicated reaper on first successful reservation only. It
     * never invokes OSAL or acquires the task table mutex. Different slots
     * may be created concurrently after their shared ID reservations. */
    k_mutex_lock(&OS_reaper_lock, K_FOREVER);
    if (!OS_reaper_started)
    {
        k_thread_create(&OS_reaper_thread, OS_reaper_stack, K_KERNEL_STACK_SIZEOF(OS_reaper_stack),
                        OS_Zephyr_TaskReaper, NULL, NULL, NULL, 0, 0, K_NO_WAIT);
        OS_reaper_started = true;
    }
    k_mutex_unlock(&OS_reaper_lock);

    thread = &impl->native_thread;
    k_thread_create(thread, stack, task->stack_size, OS_Zephyr_TaskEntry, impl, NULL, NULL,
                    OS_Zephyr_TaskPriority(task->priority), options, K_FOREVER);
    key                  = k_spin_lock(&OS_task_lock);
    impl->thread         = thread;
    impl->object_id      = OS_ObjectIdFromToken(token);
    impl->state          = OS_ZEPHYR_TASK_ACTIVE;
    k_spin_unlock(&OS_task_lock, key);

    /* The complete provisional identity precedes all execution, including a
     * failure in shared prepare. Entry waits for shared create finalization. */
    k_thread_start(thread);
    return OS_SUCCESS;
}

int32 OS_TaskDelete_Impl(const OS_object_token_t *token)
{
    ARG_UNUSED(token);
    return OS_ERR_NOT_IMPLEMENTED;
}

int32 OS_TaskDetach_Impl(const OS_object_token_t *token)
{
    OS_impl_task_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_task_table, *token);
    k_spinlock_key_t                key;
    int32                           status = OS_ERROR;

    key = k_spin_lock(&OS_task_lock);
    if (impl->state == OS_ZEPHYR_TASK_ACTIVE && impl->thread == k_current_get() &&
        OS_ObjectIdEqual(impl->object_id, OS_ObjectIdFromToken(token)))
    {
        impl->state = OS_ZEPHYR_TASK_RETIRED;
        status      = OS_SUCCESS;
    }
    k_spin_unlock(&OS_task_lock, key);
    return status;
}

void OS_TaskExit_Impl(void)
{
    OS_impl_task_internal_record_t *impl;
    k_spinlock_key_t                key;
    OS_object_token_t               token;
    osal_id_t                       object_id;
    int32                           status;
    bool                            retired;

    key       = k_spin_lock(&OS_task_lock);
    impl      = OS_Zephyr_TaskFind(k_current_get());
    retired   = impl == NULL || impl->state == OS_ZEPHYR_TASK_RETIRED;
    object_id = impl != NULL ? impl->object_id : OS_OBJECT_ID_UNDEFINED;
    k_spin_unlock(&OS_task_lock, key);
    if (!retired)
    {
        /* Shared exit normally detaches first, but its GLOBAL lookup is
         * forbidden during shutdown and can time out behind a reserved ID.
         * Complete self-retirement with an EXCLUSIVE transaction, which is
         * permitted during shutdown. */
        do
        {
            status = OS_ObjectIdGetById(OS_LOCK_MODE_EXCLUSIVE, OS_OBJECT_TYPE_OS_TASK, object_id, &token);
            if (status == OS_ERR_OBJECT_IN_USE)
            {
                /* Wait for the reserved transaction to release the ID.
                 * Never retire a still-published ID. */
                k_sleep(K_MSEC(1));
            }
        } while (status == OS_ERR_OBJECT_IN_USE);
        if (status == OS_SUCCESS)
        {
            status = OS_TaskDetach_Impl(&token);
            status = OS_ObjectIdFinalizeDelete(status, &token);
        }
        if (status != OS_SUCCESS)
        {
            k_panic();
        }
    }
    if (impl != NULL)
    {
        k_fifo_put(&OS_retired_tasks, impl);
    }
    OS_Zephyr_TaskUnregister();
    k_thread_abort(k_current_get());
    CODE_UNREACHABLE;
}

int32 OS_TaskDelay_Impl(uint32 millisecond)
{
    /* Delay holds no OSAL resource and is safe to abort. */
    if (millisecond != 0 && !IS_ENABLED(CONFIG_SYS_CLOCK_EXISTS))
    {
        return OS_ERR_NOT_IMPLEMENTED;
    }
    if (!IS_ENABLED(CONFIG_TIMEOUT_64BIT) && k_ms_to_ticks_ceil64(millisecond) > INT32_MAX)
    {
        return OS_ERR_NOT_IMPLEMENTED;
    }
    if (millisecond == 0)
    {
        k_yield();
    }
    else
    {
        k_sleep(K_MSEC(millisecond));
    }
    return OS_SUCCESS;
}

int32 OS_TaskSetPriority_Impl(const OS_object_token_t *token, osal_priority_t new_priority)
{
    OS_impl_task_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_task_table, *token);

    k_thread_priority_set(impl->thread, OS_Zephyr_TaskPriority(new_priority));
    return OS_SUCCESS;
}

int32 OS_TaskMatch_Impl(const OS_object_token_t *token)
{
    OS_impl_task_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_task_table, *token);
    k_spinlock_key_t                key;
    bool                            match;

    key   = k_spin_lock(&OS_task_lock);
    match = impl->thread == k_current_get() && OS_ObjectIdEqual(impl->object_id, OS_ObjectIdFromToken(token));
    k_spin_unlock(&OS_task_lock, key);
    return match ? OS_SUCCESS : OS_ERROR;
}

int32 OS_TaskRegister_Impl(osal_id_t global_task_id)
{
    OS_impl_task_internal_record_t *impl;
    OS_Zephyr_helper_identity_t    *available = NULL;
    k_spinlock_key_t                key;
    uint32                          i;
    int32                           status = OS_ERROR;

    key  = k_spin_lock(&OS_task_lock);
    impl = OS_Zephyr_TaskFind(k_current_get());
    if (impl != NULL)
    {
        status = OS_ObjectIdEqual(impl->object_id, global_task_id) ? OS_SUCCESS : OS_ERROR;
    }
    else if (OS_ObjectIdToType_Impl(global_task_id) == OS_OBJECT_TYPE_OS_TIMEBASE)
    {
        for (i = 0; i < OS_MAX_TIMEBASES; ++i)
        {
            if (OS_impl_helper_table[i].thread == k_current_get())
            {
                status = OS_ObjectIdEqual(OS_impl_helper_table[i].object_id, global_task_id) ? OS_SUCCESS : OS_ERROR;
                k_spin_unlock(&OS_task_lock, key);
                return status;
            }
            if (OS_impl_helper_table[i].thread == NULL)
            {
                available = &OS_impl_helper_table[i];
            }
        }
        if (available != NULL)
        {
            available->thread    = k_current_get();
            available->object_id = global_task_id;
            status               = OS_SUCCESS;
        }
    }
    k_spin_unlock(&OS_task_lock, key);
    return status;
}

void OS_Zephyr_TaskUnregister(void)
{
    k_spinlock_key_t key;
    uint32          i;

    key = k_spin_lock(&OS_task_lock);
    for (i = 0; i < OS_MAX_TIMEBASES; ++i)
    {
        if (OS_impl_helper_table[i].thread == k_current_get())
        {
            OS_impl_helper_table[i].thread    = NULL;
            OS_impl_helper_table[i].object_id = OS_OBJECT_ID_UNDEFINED;
        }
    }
    k_spin_unlock(&OS_task_lock, key);
}

osal_id_t OS_TaskGetId_Impl(void)
{
    OS_impl_task_internal_record_t *impl;
    k_spinlock_key_t                key;
    osal_id_t                       object_id = OS_OBJECT_ID_UNDEFINED;
    uint32                          i;

    key  = k_spin_lock(&OS_task_lock);
    impl = OS_Zephyr_TaskFind(k_current_get());
    if (impl != NULL)
    {
        object_id = impl->object_id;
    }
    else
    {
        for (i = 0; i < OS_MAX_TIMEBASES; ++i)
        {
            if (OS_impl_helper_table[i].thread == k_current_get())
            {
                object_id = OS_impl_helper_table[i].object_id;
                break;
            }
        }
    }
    k_spin_unlock(&OS_task_lock, key);
    return object_id;
}

int32 OS_TaskGetInfo_Impl(const OS_object_token_t *token, OS_task_prop_t *task_prop)
{
    /* All portable properties are supplied by the shared layer. */
    ARG_UNUSED(token);
    ARG_UNUSED(task_prop);
    return OS_SUCCESS;
}

int32 OS_TaskValidateSystemData_Impl(const void *sysdata, size_t sysdata_size)
{
    return sysdata != NULL && sysdata_size == sizeof(k_tid_t) ? OS_SUCCESS : OS_INVALID_POINTER;
}

bool OS_TaskIdMatchSystemData_Impl(void *ref, const OS_object_token_t *token, const OS_common_record_t *obj)
{
    OS_impl_task_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_task_table, *token);
    k_tid_t                         target;
    k_spinlock_key_t                key;
    bool                            match;

    /* Public sysdata may be byte-aligned. Never dereference it as k_tid_t*. */
    memcpy(&target, ref, sizeof(target));
    key   = k_spin_lock(&OS_task_lock);
    match = target == impl->thread && OS_ObjectIdEqual(impl->object_id, obj->active_id);
    k_spin_unlock(&OS_task_lock, key);
    return match;
}
