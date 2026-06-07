/* ============= cfl_heap_arena_allocate.c ============= */
#include "cfl_heap_arena_allocate.h"
#include "cfl_exception.h"
#include <string.h>
#include <stdio.h>
/* cfl_heap_arena_allocate.h provides: cfl_heap.h, cfl_perm.h, cfl_global_definitions.h */

/* ARENA_ALIGNMENT is defined in cfl_global_definitions.h (4 on 32-bit, 8 on 64-bit) */

#define NO_ALLOCATOR        0xFF
#define INVALID_MEMORY_IDX  0xFFFF

/* ========================================================================
 * ARENA CONTROL BLOCK (internal)
 * ======================================================================== */

struct CflHeapArenaControl {
    uint16_t memory_idx;        /* Heap index to data block                     */
    uint16_t size;              /* Size of data block                           */
    uint16_t used;              /* Bump pointer offset                          */
    uint16_t owner_node_id;     /* Which node owns this arena                   */
    cfl_heap_allocator_id_t id; /* Allocator ID                                 */
    uint8_t pad;                /* Explicit padding for alignment               */
};

/* ========================================================================
 * INTERNAL HELPERS
 * ======================================================================== */

static inline uint16_t align_up(uint16_t value, uint8_t alignment) {
    return (value + alignment - 1) & ~(uint16_t)(alignment - 1);
}

/**
 * @brief Allocate a free allocator ID.
 *
 * FIX: Previous version used hardcoded MAX_ALLOCATORS (254) instead of
 * sys->max_allocator_count.  If the system was created with a smaller
 * count, this would scan past the allocated arrays — out-of-bounds R/W.
 */
static cfl_heap_allocator_id_t find_free_id(CflHeapArenaSystem *sys) {
    uint16_t max = sys->max_allocator_count;
    for (uint16_t i = 0; i < max; ++i) {
        cfl_heap_allocator_id_t id = (sys->next_allocator_id + i) % max;
        if (sys->arenas[id] == NULL) {
            sys->next_allocator_id = (id + 1) % max;
            return id;
        }
    }
    EXCEPTION("find_free_id: All allocator IDs exhausted");
    return NO_ALLOCATOR;
}

static inline void release_id(CflHeapArenaSystem *sys, cfl_heap_allocator_id_t id) {
    if (id < sys->max_allocator_count) {
        sys->arenas[id] = NULL;
    }
}

static inline CflHeapArenaControl *get_arena(CflHeapArenaSystem *sys, cfl_heap_allocator_id_t id) {
    if (id >= sys->max_allocator_count) {
        EXCEPTION("get_arena: Invalid allocator ID");
    }
    return sys->arenas[id];
}

/**
 * @brief Get base pointer for an arena's data block.
 *
 * Allocator 0 uses a permanent buffer (from perm); all others use
 * heap-allocated buffers.  This pattern was repeated 4 times.
 */
static inline uint8_t *arena_base_ptr(CflHeapArenaSystem *sys, CflHeapArenaControl *arena) {
    if (arena->id == 0) {
        return (uint8_t *)sys->allocator_0_buffer;
    }
    return (uint8_t *)cfl_heap_ptr(sys->heap, arena->memory_idx);
}

/**
 * @brief Core bump-allocator: align within arena and advance used pointer.
 *
 * This is the single implementation of the allocate-from-arena logic.
 * Returns pointer to the aligned allocation, or throws EXCEPTION on exhaustion.
 * Sets *out_offset to the aligned offset within the arena (for node_memory_index).
 */
static void *arena_bump_alloc(CflHeapArenaSystem *sys,
                              CflHeapArenaControl *arena,
                              uint16_t size_bytes,
                              uint8_t alignment,
                              uint16_t *out_offset) {

    uint8_t *base = arena_base_ptr(sys, arena);

    uintptr_t current_addr = (uintptr_t)(base + arena->used);
    uintptr_t aligned_addr = (current_addr + (uintptr_t)(alignment - 1)) & ~(uintptr_t)(alignment - 1);
    uint16_t padding = (uint16_t)(aligned_addr - current_addr);
    uint16_t total_needed = padding + size_bytes;

    if (arena->used + total_needed > arena->size) {
        EXCEPTION("arena_bump_alloc: Arena exhausted - insufficient space");
    }

    if (out_offset) {
        *out_offset = arena->used + padding;
    }

    arena->used += total_needed;

    return (void *)aligned_addr;
}

/**
 * @brief Common validation for allocation entry points.
 */
static inline void validate_alloc_args(CflHeapArenaSystem *sys,
                                       uint16_t node_index,
                                       uint16_t size_bytes) {
    if (!sys) {
        EXCEPTION("arena_alloc: NULL system pointer");
    }
    if (size_bytes == 0) {
        EXCEPTION("arena_alloc: size_bytes is zero");
    }
    if (!sys->node_allocator_ids) {
        EXCEPTION("arena_alloc: Arena system not initialized");
    }
    if (node_index >= sys->total_node_count) {
        EXCEPTION("arena_alloc: node_index out of bounds");
    }
}

/* ========================================================================
 * LIFECYCLE
 * ======================================================================== */

CflHeapArenaSystem *cfl_heap_arena_system_create(cfl_perm_t *perm, cfl_heap_t *heap,
                                                  uint16_t max_allocator_count,
                                                  uint16_t total_node_count,
                                                  uint16_t allocator_0_size) {
    if (!perm) {
        EXCEPTION("cfl_heap_arena_system_create: NULL perm pointer");
    }
    if (!heap) {
        EXCEPTION("cfl_heap_arena_system_create: NULL heap pointer");
    }
    if (allocator_0_size == 0) {
        EXCEPTION("cfl_heap_arena_system_create: allocator_0_size is zero");
    }

    /* Allocate system structure from permanent allocator */
    CflHeapArenaSystem *sys = (CflHeapArenaSystem *)cfl_perm_alloc_pointer(perm, sizeof(CflHeapArenaSystem));

    sys->heap = heap;
    sys->max_allocator_count = max_allocator_count;

    /* Pre-allocate ALL control blocks from permanent allocator */
    sys->control_blocks = (CflHeapArenaControl *)cfl_perm_alloc_pointer(perm,
        max_allocator_count * sizeof(CflHeapArenaControl));
    memset(sys->control_blocks, 0, max_allocator_count * sizeof(CflHeapArenaControl));

    /* Arena lookup table */
    sys->arenas = (CflHeapArenaControl **)cfl_perm_alloc_pointer(perm,
        max_allocator_count * sizeof(CflHeapArenaControl *));
    memset(sys->arenas, 0, max_allocator_count * sizeof(CflHeapArenaControl *));

    sys->next_allocator_id = 1;          /* 0 is reserved */
    sys->active_allocator_context = 0;   /* Default to allocator 0 */

    /* Node arrays */
    sys->node_allocator_ids = (uint8_t *)cfl_perm_alloc_pointer(perm,
        total_node_count * sizeof(uint8_t));

    sys->node_memory_index = (uint16_t *)cfl_perm_alloc_pointer(perm,
        total_node_count * sizeof(uint16_t));

    sys->total_node_count = total_node_count;

    /* All nodes start assigned to allocator 0, with no memory allocated */
    memset(sys->node_allocator_ids, 0, total_node_count * sizeof(uint8_t));
    for (uint16_t i = 0; i < total_node_count; ++i) {
        sys->node_memory_index[i] = INVALID_MEMORY_IDX;
    }

    /* Create allocator 0 — permanent, cannot be destroyed */
    allocator_0_size = align_up(allocator_0_size, ARENA_ALIGNMENT);

    void *arena0_buffer = cfl_perm_alloc_pointer(perm, allocator_0_size);
    sys->allocator_0_buffer = arena0_buffer;

    CflHeapArenaControl *arena0 = &sys->control_blocks[0];
    arena0->memory_idx = 0;    /* Marker: 0 means use sys->allocator_0_buffer */
    arena0->size = allocator_0_size;
    arena0->used = 0;
    arena0->owner_node_id = 0xFFFF;  /* System-owned */
    arena0->id = 0;
    arena0->pad = 0;

    sys->arenas[0] = arena0;

    return sys;
}

void cfl_heap_arena_system_reset(CflHeapArenaSystem *sys) {
    if (!sys) {
        EXCEPTION("cfl_heap_arena_system_reset: NULL system pointer");
    }
    if (!sys->node_allocator_ids || !sys->node_memory_index) {
        EXCEPTION("cfl_heap_arena_system_reset: Arena system not initialized");
    }

    /* Destroy all allocators except 0 */
    for (uint16_t i = 1; i < sys->max_allocator_count; ++i) {
        if (sys->arenas[i]) {
            CflHeapArenaControl *arena = sys->arenas[i];
            cfl_heap_free(sys->heap, arena->memory_idx);
            memset(arena, 0, sizeof(CflHeapArenaControl));
            sys->arenas[i] = NULL;
        }
    }

    /* Reset allocator 0 */
    if (sys->arenas[0]) {
        sys->arenas[0]->used = 0;
        memset(sys->allocator_0_buffer, 0, sys->arenas[0]->size);
    }

    /* Reinitialize node arrays */
    memset(sys->node_allocator_ids, 0, sys->total_node_count * sizeof(uint8_t));
    for (uint16_t i = 0; i < sys->total_node_count; ++i) {
        sys->node_memory_index[i] = INVALID_MEMORY_IDX;
    }

    sys->next_allocator_id = 1;
    sys->active_allocator_context = 0;
}

/* ========================================================================
 * ARENA CREATE / DESTROY
 * ======================================================================== */

cfl_heap_allocator_id_t cfl_heap_arena_create(CflHeapArenaSystem *sys,
                                               uint16_t owner_node_id,
                                               uint16_t size_bytes) {
    if (!sys) {
        EXCEPTION("cfl_heap_arena_create: NULL system pointer");
    }
    if (size_bytes == 0) {
        EXCEPTION("cfl_heap_arena_create: size_bytes is zero");
    }
    if (!sys->node_allocator_ids) {
        EXCEPTION("cfl_heap_arena_create: Arena system not initialized");
    }
    if (owner_node_id >= sys->total_node_count) {
        printf("owner_node_id: %d\n", owner_node_id);
        printf("sys->total_node_count: %d\n", sys->total_node_count);
        EXCEPTION("cfl_heap_arena_create: owner_node_id out of bounds");
    }

    size_bytes = align_up(size_bytes, ARENA_ALIGNMENT);

    cfl_heap_allocator_id_t id = find_free_id(sys);

    CflHeapArenaControl *arena = &sys->control_blocks[id];
    
    uint16_t data_idx = cfl_heap_malloc(sys->heap, size_bytes);

    if (data_idx == INVALID_HEAP_IDX) {
        EXCEPTION("cfl_heap_arena_create: Heap exhausted");
    }

    arena->memory_idx = data_idx;
    arena->size = size_bytes;
    arena->used = 0;
    arena->owner_node_id = owner_node_id;
    arena->id = id;
    arena->pad = 0;

    sys->arenas[id] = arena;
    sys->node_allocator_ids[owner_node_id] = id;

    return id;
}

void cfl_heap_arena_destroy(CflHeapArenaSystem *sys,
                            cfl_heap_allocator_id_t id,
                            uint16_t owner_node_id) {
    if (!sys) {
        EXCEPTION("cfl_heap_arena_destroy: NULL system pointer");
    }
    if (!sys->node_allocator_ids) {
        EXCEPTION("cfl_heap_arena_destroy: Arena system not initialized");
    }
    if (id >= sys->max_allocator_count) {
        EXCEPTION("cfl_heap_arena_destroy: Invalid allocator ID");
    }
    if (id == 0) {
        EXCEPTION("cfl_heap_arena_destroy: Cannot destroy allocator 0 (permanent)");
    }

    CflHeapArenaControl *arena = sys->arenas[id];
    if (!arena) {
        EXCEPTION("cfl_heap_arena_destroy: Allocator ID not in use");
    }
    if (arena->owner_node_id != owner_node_id) {
        EXCEPTION("cfl_heap_arena_destroy: Node does not own this arena");
    }

    /* Clear all nodes that were using this allocator */
    for (uint16_t i = 0; i < sys->total_node_count; ++i) {
        if (sys->node_allocator_ids[i] == id) {
            sys->node_allocator_ids[i] = 0;
            sys->node_memory_index[i] = INVALID_MEMORY_IDX;
        }
    }

    if (sys->active_allocator_context == id) {
        sys->active_allocator_context = 0;
    }

    cfl_heap_free(sys->heap, arena->memory_idx);
    memset(arena, 0, sizeof(CflHeapArenaControl));
    sys->arenas[id] = NULL;

    /* release_id is redundant here since we just set arenas[id] = NULL,
     * but call it for clarity — find_free_id checks arenas[id] == NULL */
    release_id(sys, id);
}

/* ========================================================================
 * ALLOCATOR CONTEXT MANAGEMENT
 * ======================================================================== */

void cfl_heap_arena_set_active_allocator(CflHeapArenaSystem *sys, uint16_t owner_node_id) {
    if (!sys) {
        EXCEPTION("cfl_heap_arena_set_active_allocator: NULL system pointer");
    }
    if (!sys->node_allocator_ids) {
        EXCEPTION("cfl_heap_arena_set_active_allocator: Arena system not initialized");
    }
    if (owner_node_id >= sys->total_node_count) {
        EXCEPTION("cfl_heap_arena_set_active_allocator: owner_node_id out of bounds");
    }

    sys->active_allocator_context = sys->node_allocator_ids[owner_node_id];
}

void cfl_heap_arena_set_active_allocator_id(CflHeapArenaSystem *sys, cfl_heap_allocator_id_t allocator_id) {
    if (!sys) {
        EXCEPTION("cfl_heap_arena_set_active_allocator_id: NULL system pointer");
    }
    if (allocator_id >= sys->max_allocator_count) {
        EXCEPTION("cfl_heap_arena_set_active_allocator_id: allocator_id out of bounds");
    }
    if (sys->arenas[allocator_id] == NULL) {
        EXCEPTION("cfl_heap_arena_set_active_allocator_id: allocator_id not in use");
    }

    sys->active_allocator_context = allocator_id;
}

void cfl_heap_arena_set_node_allocator(CflHeapArenaSystem *sys, uint16_t requesting_node_id) {
    if (!sys) {
        EXCEPTION("cfl_heap_arena_set_node_allocator: NULL system pointer");
    }
    if (!sys->node_allocator_ids) {
        EXCEPTION("cfl_heap_arena_set_node_allocator: Arena system not initialized");
    }
    if (requesting_node_id >= sys->total_node_count) {
        EXCEPTION("cfl_heap_arena_set_node_allocator: requesting_node_id out of bounds");
    }

    sys->node_allocator_ids[requesting_node_id] = sys->active_allocator_context;
}

/* ========================================================================
 * ALLOCATION — NODE-BASED (updates node_memory_index)
 *
 * Allocates from the node's assigned arena.
 * ======================================================================== */

void *cfl_arena_system_alloc(CflHeapArenaSystem *sys, uint16_t requesting_node_id, uint16_t size_bytes) {
    return cfl_arena_system_alloc_aligned(sys, requesting_node_id, size_bytes, ARENA_ALIGNMENT);
}

void *cfl_arena_system_alloc_aligned(CflHeapArenaSystem *sys, uint16_t requesting_node_id,
                                     uint16_t size_bytes, uint8_t alignment) {
    validate_alloc_args(sys, requesting_node_id, size_bytes);

    cfl_heap_allocator_id_t allocator_id = sys->node_allocator_ids[requesting_node_id];
    if (allocator_id == NO_ALLOCATOR) {
        EXCEPTION("cfl_arena_system_alloc_aligned: Node has no allocator assigned");
    }

    CflHeapArenaControl *arena = get_arena(sys, allocator_id);
    if (!arena) {
        EXCEPTION("cfl_arena_system_alloc_aligned: Invalid arena for allocator ID");
    }

    uint16_t offset;
    void *ptr = arena_bump_alloc(sys, arena, size_bytes, alignment, &offset);

    sys->node_memory_index[requesting_node_id] = offset;

    return ptr;
}

/* ========================================================================
 * ALLOCATION — ADDITIONAL (does NOT update node_memory_index)
 * ======================================================================== */

void *cfl_arena_additional_alloc(CflHeapArenaSystem *sys, uint16_t node_index, uint16_t size_bytes) {
    return cfl_arena_additional_alloc_aligned(sys, node_index, size_bytes, ARENA_ALIGNMENT);
}

void *cfl_arena_additional_alloc_aligned(CflHeapArenaSystem *sys, uint16_t node_index,
                                         uint16_t size_bytes, uint8_t alignment) {
    validate_alloc_args(sys, node_index, size_bytes);

    cfl_heap_allocator_id_t allocator_id = sys->node_allocator_ids[node_index];
    if (allocator_id == NO_ALLOCATOR) {
        EXCEPTION("cfl_arena_additional_alloc_aligned: Node has no allocator assigned");
    }

    CflHeapArenaControl *arena = get_arena(sys, allocator_id);
    if (!arena) {
        EXCEPTION("cfl_arena_additional_alloc_aligned: Invalid arena for allocator ID");
    }

    return arena_bump_alloc(sys, arena, size_bytes, alignment, NULL);
}

/* ========================================================================
 * ALLOCATION — FROM ACTIVE CONTEXT
 *
 * Allocates from the currently active allocator, assigns the node to it,
 * and updates node_memory_index.
 * ======================================================================== */

void *cfl_arena_alloc_from_active(CflHeapArenaSystem *sys, uint16_t node_index, uint16_t size_bytes) {
    return cfl_arena_alloc_from_active_aligned(sys, node_index, size_bytes, ARENA_ALIGNMENT);
}

void *cfl_arena_alloc_from_active_aligned(CflHeapArenaSystem *sys, uint16_t node_index,
                                           uint16_t size_bytes, uint8_t alignment) {
    validate_alloc_args(sys, node_index, size_bytes);

    cfl_heap_allocator_id_t allocator_id = sys->active_allocator_context;
    if (allocator_id == NO_ALLOCATOR) {
        EXCEPTION("cfl_arena_alloc_from_active_aligned: No active allocator context set");
    }

    CflHeapArenaControl *arena = get_arena(sys, allocator_id);
    if (!arena) {
        EXCEPTION("cfl_arena_alloc_from_active_aligned: Invalid arena for active context");
    }

    uint16_t offset;
    void *ptr = arena_bump_alloc(sys, arena, size_bytes, alignment, &offset);

    sys->node_memory_index[node_index] = offset;
    sys->node_allocator_ids[node_index] = allocator_id;

    return ptr;
}

/* ========================================================================
 * NODE ACCESSORS
 * ======================================================================== */

void *cfl_heap_arena_get_node_ptr(CflHeapArenaSystem *sys, uint16_t node_id) {
    if (!sys) {
        EXCEPTION("cfl_heap_arena_get_node_ptr: NULL system pointer");
    }
    if (!sys->node_allocator_ids || !sys->node_memory_index) {
        EXCEPTION("cfl_heap_arena_get_node_ptr: Arena system not initialized");
    }
    if (node_id >= sys->total_node_count) {
        EXCEPTION("cfl_heap_arena_get_node_ptr: node_id out of bounds");
    }

    uint16_t memory_idx = sys->node_memory_index[node_id];
    if (memory_idx == INVALID_MEMORY_IDX) {
        return NULL;
    }

    cfl_heap_allocator_id_t allocator_id = sys->node_allocator_ids[node_id];
    if (allocator_id == NO_ALLOCATOR) {
        EXCEPTION("cfl_heap_arena_get_node_ptr: Node has no allocator assigned");
    }

    CflHeapArenaControl *arena = get_arena(sys, allocator_id);
    if (!arena) {
        EXCEPTION("cfl_heap_arena_get_node_ptr: Invalid arena for allocator ID");
    }

    return arena_base_ptr(sys, arena) + memory_idx;
}

cfl_heap_allocator_id_t cfl_heap_arena_get_node_allocator_id(CflHeapArenaSystem *sys, uint16_t node_id) {
    if (!sys || !sys->node_allocator_ids) {
        EXCEPTION("cfl_heap_arena_get_node_allocator_id: invalid system");
    }
    if (node_id >= sys->total_node_count) {
        EXCEPTION("cfl_heap_arena_get_node_allocator_id: node_id out of bounds");
    }
    return sys->node_allocator_ids[node_id];
}

void cfl_heap_arena_set_node_allocator_id(CflHeapArenaSystem *sys, uint16_t node_id,
                                           cfl_heap_allocator_id_t allocator_id) {
    if (!sys || !sys->node_allocator_ids) {
        EXCEPTION("cfl_heap_arena_set_node_allocator_id: invalid system");
    }
    if (node_id >= sys->total_node_count) {
        EXCEPTION("cfl_heap_arena_set_node_allocator_id: node_id out of bounds");
    }
    sys->node_allocator_ids[node_id] = allocator_id;
}

uint16_t cfl_heap_arena_get_node_memory_index(CflHeapArenaSystem *sys, uint16_t node_id) {
    if (!sys || !sys->node_memory_index) {
        EXCEPTION("cfl_heap_arena_get_node_memory_index: invalid system");
    }
    if (node_id >= sys->total_node_count) {
        EXCEPTION("cfl_heap_arena_get_node_memory_index: node_id out of bounds");
    }
    return sys->node_memory_index[node_id];
}

void cfl_heap_arena_set_node_memory_index(CflHeapArenaSystem *sys, uint16_t node_id, uint16_t memory_idx) {
    if (!sys || !sys->node_memory_index) {
        EXCEPTION("cfl_heap_arena_set_node_memory_index: invalid system");
    }
    if (node_id >= sys->total_node_count) {
        EXCEPTION("cfl_heap_arena_set_node_memory_index: node_id out of bounds");
    }
    sys->node_memory_index[node_id] = memory_idx;
}

/* ========================================================================
 * DIAGNOSTICS
 * ======================================================================== */

uint16_t cfl_heap_arena_used_bytes(CflHeapArenaSystem *sys, cfl_heap_allocator_id_t id) {
    if (!sys) {
        EXCEPTION("cfl_heap_arena_used_bytes: NULL system pointer");
    }
    CflHeapArenaControl *arena = get_arena(sys, id);
    if (!arena) {
        EXCEPTION("cfl_heap_arena_used_bytes: Allocator ID not in use");
    }
    return arena->used;
}

uint16_t cfl_heap_arena_free_bytes(CflHeapArenaSystem *sys, cfl_heap_allocator_id_t id) {
    if (!sys) {
        EXCEPTION("cfl_heap_arena_free_bytes: NULL system pointer");
    }
    CflHeapArenaControl *arena = get_arena(sys, id);
    if (!arena) {
        EXCEPTION("cfl_heap_arena_free_bytes: Allocator ID not in use");
    }
    return arena->size - arena->used;
}

CflHeapArenaStats cfl_heap_arena_dump_stats(CflHeapArenaSystem *sys) {
    CflHeapArenaStats stats = {0, 0, 0};

    if (!sys) {
        EXCEPTION("cfl_heap_arena_dump_stats: NULL system pointer");
    }

    printf("=== Arena System Statistics ===\n");
    printf("%-4s  %-6s  %-10s  %-10s  %-10s  %-5s\n",
           "ID", "Owner", "Size", "Used", "Free", "Util%");
    printf("----  ------  ----------  ----------  ----------  -----\n");

    for (uint16_t i = 0; i < sys->max_allocator_count; ++i) {
        CflHeapArenaControl *arena = sys->arenas[i];
        if (!arena) continue;

        uint16_t free_bytes = arena->size - arena->used;
        uint8_t utilization = (arena->size > 0)
            ? (uint8_t)((arena->used * 100) / arena->size) : 0;

        if (arena->owner_node_id == 0xFFFF) {
            printf("%-4u  %-6s  %-10u  %-10u  %-10u  %3u%%\n",
                   i, "sys", arena->size, arena->used, free_bytes, utilization);
        } else {
            printf("%-4u  %-6u  %-10u  %-10u  %-10u  %3u%%\n",
                   i, arena->owner_node_id, arena->size, arena->used, free_bytes, utilization);
        }

        stats.active_count++;
        stats.total_data_allocated += arena->size;
        stats.total_data_used += arena->used;
    }

    printf("----  ------  ----------  ----------  ----------  -----\n");
    uint8_t total_utilization = (stats.total_data_allocated > 0)
        ? (uint8_t)((stats.total_data_used * 100) / stats.total_data_allocated) : 0;
    printf("%-4s  %-6u  %-10u  %-10u  %-10u  %3u%%\n",
           "TOT", stats.active_count, stats.total_data_allocated, stats.total_data_used,
           stats.total_data_allocated - stats.total_data_used, total_utilization);
    printf("===============================\n");

    return stats;
}