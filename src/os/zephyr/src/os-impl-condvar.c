/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>

#include "os-shared-condvar.h"
#include "os-shared-idmap.h"
#include "os-impl-condvar.h"

OS_impl_condvar_internal_record_t OS_impl_condvar_table[OS_MAX_CONDVARS];

int32 OS_CondVarCreate_Impl(const OS_object_token_t *token, uint32 options)
{
    OS_impl_condvar_internal_record_t *impl;

    ARG_UNUSED(options);

    impl = OS_OBJECT_TABLE_GET(OS_impl_condvar_table, *token);

    if (k_mutex_init(&impl->lock) != 0 || k_condvar_init(&impl->changed) != 0)
    {
        return OS_ERROR;
    }

    return OS_SUCCESS;
}

int32 OS_CondVarDelete_Impl(const OS_object_token_t *token)
{
    ARG_UNUSED(token);

    /* Zephyr condition variables and mutexes have no destroy operation.
     * Reinitializing this slot is safe only after all users have stopped. */
    return OS_SUCCESS;
}

int32 OS_CondVarLock_Impl(const OS_object_token_t *token)
{
    OS_impl_condvar_internal_record_t *impl;

    impl = OS_OBJECT_TABLE_GET(OS_impl_condvar_table, *token);

    return k_mutex_lock(&impl->lock, K_FOREVER) == 0 ? OS_SUCCESS : OS_ERROR;
}

int32 OS_CondVarUnlock_Impl(const OS_object_token_t *token)
{
    OS_impl_condvar_internal_record_t *impl;

    impl = OS_OBJECT_TABLE_GET(OS_impl_condvar_table, *token);

    return k_mutex_unlock(&impl->lock) == 0 ? OS_SUCCESS : OS_ERROR;
}

int32 OS_CondVarSignal_Impl(const OS_object_token_t *token)
{
    OS_impl_condvar_internal_record_t *impl;

    impl = OS_OBJECT_TABLE_GET(OS_impl_condvar_table, *token);

    return k_condvar_signal(&impl->changed) < 0 ? OS_ERROR : OS_SUCCESS;
}

int32 OS_CondVarBroadcast_Impl(const OS_object_token_t *token)
{
    OS_impl_condvar_internal_record_t *impl;

    impl = OS_OBJECT_TABLE_GET(OS_impl_condvar_table, *token);

    /* A positive return value is the number of threads awakened. */
    return k_condvar_broadcast(&impl->changed) < 0 ? OS_ERROR : OS_SUCCESS;
}

int32 OS_CondVarWait_Impl(const OS_object_token_t *token)
{
    OS_impl_condvar_internal_record_t *impl;

    impl = OS_OBJECT_TABLE_GET(OS_impl_condvar_table, *token);

    return k_condvar_wait(&impl->changed, &impl->lock, K_FOREVER) == 0 ? OS_SUCCESS : OS_ERROR;
}

int32 OS_CondVarTimedWait_Impl(const OS_object_token_t *token, const OS_time_t *abs_wakeup_time)
{
    ARG_UNUSED(token);
    ARG_UNUSED(abs_wakeup_time);

    /* OSAL specifies an absolute OS_GetLocalTime() deadline. Zephyr's
     * absolute kernel timeouts use uptime, and the clock-change notification
     * and safe waiter-lifetime protocol are not yet implemented. */
    return OS_ERR_NOT_IMPLEMENTED;
}

int32 OS_CondVarGetInfo_Impl(const OS_object_token_t *token, OS_condvar_prop_t *condvar_prop)
{
    ARG_UNUSED(token);
    ARG_UNUSED(condvar_prop);

    return OS_SUCCESS;
}
