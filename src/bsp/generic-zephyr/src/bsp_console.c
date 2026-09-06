/*
 * Copyright (c) 2026 Space Cubics
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>

#include "generic_zephyr_bsp_internal.h"

void OS_BSP_ConsoleOutput_Impl(const char *Str, size_t DataLen)
{
    /* k_str_out() takes a mutable pointer but only reads the buffer. */
    k_str_out((char *)Str, DataLen);
}

void OS_BSP_ConsoleSetMode_Impl(uint32 ModeBits)
{
    /* The Zephyr console does not provide portable display modes. */
    (void)ModeBits;
}
