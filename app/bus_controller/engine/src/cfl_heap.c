/* ============= REENTRANT cfl_heap.c ============= */
#include "cfl_heap.h"
#include "cfl_exception.h"
#include <string.h>
#include <stdlib.h>
/* cfl_heap.h -> cfl_perm.h -> cfl_global_definitions.h provides:
 *   BLOCK_ALIGNMENT, MIN_BLOCK_SIZE
 */

#if defined(CFL_HEAP_DEBUG)
#include <stdio.h>
#define DEBUG_PRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define DEBUG_PRINT(fmt, ...)
#endif

/* ========================================================================
 * BLOCK STRUCTURE
 *
 * Each block: [BlockHeader | padding | data ... | footer(uint16_t)]
 * Guard magic numbers detect corruption.
 * ======================================================================== */

#define BLOCK_MAGIC_FREE      0xF2EE
#define BLOCK_MAGIC_ALLOC     0xA10C
#define BLOCK_FOOTER_MAGIC    0xF007

typedef struct BlockHeader {
    uint16_t magic;         /* BLOCK_MAGIC_FREE or BLOCK_MAGIC_ALLOC */
    uint16_t size;          /* Size of data area (not including header/footer) */
    uint16_t flags;         /* Allocation flags */
    uint16_t node_id;       /* ID of requesting node/component */
    uint16_t padding;       /* Alignment padding bytes before data */
    uint16_t footer_magic;  /* Placeholder — actual footer is AFTER data */
} BlockHeader;

#define HEADER_SIZE         (sizeof(BlockHeader) - sizeof(uint16_t))  /* Don't count footer placeholder */
#define FOOTER_SIZE         sizeof(uint16_t)
#define FLAG_ALLOCATED      0x0001

/* ========================================================================
 * INTERNAL HELPERS
 * ======================================================================== */

static inline uint16_t align_up(uint16_t value, uint16_t alignment) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        EXCEPTION("align_up: Alignment must be power of 2");
    }
    return (value + alignment - 1) & ~(alignment - 1);
}

static inline bool is_power_of_2(uint16_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

static inline bool is_allocated(BlockHeader *block) {
    return (block->flags & FLAG_ALLOCATED) != 0;
}

static inline void mark_allocated(BlockHeader *block, uint16_t node_id) {
    block->flags |= FLAG_ALLOCATED;
    block->magic = BLOCK_MAGIC_ALLOC;
    block->node_id = node_id;
}

static inline void mark_free(BlockHeader *block) {
    block->flags &= ~FLAG_ALLOCATED;
    block->magic = BLOCK_MAGIC_FREE;
    block->node_id = NODE_ID_NONE;
}

static inline uint16_t *get_footer_ptr(BlockHeader *block) {
    return (uint16_t *)((uint8_t *)block + HEADER_SIZE + block->size);
}

static inline void set_footer(BlockHeader *block) {
    *get_footer_ptr(block) = BLOCK_FOOTER_MAGIC;
}

static inline bool validate_block(BlockHeader *block) {
    if (block->magic != BLOCK_MAGIC_FREE && block->magic != BLOCK_MAGIC_ALLOC) {
        return false;
    }
    return *get_footer_ptr(block) == BLOCK_FOOTER_MAGIC;
}

static inline BlockHeader *get_next_block(CflHeap *heap, BlockHeader *block) {
    uint8_t *ptr = (uint8_t *)block + HEADER_SIZE + block->size + FOOTER_SIZE;
    if (ptr >= heap->pool + heap->pool_size) {
        return NULL;
    }
    return (BlockHeader *)ptr;
}

static inline uint16_t ptr_to_idx(CflHeap *heap, void *ptr) {
    if (!ptr) {
        EXCEPTION("ptr_to_idx: NULL pointer");
    }
    return (uint16_t)((uint8_t *)ptr - heap->pool);
}

static inline void *idx_to_ptr(CflHeap *heap, uint16_t idx) {
    if (idx >= heap->pool_size) {
        EXCEPTION("idx_to_ptr: Index out of bounds");
    }
    return &heap->pool[idx];
}

static inline void *block_to_data_ptr(BlockHeader *block) {
    return (uint8_t *)block + HEADER_SIZE + block->padding;
}

/* ========================================================================
 * STATISTICS AND COALESCING
 * ======================================================================== */

static void update_stats(CflHeap *heap) {
    uint16_t used = 0;
    uint16_t free_blocks = 0;
    uint16_t allocated_blocks = 0;
    uint16_t largest_free = 0;
    uint16_t total_blocks = 0;

    BlockHeader *block = (BlockHeader *)heap->pool;

    while (block != NULL) {
        total_blocks++;
        if (is_allocated(block)) {
            allocated_blocks++;
            used += HEADER_SIZE + block->size + FOOTER_SIZE;
        } else {
            free_blocks++;
            if (block->size > largest_free) {
                largest_free = block->size;
            }
        }
        block = get_next_block(heap, block);
    }

    heap->stats.current_blocks = total_blocks;
    heap->stats.allocated_blocks = allocated_blocks;
    heap->stats.free_blocks = free_blocks;
    heap->stats.current_used_bytes = used;
    heap->stats.largest_free_block = largest_free;

    if (used > heap->stats.peak_used_bytes) {
        heap->stats.peak_used_bytes = used;
    }
}

static void coalesce_free_blocks(CflHeap *heap) {
    BlockHeader *block = (BlockHeader *)heap->pool;

    while (block != NULL) {
        BlockHeader *next = get_next_block(heap, block);
        if (!is_allocated(block) && next != NULL && !is_allocated(next)) {
            block->size += HEADER_SIZE + next->size + FOOTER_SIZE;
            block->padding = 0;
            set_footer(block);
            /* Don't advance — check if more blocks can be merged */
        } else {
            block = next;
        }
    }
}

/* ========================================================================
 * LIFECYCLE
 * ======================================================================== */

CflHeap *cfl_heap_init(cfl_perm_t *perm, uint16_t buffer_size) {
    if (!perm) {
        EXCEPTION("cfl_heap_init: NULL perm pointer");
    }
    if (buffer_size < HEADER_SIZE + MIN_BLOCK_SIZE + FOOTER_SIZE) {
        EXCEPTION("cfl_heap_init: Buffer too small");
    }

    CflHeap *heap = (CflHeap *)cfl_perm_alloc_pointer(perm, sizeof(CflHeap));
    void *buffer = cfl_perm_alloc_pointer(perm, buffer_size);

    heap->pool = (uint8_t *)buffer;
    heap->pool_size = buffer_size;
    heap->owns_pool = false;

    memset(&heap->stats, 0, sizeof(CflHeapStats));
    memset(heap->pool, 0, heap->pool_size);

    /* Create initial free block spanning entire pool */
    BlockHeader *initial = (BlockHeader *)heap->pool;
    initial->magic = BLOCK_MAGIC_FREE;
    initial->size = heap->pool_size - HEADER_SIZE - FOOTER_SIZE;
    initial->flags = 0;
    initial->node_id = NODE_ID_NONE;
    initial->padding = 0;
    set_footer(initial);

    heap->initialized = true;
    update_stats(heap);

    return heap;
}

void cfl_heap_reset(CflHeap *heap) {
    if (!heap) {
        EXCEPTION("cfl_heap_reset: NULL heap pointer");
    }
    if (!heap->pool) {
        EXCEPTION("cfl_heap_reset: NULL pool pointer");
    }

    memset(&heap->stats, 0, sizeof(CflHeapStats));
    memset(heap->pool, 0, heap->pool_size);

    BlockHeader *initial = (BlockHeader *)heap->pool;
    initial->magic = BLOCK_MAGIC_FREE;
    initial->size = heap->pool_size - HEADER_SIZE - FOOTER_SIZE;
    initial->flags = 0;
    initial->node_id = NODE_ID_NONE;
    initial->padding = 0;
    set_footer(initial);

    heap->initialized = true;
    update_stats(heap);
}

/* ========================================================================
 * ALLOCATION — CORE
 *
 * cfl_heap_arena_alloc_aligned is the single allocation implementation.
 * cfl_heap_malloc delegates to it with default alignment and no node ID.
 * ======================================================================== */

uint16_t cfl_heap_arena_alloc_aligned(CflHeap *heap, uint16_t requesting_node_id,
                                      uint16_t size_bytes, uint16_t alignment) {
    if (!heap) {
        EXCEPTION("cfl_heap_arena_alloc_aligned: NULL heap pointer");
    }
    if (!heap->initialized) {
        EXCEPTION("cfl_heap_arena_alloc_aligned: Heap not initialized");
    }
    if (size_bytes == 0) {
        EXCEPTION("cfl_heap_arena_alloc_aligned: Zero size allocation");
    }
    if (alignment == 0 || !is_power_of_2(alignment)) {
        EXCEPTION("cfl_heap_arena_alloc_aligned: Alignment must be power of 2");
    }

    size_bytes = align_up(size_bytes, BLOCK_ALIGNMENT);
    if (size_bytes < MIN_BLOCK_SIZE) {
        size_bytes = MIN_BLOCK_SIZE;
    }

    BlockHeader *block = (BlockHeader *)heap->pool;

    while (block != NULL) {
        if (!validate_block(block)) {
            EXCEPTION("cfl_heap_arena_alloc_aligned: Heap corruption detected");
        }

        if (!is_allocated(block)) {
            uint8_t *block_data_start = (uint8_t *)block + HEADER_SIZE;
            uintptr_t data_addr = (uintptr_t)block_data_start;
            uintptr_t aligned_addr = (data_addr + alignment - 1) & ~(uintptr_t)(alignment - 1);
            uint16_t padding = (uint16_t)(aligned_addr - data_addr);
            uint16_t total_needed = padding + size_bytes;

            if (block->size >= total_needed) {
                /* Split if remainder is large enough for a new block */
                uint16_t remainder = block->size - total_needed;
                if (remainder >= HEADER_SIZE + MIN_BLOCK_SIZE + FOOTER_SIZE) {
                    BlockHeader *new_block = (BlockHeader *)((uint8_t *)block +
                        HEADER_SIZE + total_needed + FOOTER_SIZE);
                    new_block->magic = BLOCK_MAGIC_FREE;
                    new_block->size = remainder - HEADER_SIZE - FOOTER_SIZE;
                    new_block->flags = 0;
                    new_block->node_id = NODE_ID_NONE;
                    new_block->padding = 0;
                    set_footer(new_block);

                    block->size = total_needed;
                }

                block->padding = padding;
                mark_allocated(block, requesting_node_id);
                set_footer(block);

                heap->stats.total_allocations++;
                update_stats(heap);

                return ptr_to_idx(heap, (void *)aligned_addr);
            }
        }

        block = get_next_block(heap, block);
    }

    EXCEPTION("cfl_heap_arena_alloc_aligned: Out of memory");
    return INVALID_HEAP_IDX;
}

uint16_t cfl_heap_malloc(CflHeap *heap, uint16_t size_bytes) {
    return cfl_heap_arena_alloc_aligned(heap, NODE_ID_NONE, size_bytes, BLOCK_ALIGNMENT);
}

/* ========================================================================
 * FREE
 * ======================================================================== */

void cfl_heap_free(CflHeap *heap, uint16_t idx) {
    if (!heap) {
        EXCEPTION("cfl_heap_free: NULL heap pointer");
    }
    if (!heap->initialized) {
        EXCEPTION("cfl_heap_free: Heap not initialized");
    }
    if (idx >= heap->pool_size) {
        EXCEPTION("cfl_heap_free: Invalid heap index");
    }

    void *ptr = idx_to_ptr(heap, idx);

    /* Walk blocks to find which one contains this pointer */
    BlockHeader *block = (BlockHeader *)heap->pool;
    BlockHeader *target_block = NULL;

    while (block != NULL) {
        void *block_data = block_to_data_ptr(block);
        uint8_t *block_end = (uint8_t *)block_data + (block->size - block->padding);

        if (ptr >= block_data && ptr < (void *)block_end) {
            target_block = block;
            break;
        }
        block = get_next_block(heap, block);
    }

    if (!target_block) {
        EXCEPTION("cfl_heap_free: Invalid pointer - not found in any block");
    }

    if ((uint8_t *)target_block < heap->pool ||
        (uint8_t *)target_block >= heap->pool + heap->pool_size) {
        EXCEPTION("cfl_heap_free: Block pointer out of bounds");
    }

    if (!validate_block(target_block)) {
        EXCEPTION("cfl_heap_free: Heap corruption detected");
    }

    if (!is_allocated(target_block)) {
        EXCEPTION("cfl_heap_free: Double free detected");
    }

    mark_free(target_block);
    target_block->padding = 0;
    set_footer(target_block);

    heap->stats.total_frees++;

    coalesce_free_blocks(heap);
    update_stats(heap);
}

/* ========================================================================
 * POINTER-BASED WRAPPERS
 * ======================================================================== */

void *cfl_heap_malloc_pointer(CflHeap *heap, uint16_t size_bytes) {
    if (!heap || !heap->initialized) {
        EXCEPTION("cfl_heap_malloc_pointer: invalid heap");
    }

    uint16_t idx = cfl_heap_malloc(heap, size_bytes);
    if (idx == INVALID_HEAP_IDX) {
        return NULL;
    }
    return cfl_heap_ptr(heap, idx);
}

void cfl_heap_free_pointer(CflHeap *heap, void *ptr) {
    if (!heap || !heap->initialized) {
        EXCEPTION("cfl_heap_free_pointer: invalid heap");
    }
    if (!ptr) {
        EXCEPTION("cfl_heap_free_pointer: NULL pointer");
    }

    cfl_heap_free(heap, cfl_heap_ptr_to_idx(heap, ptr));
}

/* ========================================================================
 * INDEX / POINTER CONVERSION
 * ======================================================================== */

void *cfl_heap_ptr(CflHeap *heap, uint16_t idx) {
    if (!heap || !heap->initialized) {
        EXCEPTION("cfl_heap_ptr: invalid heap");
    }
    return idx_to_ptr(heap, idx);
}

uint16_t cfl_heap_ptr_to_idx(CflHeap *heap, void *ptr) {
    if (!heap || !heap->initialized) {
        EXCEPTION("cfl_heap_ptr_to_idx: invalid heap");
    }
    if (!ptr) {
        EXCEPTION("cfl_heap_ptr_to_idx: NULL pointer");
    }

    if ((uint8_t *)ptr < heap->pool || (uint8_t *)ptr >= heap->pool + heap->pool_size) {
        EXCEPTION("cfl_heap_ptr_to_idx: Pointer out of bounds");
    }

    return (uint16_t)((uint8_t *)ptr - heap->pool);
}

/* ========================================================================
 * DIAGNOSTICS
 * ======================================================================== */

uint16_t cfl_heap_used_bytes(CflHeap *heap) {
    if (!heap || !heap->initialized) return 0;
    return heap->stats.current_used_bytes;
}

uint16_t cfl_heap_free_bytes(CflHeap *heap) {
    if (!heap || !heap->initialized) return 0;
    return heap->pool_size - heap->stats.current_used_bytes;
}

void cfl_heap_get_stats(CflHeap *heap, CflHeapStats *stats) {
    if (!heap || !heap->initialized || !stats) {
        EXCEPTION("cfl_heap_get_stats: Invalid parameters");
    }
    update_stats(heap);
    memcpy(stats, &heap->stats, sizeof(CflHeapStats));
}

void cfl_heap_dump_stats(CflHeap *heap) {
    if (!heap || !heap->initialized) return;
    update_stats(heap);
    /* Stats stored in heap->stats — access via cfl_heap_get_stats() */
}

bool cfl_heap_validate(CflHeap *heap) {
    if (!heap || !heap->initialized) {
        return false;
    }

    BlockHeader *block = (BlockHeader *)heap->pool;
    uint16_t total_size = 0;

    while (block != NULL) {
        if (!validate_block(block)) {
            DEBUG_PRINT("Heap validation failed: Invalid magic at %p\n", (void *)block);
            return false;
        }

        total_size += HEADER_SIZE + block->size + FOOTER_SIZE;
        if (total_size > heap->pool_size) {
            DEBUG_PRINT("Heap validation failed: Size overflow\n");
            return false;
        }

        block = get_next_block(heap, block);
    }

    return true;
}

void cfl_heap_walk(CflHeap *heap,
    void (*callback)(void *block_ptr, uint16_t size, bool allocated, uint16_t node_id)) {

    if (!heap || !heap->initialized || !callback) {
        return;
    }

    BlockHeader *block = (BlockHeader *)heap->pool;

    while (block != NULL) {
        if (validate_block(block)) {
            callback(block_to_data_ptr(block),
                     block->size - block->padding,
                     is_allocated(block),
                     block->node_id);
        }
        block = get_next_block(heap, block);
    }
}

uint16_t cfl_heap_get_node_id(CflHeap *heap, uint16_t idx) {
    if (!heap || !heap->initialized) {
        EXCEPTION("cfl_heap_get_node_id: invalid heap");
    }
    if (idx >= heap->pool_size) {
        EXCEPTION("cfl_heap_get_node_id: Invalid heap index");
    }

    void *ptr = idx_to_ptr(heap, idx);

    BlockHeader *block = (BlockHeader *)heap->pool;
    while (block != NULL) {
        void *block_data = block_to_data_ptr(block);
        uint8_t *block_end = (uint8_t *)block_data + (block->size - block->padding);

        if (ptr >= block_data && ptr < (void *)block_end) {
            return block->node_id;
        }
        block = get_next_block(heap, block);
    }

    EXCEPTION("cfl_heap_get_node_id: Pointer not found in any block");
    return NODE_ID_NONE;
}