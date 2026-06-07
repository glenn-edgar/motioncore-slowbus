/*
 * fnv1a.c - FNV-1a 32-bit hash function
 *
 * Build as shared library:
 *   gcc -O2 -shared -fPIC -o libfnv1a.so fnv1a.c
 *
 * Build as static library:
 *   gcc -O2 -c fnv1a.c -o fnv1a.o
 *   ar rcs libfnv1a.a fnv1a.o
 */

 #include "fnv1a.h"

 uint32_t fnv1a_32(const char *str)
 {
     uint32_t hash = FNV1A_OFFSET_BASIS;
     while (*str) {
         hash ^= (uint8_t)*str++;
         hash *= FNV1A_PRIME;
     }
     return hash;
 }
 
 uint32_t fnv1a_32_len(const char *str, uint32_t len)
 {
     uint32_t hash = FNV1A_OFFSET_BASIS;
     for (uint32_t i = 0; i < len; i++) {
         hash ^= (uint8_t)str[i];
         hash *= FNV1A_PRIME;
     }
     return hash;
 }