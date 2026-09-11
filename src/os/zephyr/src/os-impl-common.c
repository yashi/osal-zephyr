/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>

#include "os-shared-common.h"
#include "os-impl-idmap.h"
#include "os-impl-tasks.h"

K_SEM_DEFINE(OS_shutdown_sem, 0, 1);

int32 OS_API_Impl_Init(osal_objtype_t idtype)
{
    switch (idtype)
    {
        case OS_OBJECT_TYPE_UNDEFINED:
            /* The legacy OSAL tick accessors represent whole microseconds.
             * Other clock APIs are separate providers, not implemented here. */
#if CONFIG_SYS_CLOCK_TICKS_PER_SEC <= 0 || CONFIG_SYS_CLOCK_TICKS_PER_SEC > 1000000
            return OS_ERROR;
#else
            OS_SharedGlobalVars.TicksPerSecond  = CONFIG_SYS_CLOCK_TICKS_PER_SEC;
            OS_SharedGlobalVars.MicroSecPerTick = 1000000 / CONFIG_SYS_CLOCK_TICKS_PER_SEC;
            k_sem_reset(&OS_shutdown_sem);
#endif
            break;

        case OS_OBJECT_TYPE_OS_TASK:
            if (OS_Zephyr_TaskAPI_Impl_Init() != OS_SUCCESS)
            {
                return OS_ERROR;
            }
            break;

        case OS_OBJECT_TYPE_OS_QUEUE:
        case OS_OBJECT_TYPE_OS_COUNTSEM:
        case OS_OBJECT_TYPE_OS_BINSEM:
        case OS_OBJECT_TYPE_OS_MUTEX:
        case OS_OBJECT_TYPE_OS_RWLOCK:
        case OS_OBJECT_TYPE_OS_STREAM:
        case OS_OBJECT_TYPE_OS_DIR:
        case OS_OBJECT_TYPE_OS_TIMEBASE:
        case OS_OBJECT_TYPE_OS_TIMECB:
        case OS_OBJECT_TYPE_OS_MODULE:
        case OS_OBJECT_TYPE_OS_FILESYS:
        case OS_OBJECT_TYPE_OS_CONSOLE:
        case OS_OBJECT_TYPE_OS_CONDVAR:
            /* No per-slot kernel objects exist until creation. The shared
             * initializer clears its records after this table lock is ready.
             * This does not provide the currently unported object operations. */
            break;

        default:
            /* OS_API_Init also visits reserved values below USER. These
             * have no shared records or backend resources to initialize. */
            if (idtype >= OS_OBJECT_TYPE_USER || OS_GetMaxForObjectType(idtype) != 0)
            {
                return OS_ERR_NOT_IMPLEMENTED;
            }
            break;
    }

    return OS_Zephyr_TableMutex_Init(idtype);
}

void OS_IdleLoop_Impl(void)
{
    /* A latched notification also covers shutdown just before this wait. */
    k_sem_take(&OS_shutdown_sem, K_FOREVER);
}

void OS_ApplicationShutdown_Impl(void)
{
    k_sem_give(&OS_shutdown_sem);
}
