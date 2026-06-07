/*
 * fnv1a.h - FNV-1a 32-bit hash function
 *
 * Used by ChainTree binary image format for function lookup.
 * Builds as shared library (fnv1a.so) for LuaJIT FFI binding
 * and as part of ct_runtime for the C runtime.
 */

 #ifndef FNV1A_H
 #define FNV1A_H
 
 #include <stdint.h>
 
 #define FNV1A_OFFSET_BASIS 2166136261u
 #define FNV1A_PRIME        16777619u
 
 uint32_t fnv1a_32(const char *str);
 uint32_t fnv1a_32_len(const char *str, uint32_t len);
 
 #endif /* FNV1A_H */