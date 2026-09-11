/*
 * Copyright (c) 2025 Space Cubics
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/fatal.h>
#include <zephyr/kernel.h>

#include "generic_zephyr_bsp_internal.h"
#include "os-impl-tasks.h"

static K_MUTEX_DEFINE(OS_BSP_GenericZephyrMutex);

static FUNC_NORETURN void OS_BSP_Abort(void)
{
    k_panic();

    /* A custom fatal handler may return; shutdown still must not. */
    k_fatal_halt(K_ERR_KERNEL_PANIC);
}

void OS_BSP_Lock_Impl(void)
{
    OS_Zephyr_TaskEnter();
    if (k_mutex_lock(&OS_BSP_GenericZephyrMutex, K_FOREVER) != 0)
    {
        OS_BSP_Abort();
    }
}

void OS_BSP_Unlock_Impl(void)
{
    if (k_mutex_unlock(&OS_BSP_GenericZephyrMutex) != 0)
    {
        OS_BSP_Abort();
    }
    OS_Zephyr_TaskLeave();
}

void OS_BSP_Shutdown_Impl(void)
{
    OS_BSP_Abort();
}
