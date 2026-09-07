/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <zephyr/kernel.h>

#include "os-shared-condvar.h"
#include "os-shared-idmap.h"
#include "os-impl-condvar.h"

OS_impl_condvar_internal_record_t OS_impl_condvar_table[OS_MAX_CONDVARS];

/* Shared NONE tokens do not pin a slot. Keep all kernel objects permanent
 * and protect lifecycle state separately from the application mutex, so
 * Signal/Broadcast need not acquire application ownership. When both locks
 * are needed, take the application mutex before the state mutex. */
static bool OS_Zephyr_CondVarMatches(const OS_impl_condvar_internal_record_t *impl,
                                   const OS_object_token_t *token)
{
    return impl->active && OS_ObjectIdEqual(impl->object_id, OS_ObjectIdFromToken(token));
}

int32 OS_CondVarCreate_Impl(const OS_object_token_t *token, uint32 options)
{
    OS_impl_condvar_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_condvar_table, *token);

    ARG_UNUSED(options);

    /* Shared allocation serializes first initialization. Delayed calls may
     * still use this storage after Delete, so subsequent Create must not
     * reinitialize any of its kernel objects. */
    if (!impl->initialized)
    {
        if (k_mutex_init(&impl->lock) != 0 || k_mutex_init(&impl->state_lock) != 0 ||
            k_condvar_init(&impl->changed) != 0)
        {
            return OS_ERROR;
        }
        impl->initialized = true;
    }

    if (k_mutex_lock(&impl->lock, K_FOREVER) != 0)
    {
        return OS_ERROR;
    }
    if (k_mutex_lock(&impl->state_lock, K_FOREVER) != 0)
    {
        k_mutex_unlock(&impl->lock);
        return OS_ERROR;
    }
    if (impl->active)
    {
        k_mutex_unlock(&impl->state_lock);
        k_mutex_unlock(&impl->lock);
        return OS_ERROR;
    }

    impl->object_id = OS_ObjectIdFromToken(token);
    impl->depth     = 0;
    impl->waiters   = 0;
    impl->active    = true;
    k_mutex_unlock(&impl->state_lock);
    k_mutex_unlock(&impl->lock);

    return OS_SUCCESS;
}

int32 OS_CondVarDelete_Impl(const OS_object_token_t *token)
{
    OS_impl_condvar_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_condvar_table, *token);
    int32                             status;

    /* The shared table is locked here. Never wait for application ownership
     * or a notifier holding state_lock. A failed delete restores the ID. */
    if (k_mutex_lock(&impl->lock, K_NO_WAIT) != 0)
    {
        return OS_ERROR;
    }
    if (k_mutex_lock(&impl->state_lock, K_NO_WAIT) != 0)
    {
        k_mutex_unlock(&impl->lock);
        return OS_ERROR;
    }
    if (!OS_Zephyr_CondVarMatches(impl, token))
    {
        status = OS_ERR_INVALID_ID;
    }
    else if (impl->depth != 0 || impl->waiters != 0)
    {
        status = OS_ERROR;
    }
    else
    {
        impl->active = false;
        status       = OS_SUCCESS;
    }
    k_mutex_unlock(&impl->state_lock);
    k_mutex_unlock(&impl->lock);

    return status;
}

int32 OS_CondVarLock_Impl(const OS_object_token_t *token)
{
    OS_impl_condvar_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_condvar_table, *token);
    int32                             status;

    if (k_mutex_lock(&impl->lock, K_FOREVER) != 0)
    {
        return OS_ERROR;
    }
    if (k_mutex_lock(&impl->state_lock, K_FOREVER) != 0)
    {
        k_mutex_unlock(&impl->lock);
        return OS_ERROR;
    }
    if (!OS_Zephyr_CondVarMatches(impl, token))
    {
        status = OS_ERR_INVALID_ID;
    }
    else if (impl->depth >= UINT32_MAX - 1U)
    {
        /* Reserve one native recursion level for ownership probes. */
        status = OS_ERROR;
    }
    else
    {
        ++impl->depth;
        status = OS_SUCCESS;
    }
    k_mutex_unlock(&impl->state_lock);
    if (status != OS_SUCCESS)
    {
        k_mutex_unlock(&impl->lock);
    }

    return status;
}

int32 OS_CondVarUnlock_Impl(const OS_object_token_t *token)
{
    OS_impl_condvar_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_condvar_table, *token);
    int32                             status;

    /* Probe ownership without reading native private fields. The recursive
     * acquisition must be released even if the token or depth is invalid. */
    if (k_mutex_lock(&impl->lock, K_NO_WAIT) != 0)
    {
        return OS_ERROR;
    }
    if (k_mutex_lock(&impl->state_lock, K_FOREVER) != 0)
    {
        k_mutex_unlock(&impl->lock);
        return OS_ERROR;
    }
    if (!OS_Zephyr_CondVarMatches(impl, token))
    {
        status = OS_ERR_INVALID_ID;
    }
    else if (impl->depth == 0)
    {
        status = OS_ERROR;
    }
    else
    {
        --impl->depth;
        status = OS_SUCCESS;
    }
    k_mutex_unlock(&impl->state_lock);
    k_mutex_unlock(&impl->lock); /* Release the probe. */
    if (status == OS_SUCCESS)
    {
        k_mutex_unlock(&impl->lock); /* Release one application acquisition. */
    }

    return status;
}

static int32 OS_Zephyr_CondVarNotify(const OS_object_token_t *token, bool broadcast)
{
    OS_impl_condvar_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_condvar_table, *token);
    int32                             status;

    if (k_mutex_lock(&impl->state_lock, K_FOREVER) != 0)
    {
        return OS_ERROR;
    }
    if (!OS_Zephyr_CondVarMatches(impl, token))
    {
        status = OS_ERR_INVALID_ID;
    }
    else
    {
        /* Keep the ID protected through notification. Neither branch needs
         * the application mutex; an awakened waiter may acquire it and then
         * wait for state_lock after this native call reschedules. */
        status = broadcast ? k_condvar_broadcast(&impl->changed) : k_condvar_signal(&impl->changed);
        status = status < 0 ? OS_ERROR : OS_SUCCESS;
    }
    k_mutex_unlock(&impl->state_lock);

    return status;
}

int32 OS_CondVarSignal_Impl(const OS_object_token_t *token)
{
    return OS_Zephyr_CondVarNotify(token, false);
}

int32 OS_CondVarBroadcast_Impl(const OS_object_token_t *token)
{
    return OS_Zephyr_CondVarNotify(token, true);
}

int32 OS_CondVarWait_Impl(const OS_object_token_t *token)
{
    OS_impl_condvar_internal_record_t *impl = OS_OBJECT_TABLE_GET(OS_impl_condvar_table, *token);
    int32                             status;

    if (k_mutex_lock(&impl->lock, K_NO_WAIT) != 0)
    {
        return OS_ERROR;
    }
    if (k_mutex_lock(&impl->state_lock, K_FOREVER) != 0)
    {
        k_mutex_unlock(&impl->lock);
        return OS_ERROR;
    }
    if (!OS_Zephyr_CondVarMatches(impl, token))
    {
        status = OS_ERR_INVALID_ID;
    }
    else if (impl->depth != 1 || impl->waiters == UINT32_MAX)
    {
        /* Native Wait releases the application mutex exactly once. Refuse
         * an unowned or recursively held lock without losing ownership. */
        status = OS_ERROR;
    }
    else
    {
        ++impl->waiters;
        impl->depth = 0;
        status      = OS_SUCCESS;
    }
    k_mutex_unlock(&impl->state_lock);
    k_mutex_unlock(&impl->lock); /* Drop only the probe before native Wait. */
    if (status != OS_SUCCESS)
    {
        return status;
    }

    status = k_condvar_wait(&impl->changed, &impl->lock, K_FOREVER);

    /* Native Wait reacquires the application mutex even on error. The
     * waiter reservation prevents deletion throughout sleep and reacquire.
     * Forced thread termination cannot release this reservation and remains
     * unsupported until ownership-aware task retirement is implemented. */
    k_mutex_lock(&impl->state_lock, K_FOREVER);
    impl->depth = 1;
    --impl->waiters;
    k_mutex_unlock(&impl->state_lock);

    return status == 0 ? OS_SUCCESS : OS_ERROR;
}

int32 OS_CondVarTimedWait_Impl(const OS_object_token_t *token, const OS_time_t *abs_wakeup_time)
{
    ARG_UNUSED(token);
    ARG_UNUSED(abs_wakeup_time);

    /* OSAL specifies an absolute OS_GetLocalTime() deadline. Zephyr's
     * absolute kernel timeouts use uptime, and the clock-change notification
     * protocol is not yet implemented. Preserve application ownership. */
    return OS_ERR_NOT_IMPLEMENTED;
}

int32 OS_CondVarGetInfo_Impl(const OS_object_token_t *token, OS_condvar_prop_t *condvar_prop)
{
    ARG_UNUSED(token);
    ARG_UNUSED(condvar_prop);

    /* The shared GLOBAL token protects all currently reported properties. */
    return OS_SUCCESS;
}
