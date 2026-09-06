/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <limits.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include "os-shared-clock.h"

#if defined(CONFIG_SYS_CLOCK_EXISTS)
K_MUTEX_DEFINE(OS_clock_lock);

/* Until explicitly set, local time has the same boot epoch as uptime. Keep
 * this anchor independent of OSAL initialization so redundant initialization
 * cannot undo an application clock setting. */
static int64             OS_local_reference;
static int64_t           OS_uptime_reference;

static bool OS_Zephyr_UptimeToTime(int64_t uptime_ticks, int64 *time_ticks)
{
    uint64_t seconds;
    uint64_t remainder;
    uint64_t converted;

    if (uptime_ticks < 0)
    {
        return false;
    }

    seconds   = (uint64_t)uptime_ticks / CONFIG_SYS_CLOCK_TICKS_PER_SEC;
    remainder = (uint64_t)uptime_ticks % CONFIG_SYS_CLOCK_TICKS_PER_SEC;

    /* Splitting at whole seconds avoids overflowing an intermediate
     * nanosecond representation long before OS_time_t itself is exhausted. */
    if (seconds > INT64_MAX / OS_TIME_TICKS_PER_SECOND)
    {
        return false;
    }

    converted = seconds * OS_TIME_TICKS_PER_SECOND;
    converted += k_ticks_to_ns_floor64(remainder) / OS_TIME_TICK_RESOLUTION_NS;
    if (converted > INT64_MAX)
    {
        return false;
    }

    *time_ticks = (int64)converted;
    return true;
}

static bool OS_Zephyr_LocalTimeLocked(int64_t uptime_ticks, int64 *local_time)
{
    int64 elapsed;

    if (uptime_ticks < OS_uptime_reference ||
        !OS_Zephyr_UptimeToTime(uptime_ticks - OS_uptime_reference, &elapsed) ||
        OS_local_reference > INT64_MAX - elapsed)
    {
        return false;
    }

    *local_time = OS_local_reference + elapsed;
    return true;
}

int32 OS_GetMonotonicTime_Impl(OS_time_t *time_struct)
{
    int64 time_ticks;

    /* k_uptime_ticks() includes elapsed, unannounced ticks in a tickless
     * kernel. CONFIG_SYS_CLOCK_TICKS_PER_SEC is its logical time scale, not
     * the system timer interrupt rate. */
    if (!OS_Zephyr_UptimeToTime(k_uptime_ticks(), &time_ticks))
    {
        return OS_ERROR;
    }

    time_struct->ticks = time_ticks;

    return OS_SUCCESS;
}

int32 OS_GetLocalTime_Impl(OS_time_t *time_struct)
{
    int64 local_time;
    bool  valid;

    if (k_mutex_lock(&OS_clock_lock, K_FOREVER) != 0)
    {
        return OS_ERROR;
    }

    valid = OS_Zephyr_LocalTimeLocked(k_uptime_ticks(), &local_time);
    k_mutex_unlock(&OS_clock_lock);

    if (!valid)
    {
        return OS_ERROR;
    }

    time_struct->ticks = local_time;

    return OS_SUCCESS;
}

int32 OS_SetLocalTime_Impl(const OS_time_t *time_struct)
{
    if (k_mutex_lock(&OS_clock_lock, K_FOREVER) != 0)
    {
        return OS_ERROR;
    }

    OS_local_reference  = time_struct->ticks;
    OS_uptime_reference = k_uptime_ticks();
    k_mutex_unlock(&OS_clock_lock);

    return OS_SUCCESS;
}
#else
int32 OS_GetMonotonicTime_Impl(OS_time_t *time_struct)
{
    ARG_UNUSED(time_struct);

    return OS_ERR_NOT_IMPLEMENTED;
}

int32 OS_GetLocalTime_Impl(OS_time_t *time_struct)
{
    ARG_UNUSED(time_struct);

    return OS_ERR_NOT_IMPLEMENTED;
}

int32 OS_SetLocalTime_Impl(const OS_time_t *time_struct)
{
    ARG_UNUSED(time_struct);

    return OS_ERR_NOT_IMPLEMENTED;
}
#endif
