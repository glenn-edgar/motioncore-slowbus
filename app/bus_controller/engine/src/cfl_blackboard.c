/**
 * @file cfl_blackboard.c
 * @brief Shared Blackboard implementation for ChainTree Runtime
 */

#include <string.h>
#include "cfl_blackboard.h"
#include "cfl_engine.h"
#include "cfl_exception.h"

/* ========================================================================
 * INTERNAL: Find field in descriptor array
 * ======================================================================== */

static const cfl_bb_field_t *find_field(
    const cfl_bb_field_t *fields, uint16_t field_count, uint32_t field_hash)
{
    for (uint16_t i = 0; i < field_count; i++) {
        if (fields[i].name_hash == field_hash) {
            return &fields[i];
        }
    }
    return NULL;
}

/* ========================================================================
 * LIFECYCLE
 * ======================================================================== */

bool cfl_bb_init(cfl_runtime_handle_t *handle) {
    if (!handle) {
        EXCEPTION("cfl_bb_init: NULL handle");
        return false;
    }

    const cfl_bb_table_t *bb_table = handle->flash_handle->bb_table;
    if (!bb_table || !bb_table->blackboard) {
        /* No blackboard defined — not an error */
        handle->blackboard = NULL;
        handle->blackboard_size = 0;
        handle->bb_desc = NULL;
        return true;
    }

    const cfl_bb_record_t *desc = bb_table->blackboard;
    if (desc->total_size == 0) {
        handle->blackboard = NULL;
        handle->blackboard_size = 0;
        handle->bb_desc = desc;
        return true;
    }

    handle->blackboard = cfl_perm_alloc_pointer(
        handle->perm, desc->total_size);
    if (!handle->blackboard) {
        EXCEPTION("cfl_bb_init: failed to allocate blackboard");
        return false;
    }

    handle->blackboard_size = desc->total_size;
    handle->bb_desc = desc;

    if (desc->defaults) {
        memcpy(handle->blackboard, desc->defaults, desc->total_size);
    } else {
        memset(handle->blackboard, 0, desc->total_size);
    }

    return true;
}

void cfl_bb_reset(cfl_runtime_handle_t *handle) {
    if (!handle || !handle->blackboard || handle->blackboard_size == 0) {
        return;
    }

    if (handle->bb_desc && handle->bb_desc->defaults) {
        memcpy(handle->blackboard, handle->bb_desc->defaults,
               handle->blackboard_size);
    } else {
        memset(handle->blackboard, 0, handle->blackboard_size);
    }
}

/* ========================================================================
 * DYNAMIC FIELD ACCESS
 * ======================================================================== */

void *cfl_bb_field_by_hash(cfl_runtime_handle_t *handle, uint32_t field_hash) {
    if (!handle || !handle->blackboard || !handle->bb_desc) {
        return NULL;
    }

    const cfl_bb_field_t *field = find_field(
        handle->bb_desc->fields, handle->bb_desc->field_count, field_hash);
    if (!field) {
        return NULL;
    }

    if (field->offset + field->size > handle->blackboard_size) {
        EXCEPTION("cfl_bb_field_by_hash: field exceeds blackboard size");
        return NULL;
    }

    return (uint8_t *)handle->blackboard + field->offset;
}

void *cfl_bb_field_by_name(cfl_runtime_handle_t *handle, const char *field_name) {
    if (!field_name) {
        return NULL;
    }
    return cfl_bb_field_by_hash(handle, cfl_bb_hash(field_name));
}

/* ========================================================================
 * TYPED GETTERS / SETTERS
 * ======================================================================== */

bool cfl_bb_set_int32(cfl_runtime_handle_t *handle, uint32_t field_hash, int32_t value) {
    int32_t *ptr = (int32_t *)cfl_bb_field_by_hash(handle, field_hash);
    if (!ptr) return false;
    *ptr = value;
    return true;
}

int32_t cfl_bb_get_int32(cfl_runtime_handle_t *handle, uint32_t field_hash, int32_t default_val) {
    int32_t *ptr = (int32_t *)cfl_bb_field_by_hash(handle, field_hash);
    return ptr ? *ptr : default_val;
}

bool cfl_bb_set_float(cfl_runtime_handle_t *handle, uint32_t field_hash, float value) {
    float *ptr = (float *)cfl_bb_field_by_hash(handle, field_hash);
    if (!ptr) return false;
    *ptr = value;
    return true;
}

float cfl_bb_get_float(cfl_runtime_handle_t *handle, uint32_t field_hash, float default_val) {
    float *ptr = (float *)cfl_bb_field_by_hash(handle, field_hash);
    return ptr ? *ptr : default_val;
}

bool cfl_bb_set_uint32(cfl_runtime_handle_t *handle, uint32_t field_hash, uint32_t value) {
    uint32_t *ptr = (uint32_t *)cfl_bb_field_by_hash(handle, field_hash);
    if (!ptr) return false;
    *ptr = value;
    return true;
}

uint32_t cfl_bb_get_uint32(cfl_runtime_handle_t *handle, uint32_t field_hash, uint32_t default_val) {
    uint32_t *ptr = (uint32_t *)cfl_bb_field_by_hash(handle, field_hash);
    return ptr ? *ptr : default_val;
}

bool cfl_bb_set_uint16(cfl_runtime_handle_t *handle, uint32_t field_hash, uint16_t value) {
    uint16_t *ptr = (uint16_t *)cfl_bb_field_by_hash(handle, field_hash);
    if (!ptr) return false;
    *ptr = value;
    return true;
}

uint16_t cfl_bb_get_uint16(cfl_runtime_handle_t *handle, uint32_t field_hash, uint16_t default_val) {
    uint16_t *ptr = (uint16_t *)cfl_bb_field_by_hash(handle, field_hash);
    return ptr ? *ptr : default_val;
}

/* ========================================================================
 * CONSTANT RECORD LOOKUP
 * ======================================================================== */

const cfl_bb_const_record_t *cfl_bb_const_find(
    cfl_runtime_handle_t *handle, uint32_t record_hash)
{
    if (!handle || !handle->flash_handle->bb_table) {
        return NULL;
    }

    const cfl_bb_table_t *table = handle->flash_handle->bb_table;
    for (uint16_t i = 0; i < table->const_record_count; i++) {
        if (table->const_records[i].name_hash == record_hash) {
            return &table->const_records[i];
        }
    }
    return NULL;
}

const void *cfl_bb_const_field_by_hash(
    const cfl_bb_const_record_t *record, uint32_t field_hash)
{
    if (!record || !record->fields || !record->data) {
        return NULL;
    }

    const cfl_bb_field_t *field = find_field(
        record->fields, record->field_count, field_hash);
    if (!field) {
        return NULL;
    }

    if (field->offset + field->size > record->total_size) {
        return NULL;
    }

    return (const uint8_t *)record->data + field->offset;
}
