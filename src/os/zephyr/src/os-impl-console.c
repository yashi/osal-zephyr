/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#include "os-shared-idmap.h"
#include "os-shared-printf.h"

int32 OS_ConsoleCreate_Impl(const OS_object_token_t *token)
{
    OS_console_internal_record_t *console = OS_OBJECT_TABLE_GET(OS_console_table, *token);

    /* The portable BSP output provider drains this buffer synchronously.
     * A helper task and its lifecycle are required before async is supported. */
    if (console->IsAsync)
    {
        return OS_ERR_NOT_IMPLEMENTED;
    }

    return OS_SUCCESS;
}

void OS_ConsoleWakeup_Impl(const OS_object_token_t *token)
{
    /* No helper can exist: creation rejects an asynchronous console. */
    (void)token;
}
