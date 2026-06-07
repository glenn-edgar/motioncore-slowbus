/* ============= REENTRANT cfl_perm.h ============= */
#ifndef CFL_PERM_H
#define CFL_PERM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "cfl_global_definitions.h"

#define INVALID_PERM_IDX  0xFFFF

/* Magic numbers — encode which creation path was used */
#define PERM_MAGIC_NONE    0x0000  /* Stack/static instance (no free needed)   */
#define PERM_MAGIC_CREATE  0x5045  /* 'PE' — cfl_perm_create (struct malloc'd) */
#define PERM_MAGIC_MALLOC  0x504D  /* 'PM' — cfl_perm_malloc_create (struct + pool malloc'd) */

/* Permanent allocator statistics */
typedef struct {
    uint16_t total_allocations;
    uint16_t current_used_bytes;
    uint16_t peak_used_bytes;
    uint16_t largest_allocation;
    uint16_t smallest_allocation;
} CflPermStats;

/* Permanent allocator instance — NO global state */
typedef struct CflPerm {
    uint8_t  *pool;          /* Pointer to memory pool */
    uint16_t  pool_size;     /* Total size of pool */
    uint16_t  used;          /* Bump pointer (bytes used) */
    uint16_t  magic;         /* Creation-path tag (guards against mismatched destroy) */
    bool      initialized;   /* Initialization flag */
    bool      owns_pool;     /* True if allocator owns the pool memory */
    CflPermStats stats;      /* Runtime statistics */
} CflPerm, cfl_perm_t;

/* ========================================================================
 * ALLOCATOR LIFECYCLE
 * ======================================================================== */

/* Heap-allocated struct, external buffer — destroy with cfl_perm_destroy */
CflPerm *cfl_perm_create(void);
void cfl_perm_destroy(CflPerm *perm);

/* Heap-allocated struct + pool — destroy with cfl_perm_malloc_destroy */
cfl_perm_t *cfl_perm_malloc_create(uint16_t size);
void cfl_perm_malloc_destroy(cfl_perm_t *perm);

/* Stack/static instance — no destroy needed */
void cfl_perm_set_instance(cfl_perm_t *perm);

/* Initialize with external buffer (call after create or set_instance) */
void cfl_perm_init(CflPerm *perm, void *buffer, uint16_t buffer_size);

/* Reset to initial state (all allocations lost) */
void cfl_perm_reset(CflPerm *perm);

/* ========================================================================
 * ALLOCATION — NO FREE (permanent allocations)
 * ======================================================================== */

/* Returns index into pool */
uint16_t cfl_perm_alloc(CflPerm *perm, uint16_t size_bytes);
uint16_t cfl_perm_alloc_aligned(CflPerm *perm, uint16_t size_bytes, uint16_t alignment);

/* Returns pointer */
void *cfl_perm_alloc_pointer(CflPerm *perm, uint16_t size_bytes);
void *cfl_perm_alloc_pointer_aligned(CflPerm *perm, uint16_t size_bytes, uint16_t alignment);

/* ========================================================================
 * INDEX / POINTER CONVERSION
 * ======================================================================== */

void *cfl_perm_ptr(CflPerm *perm, uint16_t idx);
uint16_t cfl_perm_ptr_to_idx(CflPerm *perm, void *ptr);

/* ========================================================================
 * DIAGNOSTICS
 * ======================================================================== */

uint16_t cfl_perm_used_bytes(CflPerm *perm);
uint16_t cfl_perm_free_bytes(CflPerm *perm);
void cfl_perm_get_stats(CflPerm *perm, CflPermStats *stats);
bool cfl_perm_validate(CflPerm *perm);

/* ========================================================================
 * STATIC ALLOCATION HELPERS
 *
 * CFL_PERM_DEFINE_STATIC(name, size) declares aligned storage and a
 * CflPerm pointer.  Call CFL_PERM_INIT_STATIC(name) once at startup
 * to wire the pool pointer and initialize the allocator.
 *
 * Example:
 *   CFL_PERM_DEFINE_STATIC(my_perm, 4096);
 *   void init(void) { CFL_PERM_INIT_STATIC(my_perm); }
 * ======================================================================== */

#define CFL_PERM_TOTAL_SIZE(buffer_size) \
    (sizeof(CflPerm) + (buffer_size))

#define CFL_PERM_DEFINE_STATIC(name, size)                                  \
    static uint8_t name##_storage[CFL_PERM_TOTAL_SIZE(size)]                \
        __attribute__((aligned(BLOCK_ALIGNMENT)));                          \
    static CflPerm *name = (CflPerm *)name##_storage

#define CFL_PERM_INIT_STATIC(name)                                          \
    do {                                                                    \
        cfl_perm_set_instance(name);                                        \
        cfl_perm_init(name,                                                 \
                      name##_storage + sizeof(CflPerm),                     \
                      (uint16_t)(sizeof(name##_storage) - sizeof(CflPerm)));\
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* CFL_PERM_H */