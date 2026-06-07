/* ============= REENTRANT cfl_perm.c ============= */
#include "cfl_perm.h"
#include "cfl_exception.h"
#include <string.h>
#include <stdlib.h>

/* Use global definitions for alignment: 4 bytes on 32-bit, 8 bytes on 64-bit */
#define PERM_ALIGNMENT     BLOCK_ALIGNMENT
#define MIN_ALLOC_SIZE     MIN_BLOCK_SIZE

#if defined(CFL_PERM_DEBUG)
#include <stdio.h>
#define DEBUG_PRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define DEBUG_PRINT(fmt, ...)
#endif

/* ========================================================================
 * INTERNAL HELPERS
 * ======================================================================== */

static inline uint16_t align_up(uint16_t value, uint16_t alignment) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        EXCEPTION("align_up: Alignment must be power of 2");
    }
    return (value + alignment - 1) & ~(uint16_t)(alignment - 1);
}

static inline bool is_power_of_2(uint16_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

static inline uint16_t ptr_to_idx(CflPerm *perm, void *ptr) {
    if (!ptr) {
        EXCEPTION("ptr_to_idx: NULL pointer");
    }
    return (uint16_t)((uint8_t *)ptr - perm->pool);
}

static inline void *idx_to_ptr(CflPerm *perm, uint16_t idx) {
    if (idx >= perm->pool_size) {
        EXCEPTION("idx_to_ptr: Index out of bounds");
    }
    return &perm->pool[idx];
}

static void update_stats(CflPerm *perm, uint16_t allocated_size) {
    perm->stats.total_allocations++;
    perm->stats.current_used_bytes = perm->used;

    if (perm->used > perm->stats.peak_used_bytes) {
        perm->stats.peak_used_bytes = perm->used;
    }

    if (allocated_size > perm->stats.largest_allocation) {
        perm->stats.largest_allocation = allocated_size;
    }

    if (perm->stats.total_allocations == 1 || allocated_size < perm->stats.smallest_allocation) {
        perm->stats.smallest_allocation = allocated_size;
    }
}

/* ========================================================================
 * LIFECYCLE — STACK / STATIC INSTANCE
 * ======================================================================== */

void cfl_perm_set_instance(cfl_perm_t *perm) {
    memset(perm, 0, sizeof(CflPerm));
    perm->magic = PERM_MAGIC_NONE;
    perm->initialized = false;
    perm->owns_pool = false;
}

/* ========================================================================
 * LIFECYCLE — HEAP-ALLOCATED STRUCT, EXTERNAL BUFFER
 * ======================================================================== */

CflPerm *cfl_perm_create(void) {
    CflPerm *perm = (CflPerm *)malloc(sizeof(CflPerm));
    if (!perm) {
        EXCEPTION("cfl_perm_create: Failed to allocate memory");
    }

    memset(perm, 0, sizeof(CflPerm));
    perm->magic = PERM_MAGIC_CREATE;
    perm->initialized = false;
    perm->owns_pool = false;

    return perm;
}

void cfl_perm_destroy(CflPerm *perm) {
    if (!perm) {
        EXCEPTION("cfl_perm_destroy: NULL perm pointer");
    }
    if (perm->magic != PERM_MAGIC_CREATE) {
        EXCEPTION("cfl_perm_destroy: wrong destroy path (was not created with cfl_perm_create)");
    }

    /* Free pool if we own it (shouldn't normally for this path, but be safe) */
    if (perm->owns_pool && perm->pool) {
        free(perm->pool);
    }

    perm->magic = 0;  /* Poison against double-free */
    free(perm);
}

/* ========================================================================
 * LIFECYCLE — HEAP-ALLOCATED STRUCT + POOL
 * ======================================================================== */

cfl_perm_t *cfl_perm_malloc_create(uint16_t size) {
    cfl_perm_t *perm = (cfl_perm_t *)malloc(sizeof(cfl_perm_t));
    if (!perm) {
        EXCEPTION("cfl_perm_malloc_create: Failed to allocate perm structure");
    }

    perm->pool = (uint8_t *)malloc(size);
    if (!perm->pool) {
        free(perm);
        EXCEPTION("cfl_perm_malloc_create: Failed to allocate pool memory");
    }

    /* cfl_perm_init sets owns_pool = false; we override after */
    cfl_perm_init(perm, perm->pool, size);
    perm->magic = PERM_MAGIC_MALLOC;
    perm->owns_pool = true;

    return perm;
}

void cfl_perm_malloc_destroy(cfl_perm_t *perm) {
    if (!perm) {
        EXCEPTION("cfl_perm_malloc_destroy: NULL perm pointer");
    }
    if (perm->magic != PERM_MAGIC_MALLOC) {
        EXCEPTION("cfl_perm_malloc_destroy: wrong destroy path (was not created with cfl_perm_malloc_create)");
    }

    if (perm->pool) {
        free(perm->pool);
    }

    perm->magic = 0;  /* Poison against double-free */
    free(perm);
}

/* ========================================================================
 * INITIALIZATION / RESET
 * ======================================================================== */

void cfl_perm_init(CflPerm *perm, void *buffer, uint16_t buffer_size) {
    if (!perm) {
        EXCEPTION("cfl_perm_init: NULL perm pointer");
    }
    if (!buffer) {
        EXCEPTION("cfl_perm_init: NULL buffer pointer");
    }
    if (buffer_size < MIN_ALLOC_SIZE) {
        EXCEPTION("cfl_perm_init: Buffer too small");
    }

    perm->pool = (uint8_t *)buffer;
    perm->pool_size = buffer_size;
    perm->used = 0;
    perm->owns_pool = false;
    /* magic is preserved — set by create path or set_instance */

    memset(&perm->stats, 0, sizeof(CflPermStats));
    memset(perm->pool, 0, perm->pool_size);

    perm->initialized = true;
}

void cfl_perm_reset(CflPerm *perm) {
    if (!perm) {
        EXCEPTION("cfl_perm_reset: NULL perm pointer");
    }
    if (!perm->pool) {
        EXCEPTION("cfl_perm_reset: NULL pool pointer");
    }

    memset(&perm->stats, 0, sizeof(CflPermStats));
    perm->used = 0;
    memset(perm->pool, 0, perm->pool_size);

    perm->initialized = true;
}

/* ========================================================================
 * ALLOCATION
 * ======================================================================== */

uint16_t cfl_perm_alloc(CflPerm *perm, uint16_t size_bytes) {
    return cfl_perm_alloc_aligned(perm, size_bytes, PERM_ALIGNMENT);
}

uint16_t cfl_perm_alloc_aligned(CflPerm *perm, uint16_t size_bytes, uint16_t alignment) {
    if (!perm) {
        EXCEPTION("cfl_perm_alloc_aligned: NULL perm pointer");
    }
    if (!perm->initialized) {
        EXCEPTION("cfl_perm_alloc_aligned: Allocator not initialized");
    }
    if (size_bytes == 0) {
        EXCEPTION("cfl_perm_alloc_aligned: Zero size allocation");
    }
    if (alignment == 0 || !is_power_of_2(alignment)) {
        EXCEPTION("cfl_perm_alloc_aligned: Alignment must be power of 2");
    }

    /* Round size up to PERM_ALIGNMENT (4 on 32-bit, 8 on 64-bit) */
    size_bytes = align_up(size_bytes, PERM_ALIGNMENT);

    if (size_bytes < MIN_ALLOC_SIZE) {
        size_bytes = MIN_ALLOC_SIZE;
    }

    /* Calculate aligned position using absolute address */
    uintptr_t current_addr = (uintptr_t)(perm->pool + perm->used);
    uintptr_t aligned_addr = (current_addr + (uintptr_t)(alignment - 1)) & ~(uintptr_t)(alignment - 1);
    uint16_t padding = (uint16_t)(aligned_addr - current_addr);

    uint16_t total_needed = padding + size_bytes;

    if (perm->used + total_needed > perm->pool_size) {
        EXCEPTION("cfl_perm_alloc_aligned: Out of memory");
    }

    uint16_t ret_idx = perm->used + padding;
    perm->used += total_needed;

    update_stats(perm, size_bytes);

    return ret_idx;
}

void *cfl_perm_alloc_pointer(CflPerm *perm, uint16_t size_bytes) {
    uint16_t idx = cfl_perm_alloc(perm, size_bytes);
    return idx_to_ptr(perm, idx);
}

void *cfl_perm_alloc_pointer_aligned(CflPerm *perm, uint16_t size_bytes, uint16_t alignment) {
    uint16_t idx = cfl_perm_alloc_aligned(perm, size_bytes, alignment);
    return idx_to_ptr(perm, idx);
}

/* ========================================================================
 * INDEX / POINTER CONVERSION
 * ======================================================================== */

void *cfl_perm_ptr(CflPerm *perm, uint16_t idx) {
    if (!perm) {
        EXCEPTION("cfl_perm_ptr: NULL perm pointer");
    }
    if (!perm->initialized) {
        EXCEPTION("cfl_perm_ptr: Allocator not initialized");
    }
    return idx_to_ptr(perm, idx);
}

uint16_t cfl_perm_ptr_to_idx(CflPerm *perm, void *ptr) {
    if (!perm) {
        EXCEPTION("cfl_perm_ptr_to_idx: NULL perm pointer");
    }
    if (!perm->initialized) {
        EXCEPTION("cfl_perm_ptr_to_idx: Allocator not initialized");
    }
    if (!ptr) {
        EXCEPTION("cfl_perm_ptr_to_idx: NULL pointer");
    }

    if ((uint8_t *)ptr < perm->pool || (uint8_t *)ptr >= perm->pool + perm->pool_size) {
        EXCEPTION("cfl_perm_ptr_to_idx: Pointer out of bounds");
    }

    return (uint16_t)((uint8_t *)ptr - perm->pool);
}

/* ========================================================================
 * DIAGNOSTICS
 * ======================================================================== */

uint16_t cfl_perm_used_bytes(CflPerm *perm) {
    if (!perm || !perm->initialized) return 0;
    return perm->used;
}

uint16_t cfl_perm_free_bytes(CflPerm *perm) {
    if (!perm || !perm->initialized) return 0;
    return perm->pool_size - perm->used;
}

void cfl_perm_get_stats(CflPerm *perm, CflPermStats *stats) {
    if (!perm || !perm->initialized || !stats) {
        EXCEPTION("cfl_perm_get_stats: Invalid parameters");
    }

    memcpy(stats, &perm->stats, sizeof(CflPermStats));
}

bool cfl_perm_validate(CflPerm *perm) {
    if (!perm || !perm->initialized) {
        return false;
    }

    if (perm->used > perm->pool_size) {
        DEBUG_PRINT("Validation failed: used > pool_size\n");
        return false;
    }

    if (perm->stats.current_used_bytes != perm->used) {
        DEBUG_PRINT("Validation failed: stats mismatch\n");
        return false;
    }

    if (perm->stats.peak_used_bytes < perm->used) {
        DEBUG_PRINT("Validation failed: peak < current\n");
        return false;
    }

    return true;
}