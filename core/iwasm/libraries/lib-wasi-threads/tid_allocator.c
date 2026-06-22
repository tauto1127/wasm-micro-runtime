/*
 * Copyright (C) 2023 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "tid_allocator.h"
#include "platform_common.h"
#include "wasm_export.h"
#include "bh_log.h"
#include <string.h>

bh_static_assert(TID_MIN <= TID_MAX);
#define MIN(a, b) (((a) < (b)) ? (a) : (b))

bool
tid_allocator_init(TidAllocator *tid_allocator)
{
    tid_allocator->size = MIN(TID_ALLOCATOR_INIT_SIZE, TID_MAX - TID_MIN + 1);
    tid_allocator->pos = tid_allocator->size;
    tid_allocator->ids =
        wasm_runtime_malloc(tid_allocator->size * sizeof(int32));
    if (tid_allocator->ids == NULL)
        return false;

    for (int64 i = tid_allocator->pos - 1; i >= 0; i--)
        tid_allocator->ids[i] =
            (uint32)(TID_MIN + (tid_allocator->pos - 1 - i));

    return true;
}

void
tid_allocator_deinit(TidAllocator *tid_allocator)
{
    wasm_runtime_free(tid_allocator->ids);
}

int32
tid_allocator_get_tid(TidAllocator *tid_allocator)
{
    if (tid_allocator->pos == 0) { // Resize stack and push new thread ids
        if (tid_allocator->size == TID_MAX - TID_MIN + 1) {
            LOG_ERROR("Maximum thread identifier reached");
            return -1;
        }

        uint32 old_size = tid_allocator->size;
        uint32 new_size = MIN(tid_allocator->size * 2, TID_MAX - TID_MIN + 1);
        if (new_size != TID_MAX - TID_MIN + 1
            && new_size / 2 != tid_allocator->size) {
            LOG_ERROR("Overflow detected during new size calculation");
            return -1;
        }

        size_t realloc_size = new_size * sizeof(int32);
        if (realloc_size / sizeof(int32) != new_size) {
            LOG_ERROR("Overflow detected during realloc");
            return -1;
        }
        int32 *tmp =
            wasm_runtime_realloc(tid_allocator->ids, (uint32)realloc_size);
        if (tmp == NULL) {
            LOG_ERROR("Thread ID allocator realloc failed");
            return -1;
        }

        tid_allocator->size = new_size;
        tid_allocator->pos = new_size - old_size;
        tid_allocator->ids = tmp;
        for (int64 i = tid_allocator->pos - 1; i >= 0; i--)
            tid_allocator->ids[i] =
                (uint32)(TID_MIN + (tid_allocator->size - 1 - i));
    }

    // Pop available thread identifier from the stack
    return tid_allocator->ids[--tid_allocator->pos];
}

void
tid_allocator_release_tid(TidAllocator *tid_allocator, int32 thread_id)
{
    // Release thread identifier by pushing it into the stack
    bh_assert(tid_allocator->pos < tid_allocator->size);
    tid_allocator->ids[tid_allocator->pos++] = thread_id;
}

void
tid_allocator_restore(TidAllocator *tid_allocator, int32 *thread_ids,
                      uint32 count)
{
    uint32 i;
    uint32 required_size = tid_allocator ? tid_allocator->size : 0;
    uint32 free_count = 0;
    int32 max_used_tid = TID_MIN - 1;
    uint8 *used_map = NULL;

    if (!tid_allocator || !tid_allocator->ids)
        return;

    /* Find maximum used tid to decide allocator range */
    for (i = 0; i < count; i++) {
        if (thread_ids[i] > max_used_tid)
            max_used_tid = thread_ids[i];
    }

    if (max_used_tid >= TID_MIN) {
        uint64 needed = (uint64)max_used_tid - (uint64)TID_MIN + 1ULL;
        if (needed > (uint64)(TID_MAX - TID_MIN + 1))
            needed = (uint64)(TID_MAX - TID_MIN + 1);
        if (needed > required_size)
            required_size = (uint32)needed;
    }

    if (required_size < TID_ALLOCATOR_INIT_SIZE)
        required_size = TID_ALLOCATOR_INIT_SIZE;

    /* Grow allocator stack if needed so later get_tid() doesn't
       reintroduce used ids by auto-growing. */
    if (required_size > tid_allocator->size) {
        size_t realloc_size = (size_t)required_size * sizeof(int32);
        int32 *tmp =
            wasm_runtime_realloc(tid_allocator->ids, (uint32)realloc_size);
        if (!tmp) {
            LOG_ERROR("tid_allocator_restore: realloc failed");
            return;
        }
        tid_allocator->ids = tmp;
        tid_allocator->size = required_size;
    }

    used_map = wasm_runtime_malloc(required_size);
    if (!used_map) {
        LOG_ERROR("tid_allocator_restore: used_map alloc failed");
        return;
    }
    memset(used_map, 0, required_size);

    /* Mark used ids */
    for (i = 0; i < count; i++) {
        int32 tid = thread_ids[i];
        if (tid < TID_MIN)
            continue;
        uint64 idx = (uint64)tid - (uint64)TID_MIN;
        if (idx < required_size)
            used_map[idx] = 1;
    }

    /* Rebuild free-id stack in descending order so pop yields smallest id */
    for (int64 tid = (int64)TID_MIN + (int64)required_size - 1;
         tid >= (int64)TID_MIN; tid--) {
        uint32 idx = (uint32)(tid - TID_MIN);
        if (used_map[idx])
            continue;
        tid_allocator->ids[free_count++] = (int32)tid;
    }

    tid_allocator->pos = free_count;
    wasm_runtime_free(used_map);
}
