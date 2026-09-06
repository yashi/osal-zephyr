/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>

#include "os-impl-idmap.h"

typedef struct
{
    struct k_mutex   mutex;
    struct k_condvar changed;
    bool             initialized;
} OS_impl_objtype_lock_t;

static OS_impl_objtype_lock_t OS_impl_objtype_lock_table[OS_OBJECT_TYPE_USER];

int32 OS_Zephyr_TableMutex_Init(osal_objtype_t idtype)
{
    OS_impl_objtype_lock_t *impl;

    if (idtype >= OS_OBJECT_TYPE_USER)
    {
        return OS_ERR_INVALID_ID;
    }

    impl = &OS_impl_objtype_lock_table[idtype];
    /* Initialization is serialized by the application, as required by OS_API_Init().
     * Keep kernel objects intact on subsequent quiescent initialization cycles. */
    if (!impl->initialized)
    {
        if (k_mutex_init(&impl->mutex) != 0 || k_condvar_init(&impl->changed) != 0)
        {
            return OS_ERROR;
        }
        impl->initialized = true;
    }

    return OS_SUCCESS;
}

void OS_Lock_Global_Impl(osal_objtype_t idtype)
{
    if (k_mutex_lock(&OS_impl_objtype_lock_table[idtype].mutex, K_FOREVER) != 0)
    {
        k_panic();
    }
}

void OS_Unlock_Global_Impl(osal_objtype_t idtype)
{
    OS_impl_objtype_lock_t *impl = &OS_impl_objtype_lock_table[idtype];

    k_condvar_broadcast(&impl->changed);
    if (k_mutex_unlock(&impl->mutex) != 0)
    {
        k_panic();
    }
}

void OS_WaitForStateChange_Impl(osal_objtype_t idtype, uint32 attempts)
{
    OS_impl_objtype_lock_t *impl = &OS_impl_objtype_lock_table[idtype];
    uint32                 delay_ms;

    /* Shared token ownership holds one level here. Zephyr's condition wait
     * releases that level atomically with waiting and reacquires it on return.
     * A bounded timeout also permits the shared retry limit to make progress
     * when no other thread changes the table. */
    delay_ms = attempts < 10 ? 10 * (attempts + 1) * (attempts + 1) : 1000;
    k_condvar_wait(&impl->changed, &impl->mutex, K_MSEC(delay_ms));
}
