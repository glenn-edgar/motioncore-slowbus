/**
 * @file cfl_event_queue.h
 * @brief Priority event queue system for ChainTree distributed control
 *
 * Provides a dual-priority event queue with variable allocation per priority level.
 * Designed for memory-constrained embedded systems (32KB RAM) through large servers.
 * Lock-free single-accessor per queue instance.
 */

 #ifndef CFL_EVENT_QUEUE_H
 #define CFL_EVENT_QUEUE_H
 
 #include <stdint.h>
 #include <stdbool.h>
 #include "cfl_perm.h"
 #include "cfl_exception.h"
 
 #ifdef __cplusplus
 extern "C" {
 #endif
 
 /*==============================================================================
  * Platform-Specific Types (CFL_64BIT / CFL_32BIT from cfl_global_definitions.h)
  *============================================================================*/
 
 #ifdef CFL_64BIT
     typedef uint64_t    cfl_size_t;
     typedef int64_t     cfl_int_t;
     typedef double      cfl_float_t;
 #else
     typedef uint32_t    cfl_size_t;
     typedef int32_t     cfl_int_t;
     typedef float       cfl_float_t;
 #endif
 
 /*==============================================================================
  * Constants and Macros
  *============================================================================*/
 
 #define CFL_EVENT_BROADCAST_NODE    0xFFFF
 #define CFL_EVENT_MALLOC_FLAG       0x01
 
 #define CFL_EVENT_PRIORITY_LOW      0
 #define CFL_EVENT_PRIORITY_HIGH     1
 
 typedef enum {
     CFL_EVENT_TYPE_PTR                        = 0,
     CFL_EVENT_TYPE_INT                        = 1,
     CFL_EVENT_TYPE_UINT                       = 2,
     CFL_EVENT_TYPE_FLOAT                      = 3,
     CFL_EVENT_TYPE_NODE_ID                    = 4,
     CFL_EVENT_TYPE_JSON_RECORD                = 5,
     CFL_EVENT_TYPE_STREAMING_DATA             = 6,
     CFL_EVENT_TYPE_STREAMING_COLLECTED_PACKETS = 7,
     CFL_EVENT_TYPE_NULL                       = 8
 } cfl_event_type_t;
 
 #define CFL_EVENT_QUEUE_MIN_SIZE    2
 
 /*==============================================================================
  * Data Types
  *============================================================================*/
 
 typedef union {
     void        *ptr;
     cfl_int_t    integer;
     cfl_size_t   unsigned_val;
     cfl_float_t  floating;
 } CFL_EVENT_VALUE_T;
 
 typedef struct {
     uint16_t            node_id;
     uint8_t             event_type;
     uint8_t             flags;
     uint16_t            event_id;
     uint16_t            queue_number;
     CFL_EVENT_VALUE_T   data;
 } CFL_EVENT_DATA_T;
 
 typedef struct {
     uint16_t            head;
     uint16_t            tail;
     uint16_t            capacity;
     uint16_t            mask;
     CFL_EVENT_DATA_T   *events;
 } CFL_EVENT_RING_T;
 
 typedef struct CFL_EVENT_QUEUE_T {
     CFL_EVENT_RING_T    high_priority;
     CFL_EVENT_RING_T    low_priority;
     uint16_t            queue_id;
     uint16_t            max_total_depth;
     uint16_t            max_high_depth;
     uint16_t            reserved;
 } CFL_EVENT_QUEUE_T, cfl_event_queue_t;
 
 /*==============================================================================
  * Core API
  *============================================================================*/
 
 CFL_EVENT_QUEUE_T *cfl_create_event_queue(
     unsigned high_priority_size,
     unsigned low_priority_size,
     cfl_perm_t *perm);
 
 void cfl_clear_queue(CFL_EVENT_QUEUE_T *queue_control);
 
 bool cfl_send_event(
     CFL_EVENT_QUEUE_T *queue_control,
     unsigned priority,
     unsigned node_id,
     unsigned event_type,
     bool malloc_flag,
     unsigned event_id,
     void *data);
 
 bool cfl_pop_event(
     CFL_EVENT_QUEUE_T *queue_control,
     CFL_EVENT_DATA_T *event_data);
 
 bool cfl_peek_event(
     CFL_EVENT_QUEUE_T *queue_control,
     CFL_EVENT_DATA_T *event_data);
 
 unsigned cfl_queue_number(CFL_EVENT_DATA_T *event_data);
 unsigned cfl_high_priority_count(CFL_EVENT_QUEUE_T *queue_control);
 unsigned cfl_low_priority_count(CFL_EVENT_QUEUE_T *queue_control);
 unsigned cfl_total_event_count(CFL_EVENT_QUEUE_T *queue_control);
 unsigned cfl_get_max_total_depth(CFL_EVENT_QUEUE_T *queue_control);
 unsigned cfl_get_max_high_depth(CFL_EVENT_QUEUE_T *queue_control);
 void cfl_reset_queue_stats(CFL_EVENT_QUEUE_T *queue_control);
 
 /*==============================================================================
  * Type-Specific Helper Functions
  *============================================================================*/
 
 bool cfl_send_unsigned_event(
     CFL_EVENT_QUEUE_T *queue_control,
     unsigned priority, unsigned node_id,
     unsigned event_id, cfl_size_t value);
 
 bool cfl_send_integer_event(
     CFL_EVENT_QUEUE_T *queue_control,
     unsigned priority, unsigned node_id,
     unsigned event_id, cfl_int_t value);
 
 bool cfl_send_float_event(
     CFL_EVENT_QUEUE_T *queue_control,
     unsigned priority, unsigned node_id,
     unsigned event_id, cfl_float_t value);
 
 bool cfl_send_data_event(
     CFL_EVENT_QUEUE_T *queue_control,
     unsigned priority, unsigned node_id,
     bool malloc_flag, unsigned event_id, void *data);
 
 bool cfl_send_null_event(
     CFL_EVENT_QUEUE_T *queue_control,
     unsigned priority, unsigned node_id,
     unsigned event_id);
 
 bool cfl_send_json_record_event(
     CFL_EVENT_QUEUE_T *queue_control,
     unsigned priority, unsigned node_id,
     unsigned event_id, uint32_t record_index);
 
 bool cfl_send_node_id_event(
     CFL_EVENT_QUEUE_T *queue_control,
     unsigned priority, unsigned node_id,
     unsigned event_id, unsigned node_index);
 
 bool cfl_send_streaming_data_event(
     CFL_EVENT_QUEUE_T *queue_control,
     unsigned priority, unsigned node_id,
     unsigned event_id, void *data);
 
 bool cfl_send_streaming_collected_packets_event(
     CFL_EVENT_QUEUE_T *queue_control,
     unsigned priority, unsigned node_id,
     unsigned event_id, void *data);
 
 #ifdef __cplusplus
 }
 #endif
 
 #endif /* CFL_EVENT_QUEUE_H */