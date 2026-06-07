/* ============================================================================
 * json_node_decoder.c - Implementation
 * ============================================================================ */
 #include "cfl_global_definitions.h"
 #include "json_node_decoder.h"
 #include <string.h>
 #include <stdio.h>
 #include <ctype.h>
 
 /* ============================================================================
  * Core Decoder Functions Implementation
  * ============================================================================ */
 
 void json_decoder_init(
      json_decoder_ctx_t *ctx,
      const chaintree_handle_t *flash_handle,
     uint32_t node_index)
 {
     if (!ctx) {
         EXCEPTION("json_decoder_init: NULL context");
     }
     
     if (!flash_handle) {
         EXCEPTION("json_decoder_init: NULL flash_handle");
     }
     
     if (node_index >= flash_handle->node_count) {
         EXCEPTION("json_decoder_init: node_count out of bounds");
     }
     const chaintree_node_t *node = &flash_handle->nodes[node_index];
     uint32_t node_data_control_index = node->node_data_id;
     
     ctx->records = flash_handle->node_data_records;
     ctx->records_count = flash_handle->node_data_records_count;
     ctx->strings = flash_handle->node_data_strings;
     ctx->strings_size = flash_handle->node_data_strings_size;
     ctx->controls = flash_handle->node_data_controls;
     ctx->controls_count = flash_handle->node_data_controls_count;
     ctx->current_control_idx = node_data_control_index;
 }
 
 /* ============================================================================
  * Runtime Handle Integration Implementation
  * ============================================================================ */
 
 void json_decoder_init_from_runtime(
     cfl_runtime_handle_t *runtime,
     uint32_t node_id)
 {
     if (!runtime) {
         EXCEPTION("json_decoder_init_from_runtime: NULL runtime handle");
     }
     
     if (!runtime->json_decoder_ctx) {
         EXCEPTION("json_decoder_init_from_runtime: NULL json_decoder_ctx");
     }
     
     if (!runtime->flash_handle) {
         EXCEPTION("json_decoder_init_from_runtime: NULL flash_handle");
     }
     
     json_decoder_init(
         runtime->json_decoder_ctx,
         runtime->flash_handle,
         node_id
     );
 }
 
 void json_extract_int32_runtime(
     const cfl_runtime_handle_t *runtime,
     const char *path,
     int32_t *out)
 {
     if (!runtime) {
         EXCEPTION("json_extract_int32_runtime: NULL runtime handle");
     }
     
     if (!runtime->json_decoder_ctx) {
         EXCEPTION("json_extract_int32_runtime: NULL json_decoder_ctx");
     }
     
     if (!runtime->flash_handle) {
         EXCEPTION("json_extract_int32_runtime: NULL flash_handle");
     }
     
     if (!path) {
         EXCEPTION("json_extract_int32_runtime: NULL path");
     }
     
     if (!out) {
         EXCEPTION("json_extract_int32_runtime: NULL output pointer");
     }
     
      const json_decoder_ctx_t *ctx = runtime->json_decoder_ctx;
     
     if (ctx->current_control_idx >= ctx->controls_count) {
         EXCEPTION("json_extract_int32_runtime: Invalid control index");
     }
     
     const record_control_t *region = &ctx->controls[ctx->current_control_idx];
     
     json_extract_int32(ctx, region->start_position, path, out);
 }
 
 void json_extract_float32_runtime(
     const cfl_runtime_handle_t *runtime,
     const char *path,
     float *out)
 {
     if (!runtime) {
         EXCEPTION("json_extract_float32_runtime: NULL runtime handle");
     }
     
     if (!runtime->json_decoder_ctx) {
         EXCEPTION("json_extract_float32_runtime: NULL json_decoder_ctx");
     }
     
     if (!runtime->flash_handle) {
         EXCEPTION("json_extract_float32_runtime: NULL flash_handle");
     }
     
     if (!path) {
         EXCEPTION("json_extract_float32_runtime: NULL path");
     }
     
     if (!out) {
         EXCEPTION("json_extract_float32_runtime: NULL output pointer");
     }
     
      const json_decoder_ctx_t *ctx = runtime->json_decoder_ctx;
     
     if (ctx->current_control_idx >= ctx->controls_count) {
         EXCEPTION("json_extract_float32_runtime: Invalid control index");
     }
     
     const record_control_t *region = &ctx->controls[ctx->current_control_idx];
     
     json_extract_float32(ctx, region->start_position, path, out);
 }
 
 void json_extract_bool_runtime(
     const cfl_runtime_handle_t *runtime,
     const char *path,
     bool *out)
 {
     if (!runtime) {
         EXCEPTION("json_extract_bool_runtime: NULL runtime handle");
     }
     
     if (!runtime->json_decoder_ctx) {
         EXCEPTION("json_extract_bool_runtime: NULL json_decoder_ctx");
     }
     
     if (!runtime->flash_handle) {
         EXCEPTION("json_extract_bool_runtime: NULL flash_handle");
     }
     
     if (!path) {
         EXCEPTION("json_extract_bool_runtime: NULL path");
     }
     
     if (!out) {
         EXCEPTION("json_extract_bool_runtime: NULL output pointer");
     }
     
      const json_decoder_ctx_t *ctx = runtime->json_decoder_ctx;
     
     if (ctx->current_control_idx >= ctx->controls_count) {
         EXCEPTION("json_extract_bool_runtime: Invalid control index");
     }
     
     const record_control_t *region = &ctx->controls[ctx->current_control_idx];
     
     json_extract_bool(ctx, region->start_position, path, out);
 }
 
 void json_extract_string_runtime(
     const cfl_runtime_handle_t *runtime,
     const char *path,
     const char **out)
 {
     if (!runtime) {
         EXCEPTION("json_extract_string_runtime: NULL runtime handle");
     }
     
     if (!runtime->json_decoder_ctx) {
         EXCEPTION("json_extract_string_runtime: NULL json_decoder_ctx");
     }
     
     if (!runtime->flash_handle) {
         EXCEPTION("json_extract_string_runtime: NULL flash_handle");
     }
     
     if (!path) {
         EXCEPTION("json_extract_string_runtime: NULL path");
     }
     
     if (!out) {
         EXCEPTION("json_extract_string_runtime: NULL output pointer");
     }
     
      const json_decoder_ctx_t *ctx = runtime->json_decoder_ctx;
     
     if (ctx->current_control_idx >= ctx->controls_count) {
         EXCEPTION("json_extract_string_runtime: Invalid control index");
     }
     
     const record_control_t *region = &ctx->controls[ctx->current_control_idx];
     
     json_extract_string(ctx, region->start_position, path, out);
 }
 
 /* ============================================================================
  * Low-Level Navigation Functions Implementation
  * ============================================================================ */
 
/**
 * Calculate subtree size (needed for navigation)
 */
 static uint32_t json_calc_subtree_size(
    const  json_decoder_ctx_t *ctx,
    uint32_t record_idx)
{
    const json_record_t *record = json_get_record(ctx, record_idx);
    if (!record) {
        return 1;
    }
    
    uint32_t size = 1; // Count this record
    
    if (record->object_type == JSON_TYPE_OBJECT || record->object_type == JSON_TYPE_ARRAY) {
        uint32_t child_idx = record_idx + 1;
        
        if (record->object_type == JSON_TYPE_OBJECT) {
            // For objects: iterate through key-value pairs
            // container_count is total children (keys + values)
            for (uint32_t i = 0; i < record->value.container_count; i += 2) {
                // Key (always 1 record)
                size += 1;
                child_idx += 1;
                
                // Value (recursive)
                uint32_t value_size = json_calc_subtree_size(ctx, child_idx);
                size += value_size;
                child_idx += value_size;
            }
        } else {
            // For arrays: iterate through elements
            for (uint32_t i = 0; i < record->value.container_count; i++) {
                uint32_t elem_size = json_calc_subtree_size(ctx, child_idx);
                size += elem_size;
                child_idx += elem_size;
            }
        }
    }
    
    return size;
}

void json_find_object_child(
    const  json_decoder_ctx_t *ctx,
    uint32_t parent_record,
    const char *key,
    uint32_t *out_record)
{
    if (!ctx) {
        EXCEPTION("json_find_object_child: NULL context");
    }
    
    if (!key) {
        EXCEPTION("json_find_object_child: NULL key");
    }
    
    if (!out_record) {
        EXCEPTION("json_find_object_child: NULL output pointer");
    }
    
    const json_record_t *parent = json_get_record(ctx, parent_record);
    if (!parent) {
        EXCEPTION("json_find_object_child: Invalid parent record");
    }
    
    if (parent->object_type != JSON_TYPE_OBJECT) {
        EXCEPTION("json_find_object_child: Parent is not an object");
    }
    
    uint32_t container_count = parent->value.container_count;
    uint32_t child_idx = parent_record + 1;
    
    // container_count is total children (keys + values)
    // Iterate through key-value pairs (i += 2)
    for (uint32_t i = 0; i < container_count; i += 2) {
        // Get key record
        const json_record_t *key_record = json_get_record(ctx, child_idx);
        if (!key_record || key_record->object_type != JSON_TYPE_STRING) {
            // Skip malformed pair
            child_idx += 1; // Skip key
            if (i + 1 < container_count) {
                uint32_t val_size = json_calc_subtree_size(ctx, child_idx);
                child_idx += val_size; // Skip value
            }
            continue;
        }
        
        // Check if this is the key we're looking for
        const char *key_str = json_get_string(ctx, key_record->value.string_offset);
        if (key_str && strcmp(key_str, key) == 0) {
            // Found it! Return the value record index
            *out_record = child_idx + 1;
            return;
        }
        
        // Not the right key, skip to next pair
        child_idx += 1; // Skip key (always 1 record)
        
        // Skip value (may be multiple records if nested)
        uint32_t value_size = json_calc_subtree_size(ctx, child_idx);
        child_idx += value_size;
    }
    
    EXCEPTION("json_find_object_child: Key not found");
}
 
void json_get_array_child(
    const json_decoder_ctx_t *ctx,
    uint32_t parent_record,
    uint32_t index,
    uint32_t *out_record)
{
    if (!ctx) {
        EXCEPTION("json_get_array_child: NULL context");
    }
    
    if (!out_record) {
        EXCEPTION("json_get_array_child: NULL output pointer");
    }
    
    const json_record_t *parent = json_get_record(ctx, parent_record);
    if (!parent) {
        EXCEPTION("json_get_array_child: Invalid parent record");
    }
    
    if (parent->object_type != JSON_TYPE_ARRAY) {
        EXCEPTION("json_get_array_child: Parent is not an array");
    }
    
    if (index >= parent->value.container_count) {
        EXCEPTION("json_get_array_child: Array index out of bounds");
    }
    
    // Navigate to the correct element by skipping over preceding elements
    uint32_t child_idx = parent_record + 1;
    for (uint32_t i = 0; i < index; i++) {
        uint32_t elem_size = json_calc_subtree_size(ctx, child_idx);
        child_idx += elem_size;
    }
    
    *out_record = child_idx;
}
 
 /* ============================================================================
  * Path Navigation Implementation
  * ============================================================================ */
 
 /**
  * Parse next path component from path string
  * Handles both "key" and "array[index]" syntax
  * 
  * @param path Current position in path
  * @param key_buf Output buffer for key name
  * @param key_buf_size Size of key buffer
  * @param array_index Output for array index (-1 if not array access)
  * @param next_path Output pointer to rest of path (after '.' or end)
  */
 static void parse_path_component(
     const char *path,
     char *key_buf,
     size_t key_buf_size,
     int32_t *array_index,
     const char **next_path)
 {
     if (!path || !key_buf || !array_index || !next_path) {
         EXCEPTION("parse_path_component: NULL parameter");
     }
     
     *array_index = -1;
     const char *p = path;
     size_t i = 0;
     
     // Copy key name until '.', '[', or end
     while (*p && *p != '.' && *p != '[' && i < key_buf_size - 1) {
         key_buf[i++] = *p++;
     }
     key_buf[i] = '\0';
     
     // Check for array index
     if (*p == '[') {
         p++; // Skip '['
         *array_index = 0;
         while (isdigit(*p)) {
             *array_index = (*array_index * 10) + (*p - '0');
             p++;
         }
         if (*p == ']') {
             p++; // Skip ']'
         }
     }
     
     // Skip '.' if present
     if (*p == '.') {
         p++;
     }
     
     *next_path = p;
 }
 
 void json_navigate_path(
     const  json_decoder_ctx_t *ctx,
     uint32_t root_record,
     const char *path,
     uint32_t *out_record)
 {
     if (!ctx) {
         EXCEPTION("json_navigate_path: NULL context");
     }
     
     if (!path) {
         EXCEPTION("json_navigate_path: NULL path");
     }
     
     if (!out_record) {
         EXCEPTION("json_navigate_path: NULL output pointer");
     }
     
     uint32_t current_record = root_record;
     const char *current_path = path;
     char key_buf[64];
     
     while (*current_path) {
         int32_t array_index;
         const char *next_path;
         
         parse_path_component(current_path, key_buf, sizeof(key_buf),
                            &array_index, &next_path);
         
         // If we have a key name, navigate into object
         if (key_buf[0] != '\0') {
             json_find_object_child(ctx, current_record, key_buf, &current_record);
         }
         
         // If we have an array index, navigate into array
         if (array_index >= 0) {
             json_get_array_child(ctx, current_record, (uint32_t)array_index,
                                &current_record);
         }
         
         current_path = next_path;
     }
     
     *out_record = current_record;
 }
 
 /* ============================================================================
  * Type-Safe Value Getters Implementation
  * ============================================================================ */
 
 void json_get_int32(
     const  json_decoder_ctx_t *ctx,
     uint32_t record_idx,
     int32_t *out)
 {
     if (!ctx) {
         EXCEPTION("json_get_int32: NULL context");
     }
     
     if (!out) {
         EXCEPTION("json_get_int32: NULL output pointer");
     }
     
     const json_record_t *record = json_get_record(ctx, record_idx);
     if (!record) {
         EXCEPTION("json_get_int32: Invalid record index");
     }
     
     // Accept both INT32 and FLOAT32, converting as needed
     if (record->object_type == JSON_TYPE_INT32) {
         *out = record->value.i32_value;
     } else if (record->object_type == JSON_TYPE_FLOAT32) {
         // Convert float to int32 using truncation (toward zero)
         *out = (int32_t)record->value.f32_value;
     } else {
        
         EXCEPTION("json_get_int32: Type mismatch - expected INT32 or FLOAT32");
     }
 }
 
 void json_get_float32(
     const  json_decoder_ctx_t *ctx,
     uint32_t record_idx,
     float *out)
 {
     if (!ctx) {
         EXCEPTION("json_get_float32: NULL context");
     }
     
     if (!out) {
         EXCEPTION("json_get_float32: NULL output pointer");
     }
     
     const json_record_t *record = json_get_record(ctx, record_idx);
     if (!record) {
         EXCEPTION("json_get_float32: Invalid record index");
     }
     
     // Accept both FLOAT32 and INT32, converting as needed
     if (record->object_type == JSON_TYPE_FLOAT32) {
         *out = record->value.f32_value;
     } else if (record->object_type == JSON_TYPE_INT32) {
         // Convert int32 to float
         *out = (float)record->value.i32_value;
     } else {
         EXCEPTION("json_get_float32: Type mismatch - expected FLOAT32 or INT32");
     }
 }
 
 void json_get_bool(
     const  json_decoder_ctx_t *ctx,
     uint32_t record_idx,
     bool *out)
 {
     if (!ctx) {
         EXCEPTION("json_get_bool: NULL context");
     }
     
     if (!out) {
         EXCEPTION("json_get_bool: NULL output pointer");
     }
     
     const json_record_t *record = json_get_record(ctx, record_idx);
     if (!record) {
         EXCEPTION("json_get_bool: Invalid record index");
     }
     
     if (record->object_type != JSON_TYPE_BOOL) {
         EXCEPTION("json_get_bool: Type mismatch - expected BOOL");
     }
     
     *out = (record->value.bool_value != 0);
 }
 
 void json_get_string_value(
     const  json_decoder_ctx_t *ctx,
     uint32_t record_idx,
     const char **out)
 {
     if (!ctx) {
         EXCEPTION("json_get_string_value: NULL context");
     }
     
     if (!out) {
         EXCEPTION("json_get_string_value: NULL output pointer");
     }
     
     const json_record_t *record = json_get_record(ctx, record_idx);
     if (!record) {
         EXCEPTION("json_get_string_value: Invalid record index");
     }
     
     if (record->object_type != JSON_TYPE_STRING) {
         EXCEPTION("json_get_string_value: Type mismatch - expected STRING");
     }
     
     *out = json_get_string(ctx, record->value.string_offset);
     if (!*out) {
         EXCEPTION("json_get_string_value: Invalid string offset");
     }
 }
 
 bool json_is_null(
     const  json_decoder_ctx_t *ctx,
     uint32_t record_idx)
 {
     if (!ctx) {
         EXCEPTION("json_is_null: NULL context");
     }
     
     const json_record_t *record = json_get_record(ctx, record_idx);
     return record && (record->object_type == JSON_TYPE_NULL);
 }
 
 /* ============================================================================
  * Path-Based Extraction API Implementation
  * ============================================================================ */
 
 void json_extract_int32(
     const  json_decoder_ctx_t *ctx,
     uint32_t root_record,
     const char *path,
     int32_t *out)
 {
     uint32_t target_record;
     json_navigate_path(ctx, root_record, path, &target_record);
     json_get_int32(ctx, target_record, out);
 }
 
 void json_extract_float32(
     const  json_decoder_ctx_t *ctx,
     uint32_t root_record,
     const char *path,
     float *out)
 {
     uint32_t target_record;
     json_navigate_path(ctx, root_record, path, &target_record);
     json_get_float32(ctx, target_record, out);
 }
 
 void json_extract_bool(
     const  json_decoder_ctx_t *ctx,
     uint32_t root_record,
     const char *path,
     bool *out)
 {
     uint32_t target_record;
     json_navigate_path(ctx, root_record, path, &target_record);
     json_get_bool(ctx, target_record, out);
 }
 
 void json_extract_string(
     const  json_decoder_ctx_t *ctx,
     uint32_t root_record,
     const char *path,
     const char **out)
 {
     uint32_t target_record;
     json_navigate_path(ctx, root_record, path, &target_record);
     json_get_string_value(ctx, target_record, out);
 }
 
 /* ============================================================================
  * Utility Functions Implementation
  * ============================================================================ */
 
 void json_get_child_count(
     const  json_decoder_ctx_t *ctx,
     uint32_t record_idx,
     uint32_t *out_count)
 {
     if (!ctx) {
         EXCEPTION("json_get_child_count: NULL context");
     }
     
     if (!out_count) {
         EXCEPTION("json_get_child_count: NULL output pointer");
     }
     
     const json_record_t *record = json_get_record(ctx, record_idx);
     if (!record) {
         EXCEPTION("json_get_child_count: Invalid record index");
     }
     
     if (record->object_type != JSON_TYPE_ARRAY && 
         record->object_type != JSON_TYPE_OBJECT) {
         EXCEPTION("json_get_child_count: Record is not a container type");
     }
     
     *out_count = record->value.container_count;
 }
 
 void json_validate_records(
     const  json_decoder_ctx_t *ctx,
     uint32_t control_idx)
 {
     if (!ctx || !ctx->records || !ctx->controls) {
         EXCEPTION("json_validate_records: NULL context or data");
     }
     
     if (control_idx >= ctx->controls_count) {
         EXCEPTION("json_validate_records: Invalid control index");
     }
     
     const record_control_t *control = &ctx->controls[control_idx];
     
     uint32_t start = control->start_position;
     uint32_t end = start + control->num_records;
     
     if (end > ctx->records_count) {
         EXCEPTION("json_validate_records: Control region extends beyond records array");
     }
     
     for (uint32_t i = start; i < end; i++) {
         const json_record_t *record = &ctx->records[i];
         
         // Validate type
         if (record->object_type > JSON_TYPE_OBJECT) {
             EXCEPTION("json_validate_records: Invalid record type");
         }
         
         // Validate string offsets
         if (record->object_type == JSON_TYPE_STRING) {
             if (record->value.string_offset >= ctx->strings_size) {
                 EXCEPTION("json_validate_records: String offset out of bounds");
             }
         }
         
         // Validate container bounds
         if (record->object_type == JSON_TYPE_OBJECT || 
             record->object_type == JSON_TYPE_ARRAY) {
             uint32_t children_end = i + 1 + record->value.container_count;
             if (children_end > end) {
                 EXCEPTION("json_validate_records: Container children extend beyond control region");
             }
         }
     }
 }
 
 /* ============================================================================
  * Debug Functions Implementation
  * ============================================================================ */
 
 /* ============================================================================
 * Enhanced Debug Functions for Node Data Printing
 * ============================================================================ */

/* ============================================================================
 * Debug Functions Implementation
 * ============================================================================ */

 /* ============================================================================
 * Debug Functions Implementation
 * ============================================================================ */

/* ============================================================================
 * Debug Functions Implementation
 * ============================================================================ */

 /* ============================================================================
 * Debug Functions Implementation
 * ============================================================================ */

 /* ============================================================================
 * Array Length and Element Extraction Functions
 * ============================================================================ */

void json_extract_array_length(
    const json_decoder_ctx_t *ctx,
    uint32_t root_record,
    const char *path,
    uint32_t *out_length)
{
    if (!ctx) {
        EXCEPTION("json_extract_array_length: NULL context");
    }
    
    if (!path) {
        EXCEPTION("json_extract_array_length: NULL path");
    }
    
    if (!out_length) {
        EXCEPTION("json_extract_array_length: NULL output pointer");
    }
    
    uint32_t target_record;
    json_navigate_path(ctx, root_record, path, &target_record);
    
    const json_record_t *record = json_get_record(ctx, target_record);
    if (!record) {
        EXCEPTION("json_extract_array_length: Invalid target record");
    }
    
    if (record->object_type != JSON_TYPE_ARRAY) {
        EXCEPTION("json_extract_array_length: Target is not an array");
    }
    
    *out_length = record->value.container_count;
}

void json_extract_array_int32(
    const json_decoder_ctx_t *ctx,
    uint32_t root_record,
    const char *path,
    uint32_t index,
    int32_t *out)
{
    if (!ctx) {
        EXCEPTION("json_extract_array_int32: NULL context");
    }
    
    if (!path) {
        EXCEPTION("json_extract_array_int32: NULL path");
    }
    
    if (!out) {
        EXCEPTION("json_extract_array_int32: NULL output pointer");
    }
    
    uint32_t array_record;
    json_navigate_path(ctx, root_record, path, &array_record);
    
    uint32_t element_record;
    json_get_array_child(ctx, array_record, index, &element_record);
    
    json_get_int32(ctx, element_record, out);
}

/* ============================================================================
 * Runtime Variants
 * ============================================================================ */

void json_extract_array_length_runtime(
    const cfl_runtime_handle_t *runtime,
    const char *path,
    uint32_t *out_length)
{
    if (!runtime) {
        EXCEPTION("json_extract_array_length_runtime: NULL runtime handle");
    }
    
    if (!runtime->json_decoder_ctx) {
        EXCEPTION("json_extract_array_length_runtime: NULL json_decoder_ctx");
    }
    
    if (!path) {
        EXCEPTION("json_extract_array_length_runtime: NULL path");
    }
    
    if (!out_length) {
        EXCEPTION("json_extract_array_length_runtime: NULL output pointer");
    }
    
    const json_decoder_ctx_t *ctx = runtime->json_decoder_ctx;
    
    if (ctx->current_control_idx >= ctx->controls_count) {
        EXCEPTION("json_extract_array_length_runtime: Invalid control index");
    }
    
    const record_control_t *region = &ctx->controls[ctx->current_control_idx];
    
    json_extract_array_length(ctx, region->start_position, path, out_length);
}

void json_extract_array_int32_runtime(
    const cfl_runtime_handle_t *runtime,
    const char *path,
    uint32_t index,
    int32_t *out)
{
    if (!runtime) {
        EXCEPTION("json_extract_array_int32_runtime: NULL runtime handle");
    }
    
    if (!runtime->json_decoder_ctx) {
        EXCEPTION("json_extract_array_int32_runtime: NULL json_decoder_ctx");
    }
    
    if (!path) {
        EXCEPTION("json_extract_array_int32_runtime: NULL path");
    }
    
    if (!out) {
        EXCEPTION("json_extract_array_int32_runtime: NULL output pointer");
    }
    
    const json_decoder_ctx_t *ctx = runtime->json_decoder_ctx;
    
    if (ctx->current_control_idx >= ctx->controls_count) {
        EXCEPTION("json_extract_array_int32_runtime: Invalid control index");
    }
    
    const record_control_t *region = &ctx->controls[ctx->current_control_idx];
    
    json_extract_array_int32(ctx, region->start_position, path, index, out);
}

void json_navigate_path_runtime(
    const cfl_runtime_handle_t *runtime,
    const char *path,
    uint32_t *out_record)
{
    if (!runtime) {
        EXCEPTION("json_navigate_path_runtime: NULL runtime handle");
    }
    
    if (!runtime->json_decoder_ctx) {
        EXCEPTION("json_navigate_path_runtime: NULL json_decoder_ctx");
    }
    
    if (!path) {
        EXCEPTION("json_navigate_path_runtime: NULL path");
    }
    
    if (!out_record) {
        EXCEPTION("json_navigate_path_runtime: NULL output pointer");
    }
    
    const json_decoder_ctx_t *ctx = runtime->json_decoder_ctx;
    
    if (ctx->current_control_idx >= ctx->controls_count) {
        EXCEPTION("json_navigate_path_runtime: Invalid control index");
    }
    
    const record_control_t *region = &ctx->controls[ctx->current_control_idx];
    
    json_navigate_path(ctx, region->start_position, path, out_record);
}

#ifdef JSON_DEBUG

/**
 * Calculate the total size (number of records) in a subtree
 * This is needed to properly skip over nested structures
 */
static uint32_t json_get_subtree_size(
    const  json_decoder_ctx_t *ctx,
    uint32_t record_idx)
{
    const json_record_t *record = json_get_record(ctx, record_idx);
    if (!record) {
        return 1;
    }
    
    uint32_t size = 1; // Count this record
    
    if (record->object_type == JSON_TYPE_OBJECT || record->object_type == JSON_TYPE_ARRAY) {
        // For containers, all children follow sequentially
        uint32_t child_idx = record_idx + 1;
        
        if (record->object_type == JSON_TYPE_OBJECT) {
            // For objects: count is number of children (keys + values)
            // Iterate through pairs
            for (uint32_t i = 0; i < record->value.container_count; i += 2) {
                // Key (always 1 record)
                size += 1;
                child_idx += 1;
                
                // Value (recursive)
                uint32_t value_size = json_get_subtree_size(ctx, child_idx);
                size += value_size;
                child_idx += value_size;
            }
        } else {
            // For arrays: iterate through elements
            for (uint32_t i = 0; i < record->value.container_count; i++) {
                uint32_t elem_size = json_get_subtree_size(ctx, child_idx);
                size += elem_size;
                child_idx += elem_size;
            }
        }
    }
    
    return size;
}

const char *json_type_to_string(json_type_t type) {
    switch (type) {
        case JSON_TYPE_STRING:  return "string";
        case JSON_TYPE_INT32:   return "int32";
        case JSON_TYPE_FLOAT32: return "float32";
        case JSON_TYPE_NULL:    return "null";
        case JSON_TYPE_BOOL:    return "bool";
        case JSON_TYPE_ARRAY:   return "array";
        case JSON_TYPE_OBJECT:  return "object";
        default:                return "invalid";
    }
}

void json_print_record(
    const  json_decoder_ctx_t *ctx,
    uint32_t record_idx,
    int indent_level)
{
    const json_record_t *record = json_get_record(ctx, record_idx);
    if (!record) {
        printf("Invalid record %u\n", record_idx);
        return;
    }
    
    for (int i = 0; i < indent_level; i++) {
        printf("  ");
    }
    
    printf("[%u] %s: ", record_idx, json_type_to_string(record->object_type));
    
    switch (record->object_type) {
        case JSON_TYPE_NULL:
            printf("null\n");
            break;
            
        case JSON_TYPE_BOOL:
            printf("%s\n", record->value.bool_value ? "true" : "false");
            break;
            
        case JSON_TYPE_INT32:
            printf("%d\n", record->value.i32_value);
            break;
            
        case JSON_TYPE_FLOAT32:
            printf("%f\n", record->value.f32_value);
            break;
            
        case JSON_TYPE_STRING: {
            const char *str = json_get_string(ctx, record->value.string_offset);
            printf("\"%s\"\n", str ? str : "(null)");
            break;
        }
            
        case JSON_TYPE_OBJECT: {
            printf("{ count=%u\n", record->value.container_count);
            
            // Track actual record position, don't rely on loop counter
            uint32_t child_idx = record_idx + 1;
            
            // container_count is total number of children (keys + values)
            // Iterate in pairs: key, value, key, value...
            for (uint32_t i = 0; i < record->value.container_count; i += 2) {
                // Print indentation
                for (int j = 0; j < indent_level + 1; j++) printf("  ");
                
                // Print key
                const json_record_t *key_rec = json_get_record(ctx, child_idx);
                if (key_rec && key_rec->object_type == JSON_TYPE_STRING) {
                    const char *key_str = json_get_string(ctx, key_rec->value.string_offset);
                    if (key_str) {
                        printf("\"%s\": ", key_str);
                    } else {
                        printf("\"(null)\": ");
                    }
                } else {
                    printf("???: ");
                }
                child_idx += 1; // Keys are always single string records
                
                // Print value
                const json_record_t *val_rec = json_get_record(ctx, child_idx);
                if (val_rec) {
                    // For complex types, print on new line with indentation
                    if (val_rec->object_type == JSON_TYPE_OBJECT || 
                        val_rec->object_type == JSON_TYPE_ARRAY) {
                        printf("\n");
                        json_print_record(ctx, child_idx, indent_level + 1);
                    } else {
                        // For simple types, print inline
                        switch (val_rec->object_type) {
                            case JSON_TYPE_NULL:
                                printf("null\n");
                                break;
                            case JSON_TYPE_BOOL:
                                printf("%s\n", val_rec->value.bool_value ? "true" : "false");
                                break;
                            case JSON_TYPE_INT32:
                                printf("%d\n", val_rec->value.i32_value);
                                break;
                            case JSON_TYPE_FLOAT32:
                                printf("%f\n", val_rec->value.f32_value);
                                break;
                            case JSON_TYPE_STRING: {
                                const char *str = json_get_string(ctx, val_rec->value.string_offset);
                                printf("\"%s\"\n", str ? str : "(null)");
                                break;
                            }
                            default:
                                printf("???\n");
                                break;
                        }
                    }
                    
                    // Advance past the entire value subtree
                    uint32_t value_size = json_get_subtree_size(ctx, child_idx);
                    child_idx += value_size;
                } else {
                    printf("(invalid)\n");
                    child_idx += 1;
                }
            }
            
            // Closing brace
            for (int i = 0; i < indent_level; i++) printf("  ");
            printf("}\n");
            break;
        }
            
        case JSON_TYPE_ARRAY: {
            printf("[ count=%u\n", record->value.container_count);
            
            uint32_t elem_idx = record_idx + 1;
            for (uint32_t i = 0; i < record->value.container_count; i++) {
                json_print_record(ctx, elem_idx, indent_level + 1);
                uint32_t elem_size = json_get_subtree_size(ctx, elem_idx);
                elem_idx += elem_size;
            }
            
            // Closing bracket
            for (int i = 0; i < indent_level; i++) printf("  ");
            printf("]\n");
            break;
        }
    }
}

void json_print_control_region(
    const  json_decoder_ctx_t *ctx,
    uint32_t control_idx)
{
    if (!ctx) return;
    
    if (control_idx >= ctx->controls_count) {
        printf("Invalid control index %u\n", control_idx);
        return;
    }
    
    const record_control_t *control = &ctx->controls[control_idx];
    
    printf("=== Control Region %u ===\n", control_idx);
    printf("Start position: %u\n", control->start_position);
    printf("Num records: %u\n\n", control->num_records);
    
    json_print_record(ctx, control->start_position, 0);
}

void json_print_node_data(
    const  json_decoder_ctx_t *ctx,
    uint32_t node_data_id)
{
    if (!ctx) {
        printf("json_print_node_data: NULL context\n");
        return;
    }
    
    if (node_data_id >= ctx->controls_count) {
        printf("json_print_node_data: Invalid node_data_id %u (max %u)\n", 
               node_data_id, ctx->controls_count);
        return;
    }
    
    const record_control_t *control = &ctx->controls[node_data_id];
    
    printf("=== Node Data ID: %u ===\n", node_data_id);
    printf("Start position: %u\n", control->start_position);
    printf("Num records: %u\n", control->num_records);
    printf("\nJSON Structure:\n");
    
    json_print_record(ctx, control->start_position, 0);
}

void json_print_node_data_runtime(
    const cfl_runtime_handle_t *runtime,
    uint32_t node_index)
{
    if (!runtime) {
        printf("json_print_node_data_runtime: NULL runtime\n");
        return;
    }
    
    if (!runtime->json_decoder_ctx) {
        printf("json_print_node_data_runtime: NULL json_decoder_ctx\n");
        return;
    }
    
    if (!runtime->flash_handle) {
        printf("json_print_node_data_runtime: NULL flash_handle\n");
        return;
    }
    
    if (node_index >= runtime->flash_handle->node_count) {
        printf("json_print_node_data_runtime: Invalid node_index %u (max %u)\n",
               node_index, runtime->flash_handle->node_count);
        return;
    }

    const chaintree_node_t *node = &runtime->flash_handle->nodes[node_index];
    uint32_t node_data_id = node->node_data_id;
    
    printf("=== Node Index: %u, Node Data ID: %u ===\n", node_index, node_data_id);
    
    json_print_node_data(runtime->json_decoder_ctx, node_data_id);
}

void json_print_current_node_data(
    const cfl_runtime_handle_t *runtime)
{
    if (!runtime) {
        printf("json_print_current_node_data: NULL runtime\n");
        return;
    }
    
    if (!runtime->json_decoder_ctx) {
        printf("json_print_current_node_data: NULL json_decoder_ctx\n");
        return;
    }
    
    const  json_decoder_ctx_t *ctx = runtime->json_decoder_ctx;
    
    if (ctx->current_control_idx >= ctx->controls_count) {
        printf("json_print_current_node_data: Invalid current_control_idx %u\n",
               ctx->current_control_idx);
        return;
    }
    
    printf("=== Current Active Node Data ===\n");
    json_print_node_data(ctx, ctx->current_control_idx);
}

#endif /* JSON_DEBUG */

