/*
 * Copyright (c) 2026 Space Cubics
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef OS_IMPL_TASKS_H
#define OS_IMPL_TASKS_H

#include "common_types.h"

/* OS_TaskCreate stack contract for Zephyr supervisor tasks:
 *
 * Supply the symbol of a static-duration K_KERNEL_STACK_DEFINE object as
 * stack_pointer and K_KERNEL_STACK_SIZEOF(symbol) as stack_size. Plain byte
 * arrays, the usable-buffer pointer, and user-capable stacks are not part of
 * this contract. The caller must provide the complete declared object and
 * enough usable stack for the entry function, OSAL and native call paths.
 * Supervisor mode cannot verify the declaration or actual storage capacity.
 *
 * Invalid size/rounding geometry returns OS_ERR_INVALID_SIZE; a misaligned
 * base or overflowing address range returns OS_INVALID_POINTER. A claim
 * overlapping another OSAL generation's full stack object (including guard
 * and padding) returns OS_ERROR without waiting for that stack's owner.
 * The claim persists after public ID removal until native join completes.
 * Reuse through OSAL may therefore fail temporarily following self-exit;
 * public ID disappearance alone never permits external reuse of the storage.
 * Caller stacks are never freed. Lifetime must also exclude native threads
 * or other users outside OSAL, whose stack ownership is not tracked here.
 *
 * A NULL pointer returns OS_ERR_NOT_IMPLEMENTED unless
 * CONFIG_CFS_OSAL_DYNAMIC_TASK_STACKS is enabled. In that mode only the
 * stack is allocated, allocation failure returns OS_ERROR, and OSAL frees
 * the owned stack after join. Every task's k_thread is statically stored.
 * CONFIG_USERSPACE task creation is unsupported in both configurations.
 */
int32 OS_Zephyr_TaskAPI_Impl_Init(void);

/* Protect provider-owned resources from native task abort. Enter before
 * acquiring an internal resource, and leave after its final release. These
 * calls nest and do nothing for threads not created by OSAL. A task whose
 * deletion has committed parks in Enter until the deleter aborts it. */
void OS_Zephyr_TaskEnter(void);
void OS_Zephyr_TaskLeave(void);

static inline int32 OS_Zephyr_TaskLeaveResult(int32 status)
{
    OS_Zephyr_TaskLeave();
    return status;
}

/* Timebase helpers must unregister before terminating their native thread. */
void OS_Zephyr_TaskUnregister(void);

#endif /* OS_IMPL_TASKS_H */
