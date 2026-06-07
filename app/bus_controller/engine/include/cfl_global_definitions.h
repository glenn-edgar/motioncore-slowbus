/**
 * @file cfl_global_definitions.h
 * @brief Platform configuration for ChainTree runtime
 *
 * Auto-detects 32-bit vs 64-bit platforms.  Override by defining
 * CFL_32BIT or CFL_64BIT before including this header, or via
 * compiler flag:  -DCFL_32BIT  or  -DCFL_64BIT
 */

 #ifndef CFL_GLOBAL_DEFINITIONS_H
 #define CFL_GLOBAL_DEFINITIONS_H
 
 /* ========================================================================
  * PLATFORM DETECTION - manual override wins, then auto-detect
  * ======================================================================== */
 
 #if defined(CFL_32BIT) && defined(CFL_64BIT)
     #error "CFL_32BIT and CFL_64BIT cannot both be defined"
 #endif
 
 #if !defined(CFL_32BIT) && !defined(CFL_64BIT)
     /* Auto-detect from compiler/platform */
     #if defined(__LP64__) || defined(_WIN64)        \
         || (defined(__WORDSIZE) && __WORDSIZE == 64) \
         || defined(__x86_64__) || defined(__aarch64__) \
         || defined(__PPC64__) || defined(__s390x__)
         #define CFL_64BIT
     #else
         #define CFL_32BIT
     #endif
 #endif
 
 /* ========================================================================
  * ALIGNMENT AND BLOCK SIZES - derived from platform
  *
  *   32-bit (ARM Cortex-M, etc.):  4-byte alignment
  *   64-bit (x86-64, aarch64):     8-byte alignment
  * ======================================================================== */
 
 #ifdef CFL_64BIT
     #define BLOCK_ALIGNMENT    8
     #define ARENA_ALIGNMENT    8
     #define MIN_BLOCK_SIZE     8
 #else
     #define BLOCK_ALIGNMENT    4
     #define ARENA_ALIGNMENT    4
     #define MIN_BLOCK_SIZE     8   /* 8 even on 32-bit: room for free-list pointers */
 #endif
 
 /* ========================================================================
  * OPTIONAL DEBUG FLAGS - uncomment or pass via -D as needed
  * ======================================================================== */
 
 /* #define JSON_DEBUG  1 */
 /* #define CFL_PERM_DEBUG 1 */
 
 #endif /* CFL_GLOBAL_DEFINITIONS_H */