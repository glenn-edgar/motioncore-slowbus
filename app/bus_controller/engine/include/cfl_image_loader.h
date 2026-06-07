/*
 * ct_runtime.h - ChainTree Binary Image Runtime
 *
 * Loads .ctb binary images and populates a standard chaintree_handle_t.
 * Functions are registered by name at runtime via FNV-1a hash lookup.
 *
 * After loading and registration, the handle is identical to one produced
 * by the compiled .h/.c backend — the engine code is the same either way.
 *
 * Usage:
 *   ct_image_t *img = ct_image_load(mmap_ptr, file_size);
 *   ct_register_main(img, "my_handler", my_handler_fn);
 *   ct_register_one_shot(img, "my_init", my_init_fn);
 *   int errors = ct_validate(img);
 *   chaintree_handle_t *handle = ct_get_handle(img);
 *   // handle is now identical to compiled version
 */

 #ifndef CFL_IMAGE_LOADER_H
 #define CFL_IMAGE_LOADER_H
 
 #include "chaintree_support.h"
 #include "fnv1a.h"
 
 #include <stdint.h>
 #include <stdbool.h>
 
 /* ===== Binary Image Constants ===== */
 
 #define CTB_MAGIC           0x43544231  /* "CTB1" */
 #define CTB_VERSION_MAJOR   1
 #define CTB_VERSION_MINOR   0
 
 /* Header flags */
 #define CTB_FLAG_HAS_NODE_DATA  (1u << 0)
 #define CTB_FLAG_HAS_EVENTS     (1u << 1)
 #define CTB_FLAG_HAS_BITMASKS   (1u << 2)
 
 /* Section type identifiers */
 #define CTB_SECT_NODE   0x0001
 #define CTB_SECT_LINK   0x0002
 #define CTB_SECT_MFHT   0x0003
 #define CTB_SECT_OSHT   0x0004
 #define CTB_SECT_BFHT   0x0005
 #define CTB_SECT_FSTR   0x0006
 #define CTB_SECT_JREC   0x0007
 #define CTB_SECT_JCTL   0x0008
 #define CTB_SECT_JSTR   0x0009
 #define CTB_SECT_EVNT   0x000A
 #define CTB_SECT_BMSK   0x000B
 #define CTB_SECT_KBIN   0x000C
 #define CTB_SECT_KBAL   0x000D
 #define CTB_SECT_GSTR   0x000E
 
 /* ===== Binary Image On-Disk Structures ===== */
 
 typedef struct __attribute__((packed)) {
     uint32_t magic;
     uint16_t version_major;
     uint16_t version_minor;
     uint32_t flags;
     uint32_t total_image_size;
     uint32_t checksum;
     uint16_t section_count;
     uint16_t node_count;
     uint16_t node_active_count;
     uint16_t link_table_size;
     uint16_t main_func_count;
     uint16_t one_shot_func_count;
     uint16_t boolean_func_count;
     uint16_t event_count;
     uint16_t bitmask_count;
     uint16_t kb_count;
     uint16_t json_records_count;
     uint16_t json_controls_count;
     uint32_t json_strings_size;
     uint8_t  reserved[16];
 } ctb_header_t;
 
 typedef struct __attribute__((packed)) {
     uint32_t section_type;
     uint32_t offset;
     uint32_t size;
     uint16_t entry_count;
     uint16_t entry_size;
 } ctb_section_dir_t;
 
 /* On-disk bitmask entry */
 typedef struct __attribute__((packed)) {
     uint32_t string_pool_offset;
     uint8_t  bit_position;
     uint8_t  reserved[3];
 } ctb_bitmask_entry_t;
 
 /* On-disk KB info entry */
 typedef struct __attribute__((packed)) {
     uint32_t name_offset;
     uint16_t root_node_index;
     uint16_t start_index;
     uint16_t node_count;
     uint16_t max_depth;
     uint16_t memory_factor;
     uint16_t alias_start;
     uint16_t alias_count;
     uint8_t  reserved[6];
 } ctb_kb_info_entry_t;
 
 /* On-disk KB alias entry */
 typedef struct __attribute__((packed)) {
     uint32_t name_offset;
     uint16_t node_index;
     uint16_t reserved;
 } ctb_kb_alias_entry_t;
 
 /* ===== Runtime Image Handle ===== */
 
 typedef struct {
     /* Image memory */
     const void          *image_base;
     uint32_t             image_size;
     const ctb_header_t  *header;
 
     /* Standard handle (populated from image) */
     chaintree_handle_t   handle;
 
     /* Hash tables (point into image, used for registration) */
     const uint32_t      *main_hashes;
     const uint32_t      *one_shot_hashes;
     const uint32_t      *boolean_hashes;
 
     /* Function pointer arrays (allocated at runtime) */
     main_function_t     *main_fn_ptrs;
     one_shot_function_t *one_shot_fn_ptrs;
     boolean_function_t  *boolean_fn_ptrs;
 
     /* Function name strings (for diagnostics) */
     const char          *func_name_base;
     uint32_t             func_name_size;
 
     /* General string pool */
     const char          *string_pool;
     uint32_t             string_pool_size;
 
     /* Counts (from header) */
     uint16_t main_func_count;
     uint16_t one_shot_func_count;
     uint16_t boolean_func_count;
 
 } cfl_image_loader_t;
 
 /* ===== Error codes ===== */
 
 #define CFL_IMAGE_LOADER_OK                    0
 #define CFL_IMAGE_LOADER_NULL_PTR         -1
 #define CFL_IMAGE_LOADER_TOO_SMALL        -2
 #define CFL_IMAGE_LOADER_BAD_MAGIC        -3
 #define CFL_IMAGE_LOADER_BAD_VERSION      -4
 #define CFL_IMAGE_LOADER_BAD_CHECKSUM     -5
 #define CFL_IMAGE_LOADER_BAD_SIZE         -6
 #define CFL_IMAGE_LOADER_SECTION_OOB      -7
 #define CFL_IMAGE_LOADER_ALLOC            -8
 #define CFL_IMAGE_LOADER_HASH_NOT_FOUND   -9
 #define CFL_IMAGE_LOADER_UNREGISTERED    -10
 
 /* ===== API ===== */
 
 /*
  * Load a binary image into a runtime handle.
  * image_data: pointer to mmap'd or loaded .ctb file
  * image_size: size in bytes
  * out:        pointer to caller-allocated ct_image_t
  * Returns CT_OK on success, negative error code on failure.
  *
  * The image memory must remain valid for the lifetime of the handle.
  * This function allocates function pointer arrays (caller must call ct_image_free).
  */
 int cfl_image_loader_load(const void *image_data, uint32_t image_size, cfl_image_loader_t *out);
 
 /*
  * Free runtime-allocated resources (function pointer arrays).
  * Does NOT free the image memory itself.
  */
 void cfl_image_loader_free(cfl_image_loader_t *img);
 
 /*
  * Get the standard chaintree_handle_t pointer.
  * Valid after ct_image_load and function registration.
  */
 const chaintree_handle_t *cfl_image_loader_get_handle(const cfl_image_loader_t *img);
 
 /*
  * Register function implementations by name.
  * Returns the slot index (>= 0) on success, negative error on failure.
  */
 int cfl_image_loader_register_main(cfl_image_loader_t *img, const char *name, main_function_t fn);
 int cfl_image_loader_register_one_shot(cfl_image_loader_t *img, const char *name, one_shot_function_t fn);
 int cfl_image_loader_register_boolean(cfl_image_loader_t *img, const char *name, boolean_function_t fn);
 
 /*
  * Validate that all referenced function slots are registered.
  * Returns the number of unregistered slots (0 = all good).
  * If names_buf is non-NULL, writes missing function names to it.
  */
 int cfl_image_loader_validate(const cfl_image_loader_t *img);
 
 /*
  * Get function name for a given slot index (for diagnostics).
  * table_type: CTB_SECT_MFHT, CTB_SECT_OSHT, or CTB_SECT_BFHT
  */
 const char *cfl_image_loader_get_func_name(const cfl_image_loader_t *img, uint16_t table_type, uint16_t slot);
 
 /*
  * CRC32 utility (ISO 3309 / zlib compatible).
  */
 uint32_t cfl_image_loader_crc32(const void *data, uint32_t len);
 
 #endif /* CT_RUNTIME_H */