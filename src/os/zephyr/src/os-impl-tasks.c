/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include "os-shared-task.h"

osal_id_t OS_TaskGetId_Impl(void)
{
    /* Until OSAL task creation/registration is ported, all callers are
     * external Zephyr threads. The initial thread also has no OSAL ID. */
    return OS_OBJECT_ID_UNDEFINED;
}
