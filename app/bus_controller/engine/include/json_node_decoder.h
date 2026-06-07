/* ============================================================================
 * Runtime Handle Integration
 * ============================================================================ */

 #ifndef JSON_NODE_DECODER_H
 #define JSON_NODE_DECODER_H
 
 #ifdef __cplusplus
 extern "C" {
 #endif
 
 #include "cfl_runtime.h"
 
 /* ============================================================================
  * json_node_decoder.h - JSON Decoder for ChainTree Node Data
  * ============================================================================ */
 
 #include <stdint.h>
 #include <stdbool.h>
 #include <stddef.h>
 
 /* Forward declarations */
 
 
 /* Note: json_type_t, json_record_t, and record_control_t are defined 
    in your existing code with the following structure:
 
 typedef enum {
     JSON_TYPE_STRING = 0,
     JSON_TYPE_INT32 = 1,
     JSON_TYPE_FLOAT32 = 2,
     JSON_TYPE_NULL = 3,
     JSON_TYPE_BOOL = 4,
     JSON_TYPE_ARRAY = 5,
     JSON_TYPE_OBJECT = 6
 } json_type_t;
 
 typedef struct {
     json_type_t object_type;
     union {
         uint32_t string_offset;
         int32_t i32_value;
         float f32_value;
         uint8_t bool_value;
         uint32_t container_count;
     } value;
 } json_record_t;
 
 typedef struct {
     uint32_t start_position;
     uint32_t num_records;
 } record_control_t;
 */
 
 /* ============================================================================
  * Decoder Context
  * ============================================================================ */
 
 /* ============================================================================
  * Core Decoder Functions
  * ============================================================================ */
 
 /**
  * Initialize decoder context from flash handle
  * 
  * @param ctx Context to initialize
  * @param flash_handle Pointer to chaintree flash handle structure
  * @param node_data_control_index Control region index (typically node->node_data_id)
  * Uses EXCEPTION for all errors
  */
 void json_decoder_init(
      json_decoder_ctx_t *ctx,
      const chaintree_handle_t *flash_handle,
     uint32_t node_index
 );
 
 /**
  * Get record by index with bounds checking
  */
 static inline const json_record_t *json_get_record(
     const  json_decoder_ctx_t *ctx,
     uint32_t record_idx)
 {
     if (!ctx || !ctx->records || record_idx >= ctx->records_count) {
         return NULL;
     }
     return &ctx->records[record_idx];
 }
 
 /**
  * Get string from string table by offset
  */
 static inline const char *json_get_string(
     const  json_decoder_ctx_t *ctx,
     uint32_t offset)
 {
     if (!ctx || !ctx->strings || offset >= ctx->strings_size) {
         return NULL;
     }
     return &ctx->strings[offset];
 }
 
 /* ============================================================================
  * Runtime Handle Integration (Primary Interface)
  * ============================================================================ */
 
 /**
  * Initialize decoder context for a specific node using runtime handle
  * 
  * @param runtime Runtime handle containing flash_handle and json_decoder_ctx
  * @param node_id Node data control index (typically from chaintree_node_t->node_data_id)
  * Uses EXCEPTION for all errors
  */
 void  json_decoder_init_from_runtime(
     cfl_runtime_handle_t *runtime,
     uint32_t node_id
 );
 
 /**
  * Extract int32 value by path from current active node
  * Accepts both INT32 and FLOAT32 types (float converted via truncation)
  * 
  * @param runtime Runtime handle
  * @param path Dot-separated path (e.g., "node_dict.timeout")
  * @param out Output value pointer
  * Uses EXCEPTION for errors including not found and type mismatch
  */
 void json_extract_int32_runtime(
     const cfl_runtime_handle_t *runtime,
     const char *path,
     int32_t *out
 );
 
 /**
  * Extract float32 value by path from current active node
  * Accepts both FLOAT32 and INT32 types (int converted to float)
  */
 void json_extract_float32_runtime(
     const cfl_runtime_handle_t *runtime,
     const char *path,
     float *out
 );
 
 /**
  * Extract boolean value by path from current active node
  */
 void json_extract_bool_runtime(
     const cfl_runtime_handle_t *runtime,
     const char *path,
     bool *out
 );
 
 /**
  * Extract string value by path from current active node
  * Returns pointer into string table (no allocation)
  */
 void json_extract_string_runtime(
     const cfl_runtime_handle_t *runtime,
     const char *path,
     const char **out
 );

 void json_extract_type_runtime(
     const cfl_runtime_handle_t *runtime,
     const char *path,
     json_type_t *out
 );
 
 /* ============================================================================
  * Path-Based Extraction API (Lower-Level Interface)
  * ============================================================================ */
 
 /**
  * Extract int32 value by path
  * Accepts both INT32 and FLOAT32 types (float converted via truncation)
  * 
  * @param ctx Decoder context
  * @param root_record Starting record index (typically control->start_position)
  * @param path Dot-separated path (e.g., "device.id" or "sensors[2].value")
  * @param out Output value pointer
  * Uses EXCEPTION for errors including not found and type mismatch
  */
 void json_extract_int32(
     const  json_decoder_ctx_t *ctx,
     uint32_t root_record,
     const char *path,
     int32_t *out
 );
 
 /**
  * Extract float32 value by path
  * Accepts both FLOAT32 and INT32 types (int converted to float)
  */
 void json_extract_float32(
     const  json_decoder_ctx_t *ctx,
     uint32_t root_record,
     const char *path,
     float *out
 );
 
 /**
  * Extract boolean value by path
  */
 void json_extract_bool(
     const  json_decoder_ctx_t *ctx,
     uint32_t root_record,
     const char *path,
     bool *out
 );
 
 /**
  * Extract string value by path
  * Returns pointer into string table (no allocation)
  */
 void json_extract_string(
     const  json_decoder_ctx_t *ctx,
     uint32_t root_record,
     const char *path,
     const char **out
 );
 
 /* ============================================================================
  * Low-Level Navigation Functions
  * ============================================================================ */
 
 /**
  * Find child in OBJECT by key name
  * Handles jsmn-style key-value pairs
  * 
  * @param ctx Decoder context
  * @param parent_record Parent OBJECT record index
  * @param key Key to search for
  * @param out_record Output record index if found (points to VALUE, not key)
  * Uses EXCEPTION if not found or on errors
  */
 void json_find_object_child(
     const  json_decoder_ctx_t *ctx,
     uint32_t parent_record,
     const char *key,
     uint32_t *out_record
 );
 
 /**
  * Get child from ARRAY by index
  * 
  * @param ctx Decoder context
  * @param parent_record Parent ARRAY record index
  * @param index Array index (0-based)
  * @param out_record Output record index
  * Uses EXCEPTION on errors
  */
 void json_get_array_child(
     const  json_decoder_ctx_t *ctx,
     uint32_t parent_record,
     uint32_t index,
     uint32_t *out_record
 );
 
 /**
  * Navigate path to find target record
  * Supports both object keys and array indices
  * 
  * @param ctx Decoder context
  * @param root_record Starting record
  * @param path Path string (e.g., "device.sensors[2].temp")
  * @param out_record Output record index
  * Uses EXCEPTION on errors
  */
 void json_navigate_path(
     const  json_decoder_ctx_t *ctx,
     uint32_t root_record,
     const char *path,
     uint32_t *out_record
 );
 
 /* ============================================================================
  * Type-Safe Value Getters (from record index)
  * ============================================================================ */
 
 /**
  * Get int32 value from record
  * Accepts both INT32 and FLOAT32 types (float converted via truncation)
  * Uses EXCEPTION for type mismatch
  */
 void json_get_int32(
     const  json_decoder_ctx_t *ctx,
     uint32_t record_idx,
     int32_t *out
 );
 
 /**
  * Get float32 value from record
  * Accepts both FLOAT32 and INT32 types (int converted to float)
  */
 void json_get_float32(
     const  json_decoder_ctx_t *ctx,
     uint32_t record_idx,
     float *out
 );
 
 /**
  * Get bool value from record
  */
 void json_get_bool(
     const  json_decoder_ctx_t *ctx,
     uint32_t record_idx,
     bool *out
 );
 
 /**
  * Get string value from record
  */
 void json_get_string_value(
     const  json_decoder_ctx_t *ctx,
     uint32_t record_idx,
     const char **out
 );
 
 /**
  * Check if record is null
  */
 bool json_is_null(
     const  json_decoder_ctx_t *ctx,
     uint32_t record_idx
 );
 
 /* ============================================================================
  * Utility Functions
  * ============================================================================ */
 
 /**
  * Get number of children in container (OBJECT or ARRAY)
  */
 void json_get_child_count(
     const  json_decoder_ctx_t *ctx,
     uint32_t record_idx,
     uint32_t *out_count
 );
 
 /**
  * Get type of record
  */
 static inline json_type_t json_get_type(
     const  json_decoder_ctx_t *ctx,
     uint32_t record_idx)
 {
     const json_record_t *record = json_get_record(ctx, record_idx);
     return record ? record->object_type : JSON_TYPE_NULL;
 }
 
 /**
  * Validate record structure (for debugging)
  */
 void json_validate_records(
     const  json_decoder_ctx_t *ctx,
     uint32_t control_idx
 );
 
 /* Array length extraction */
void json_extract_array_length(
    const json_decoder_ctx_t *ctx,
    uint32_t root_record,
    const char *path,
    uint32_t *out_length);

void json_extract_array_length_runtime(
    const cfl_runtime_handle_t *runtime,
    const char *path,
    uint32_t *out_length);

/* Array element extraction */
void json_extract_array_int32(
    const json_decoder_ctx_t *ctx,
    uint32_t root_record,
    const char *path,
    uint32_t index,
    int32_t *out);

void json_extract_array_int32_runtime(
    const cfl_runtime_handle_t *runtime,
    const char *path,
    uint32_t index,
    int32_t *out);

void json_navigate_path_runtime(
        const cfl_runtime_handle_t *runtime,
        const char *path,
        uint32_t *out_record);

 /* ============================================================================
  * Debug Functions
  * ============================================================================ */
 
 /* ============================================================================
 * Debug Functions
 * ============================================================================ */

 #define JSON_DEBUG 1
 #ifdef JSON_DEBUG
const char *json_type_to_string(json_type_t type);

void json_print_record(
    const  json_decoder_ctx_t *ctx,
    uint32_t record_idx,
    int indent_level
);

void json_print_control_region(
    const  json_decoder_ctx_t *ctx,
    uint32_t control_idx
);

/**
 * Print JSON structure starting from node_data_id
 */
void json_print_node_data(
    const  json_decoder_ctx_t *ctx,
    uint32_t node_data_id
);

/**
 * Print JSON structure for a specific node using runtime handle
 */
void json_print_node_data_runtime(
    const cfl_runtime_handle_t *runtime,
    uint32_t node_index
);

/**
 * Print JSON structure for currently active node
 */
void json_print_current_node_data(
    const cfl_runtime_handle_t *runtime
);
#endif
 
 #ifdef __cplusplus
 }
 #endif
 
 #endif /* JSON_NODE_DECODER_H */

