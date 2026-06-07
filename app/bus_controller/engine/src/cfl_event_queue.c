/**
 * @file cfl_event_queue.c
 * @brief Implementation of priority event queue system
 */

 #include "cfl_event_queue.h"
 #include "cfl_exception.h"
 #include <string.h>
 #include <stdlib.h>
 #include <stdio.h>
 #include <stdint.h>
 
 /*==============================================================================
  * Global Variables
  *============================================================================*/
 
 /** Next queue ID to assign (increments for each created queue) */
 static uint16_t g_next_queue_id = 0;
 
 /*==============================================================================
  * Static Helper Functions
  *============================================================================*/
 
 /**
  * @brief Round up to next power of 2
  */
 static uint16_t round_up_power_of_2(unsigned size) {
     if (size < CFL_EVENT_QUEUE_MIN_SIZE) {
         return CFL_EVENT_QUEUE_MIN_SIZE;
     }
     if (size > 32768) {
         return 32768;  /* Max uint16_t power of 2 */
     }
 
     uint16_t power = 1;
     while (power < size) {
         power <<= 1;
     }
     return power;
 }
 
 /**
  * @brief Initialize a ring buffer
  */
 static void init_ring(CFL_EVENT_RING_T *ring, uint16_t capacity, CFL_EVENT_DATA_T *events) {
     ring->head = 0;
     ring->tail = 0;
     ring->capacity = capacity;
     ring->mask = capacity - 1;
     ring->events = events;
 }
 
 /**
  * @brief Check if ring buffer is full
  */
 static inline bool ring_is_full(const CFL_EVENT_RING_T *ring) {
     return ((ring->head + 1) & ring->mask) == ring->tail;
 }
 
 /**
  * @brief Check if ring buffer is empty
  */
 static inline bool ring_is_empty(const CFL_EVENT_RING_T *ring) {
     return ring->head == ring->tail;
 }
 
 /**
  * @brief Get number of events in ring buffer
  */
 static inline uint16_t ring_count(const CFL_EVENT_RING_T *ring) {
     return (ring->head - ring->tail) & ring->mask;
 }
 
 /**
  * @brief Insert event into ring buffer (assumes not full)
  */
 static inline void ring_push(CFL_EVENT_RING_T *ring, const CFL_EVENT_DATA_T *event) {
     uint16_t head = ring->head;
     ring->events[head] = *event;
     ring->head = (head + 1) & ring->mask;
 }
 
 /**
  * @brief Remove event from ring buffer (assumes not empty)
  */
 static inline void ring_pop(CFL_EVENT_RING_T *ring, CFL_EVENT_DATA_T *event) {
     uint16_t tail = ring->tail;
     *event = ring->events[tail];
     ring->tail = (tail + 1) & ring->mask;
 }
 
 /**
  * @brief Peek at event in ring buffer without removing (assumes not empty)
  */
 static inline void ring_peek(const CFL_EVENT_RING_T *ring, CFL_EVENT_DATA_T *event) {
     *event = ring->events[ring->tail];
 }
 
 /**
  * @brief Clear ring buffer, freeing any malloc'd event data
  *
  * FIX: Previous version validated malloc_flag but never called free().
  */
 static void ring_clear(CFL_EVENT_RING_T *ring) {
     CFL_EVENT_DATA_T event;
 
     while (!ring_is_empty(ring)) {
         ring_pop(ring, &event);
 
         if (event.flags & CFL_EVENT_MALLOC_FLAG) {
             if (event.event_type != CFL_EVENT_TYPE_PTR) {
                 EXCEPTION("ring_clear: malloc_flag set on non-pointer event type");
             }
             if (event.data.ptr != NULL) {
                 free(event.data.ptr);
             }
         }
     }
 }
 
 /**
  * @brief Update queue depth statistics
  */
 static inline void update_queue_stats(CFL_EVENT_QUEUE_T *queue) {
     uint16_t high_depth = ring_count(&queue->high_priority);
     if (high_depth > queue->max_high_depth) {
         queue->max_high_depth = high_depth;
     }
 
     uint16_t total_depth = high_depth + ring_count(&queue->low_priority);
     if (total_depth > queue->max_total_depth) {
         queue->max_total_depth = total_depth;
     }
 }
 
 /*==============================================================================
  * Public Function Implementations
  *============================================================================*/
 
 CFL_EVENT_QUEUE_T *cfl_create_event_queue(
     unsigned high_priority_size,
     unsigned low_priority_size,
     cfl_perm_t *perm) {
 
     if (perm == NULL) {
         EXCEPTION("cfl_create_event_queue: NULL perm pointer");
     }
     if (!perm->initialized) {
         EXCEPTION("cfl_create_event_queue: Perm allocator not initialized");
     }
 
     uint16_t high_capacity = round_up_power_of_2(high_priority_size);
     uint16_t low_capacity = round_up_power_of_2(low_priority_size);
 
     uint16_t control_size = sizeof(CFL_EVENT_QUEUE_T);
     uint16_t high_array_size = high_capacity * sizeof(CFL_EVENT_DATA_T);
     uint16_t low_array_size = low_capacity * sizeof(CFL_EVENT_DATA_T);
 
     uint16_t total_needed = control_size + high_array_size + low_array_size;
     if (cfl_perm_free_bytes(perm) < total_needed) {
         EXCEPTION("cfl_create_event_queue: Insufficient memory in perm allocator");
     }
 
     CFL_EVENT_QUEUE_T *queue =
         (CFL_EVENT_QUEUE_T *)cfl_perm_alloc_pointer(perm, control_size);
     if (queue == NULL) {
         EXCEPTION("cfl_create_event_queue: Failed to allocate control structure");
     }
 
     CFL_EVENT_DATA_T *high_events =
         (CFL_EVENT_DATA_T *)cfl_perm_alloc_pointer(perm, high_array_size);
     if (high_events == NULL) {
         EXCEPTION("cfl_create_event_queue: Failed to allocate high priority array");
     }
 
     CFL_EVENT_DATA_T *low_events =
         (CFL_EVENT_DATA_T *)cfl_perm_alloc_pointer(perm, low_array_size);
     if (low_events == NULL) {
         EXCEPTION("cfl_create_event_queue: Failed to allocate low priority array");
     }
 
     memset(queue, 0, sizeof(CFL_EVENT_QUEUE_T));
     init_ring(&queue->high_priority, high_capacity, high_events);
     init_ring(&queue->low_priority, low_capacity, low_events);
 
     queue->queue_id = g_next_queue_id++;
     queue->max_total_depth = 0;
     queue->max_high_depth = 0;
     queue->reserved = 0;
 
     return queue;
 }
 
 void cfl_clear_queue(CFL_EVENT_QUEUE_T *queue_control) {
     if (queue_control == NULL) {
         EXCEPTION("cfl_clear_queue: NULL queue_control pointer");
     }
 
     ring_clear(&queue_control->high_priority);
     ring_clear(&queue_control->low_priority);
 }
 
 bool cfl_send_event(
     CFL_EVENT_QUEUE_T *queue_control,
     unsigned priority,
     unsigned node_id,
     unsigned event_type,
     bool malloc_flag,
     unsigned event_id,
     void *data) {
 
     if (queue_control == NULL) {
         EXCEPTION("cfl_send_event: NULL queue_control pointer");
     }
     if (priority != CFL_EVENT_PRIORITY_LOW && priority != CFL_EVENT_PRIORITY_HIGH) {
         EXCEPTION("cfl_send_event: Invalid priority level");
     }
     if (event_type > CFL_EVENT_TYPE_NULL) {
         EXCEPTION("cfl_send_event: Invalid event_type");
     }
 
     CFL_EVENT_RING_T *ring = (priority == CFL_EVENT_PRIORITY_HIGH)
         ? &queue_control->high_priority
         : &queue_control->low_priority;
 
     if (ring_is_full(ring)) {
         EXCEPTION("cfl_send_event: ring is full");
     }
 
     CFL_EVENT_DATA_T event;
     event.node_id = (uint16_t)node_id;
     event.event_type = (uint8_t)event_type;
     event.flags = malloc_flag ? CFL_EVENT_MALLOC_FLAG : 0;
     event.event_id = (uint16_t)event_id;
     event.queue_number = queue_control->queue_id;
     event.data.ptr = data;
 
     ring_push(ring, &event);
     update_queue_stats(queue_control);
 
     return true;
 }
 
 bool cfl_pop_event(
     CFL_EVENT_QUEUE_T *queue_control,
     CFL_EVENT_DATA_T *event_data) {
 
     if (queue_control == NULL) {
         EXCEPTION("cfl_pop_event: NULL queue_control pointer");
     }
     if (event_data == NULL) {
         EXCEPTION("cfl_pop_event: NULL event_data pointer");
     }
 
     if (!ring_is_empty(&queue_control->high_priority)) {
         ring_pop(&queue_control->high_priority, event_data);
         return true;
     }
 
     if (!ring_is_empty(&queue_control->low_priority)) {
         ring_pop(&queue_control->low_priority, event_data);
         return true;
     }
 
     return false;
 }
 
 bool cfl_peek_event(
     CFL_EVENT_QUEUE_T *queue_control,
     CFL_EVENT_DATA_T *event_data) {
 
     if (queue_control == NULL) {
         EXCEPTION("cfl_peek_event: NULL queue_control pointer");
     }
     if (event_data == NULL) {
         EXCEPTION("cfl_peek_event: NULL event_data pointer");
     }
 
     if (!ring_is_empty(&queue_control->high_priority)) {
         ring_peek(&queue_control->high_priority, event_data);
         return true;
     }
 
     if (!ring_is_empty(&queue_control->low_priority)) {
         ring_peek(&queue_control->low_priority, event_data);
         return true;
     }
 
     return false;
 }
 
 unsigned cfl_queue_number(CFL_EVENT_DATA_T *event_data) {
     if (event_data == NULL) {
         EXCEPTION("cfl_queue_number: NULL event_data pointer");
     }
     return event_data->queue_number;
 }
 
 unsigned cfl_high_priority_count(CFL_EVENT_QUEUE_T *queue_control) {
     if (queue_control == NULL) {
         EXCEPTION("cfl_high_priority_count: NULL queue_control pointer");
     }
     return ring_count(&queue_control->high_priority);
 }
 
 unsigned cfl_low_priority_count(CFL_EVENT_QUEUE_T *queue_control) {
     if (queue_control == NULL) {
         EXCEPTION("cfl_low_priority_count: NULL queue_control pointer");
     }
     return ring_count(&queue_control->low_priority);
 }
 
 unsigned cfl_total_event_count(CFL_EVENT_QUEUE_T *queue_control) {
     if (queue_control == NULL) {
         EXCEPTION("cfl_total_event_count: NULL queue_control pointer");
     }
     return ring_count(&queue_control->high_priority) +
            ring_count(&queue_control->low_priority);
 }
 
 unsigned cfl_get_max_total_depth(CFL_EVENT_QUEUE_T *queue_control) {
     if (queue_control == NULL) {
         EXCEPTION("cfl_get_max_total_depth: NULL queue_control pointer");
     }
     return queue_control->max_total_depth;
 }
 
 unsigned cfl_get_max_high_depth(CFL_EVENT_QUEUE_T *queue_control) {
     if (queue_control == NULL) {
         EXCEPTION("cfl_get_max_high_depth: NULL queue_control pointer");
     }
     return queue_control->max_high_depth;
 }
 
 void cfl_reset_queue_stats(CFL_EVENT_QUEUE_T *queue_control) {
     if (queue_control == NULL) {
         EXCEPTION("cfl_reset_queue_stats: NULL queue_control pointer");
     }
     queue_control->max_total_depth = 0;
     queue_control->max_high_depth = 0;
 }
 
 /*==============================================================================
  * Type-Specific Helper Functions
  *============================================================================*/
 
 bool cfl_send_unsigned_event(
     CFL_EVENT_QUEUE_T *queue_control,
     unsigned priority,
     unsigned node_id,
     unsigned event_id,
     cfl_size_t value) {
 
     CFL_EVENT_VALUE_T temp;
     temp.unsigned_val = value;
 
     return cfl_send_event(queue_control, priority, node_id,
         CFL_EVENT_TYPE_UINT, false, event_id, temp.ptr);
 }
 
 bool cfl_send_integer_event(
     CFL_EVENT_QUEUE_T *queue_control,
     unsigned priority,
     unsigned node_id,
     unsigned event_id,
     cfl_int_t value) {
 
     CFL_EVENT_VALUE_T temp;
     temp.integer = value;
 
     return cfl_send_event(queue_control, priority, node_id,
         CFL_EVENT_TYPE_INT, false, event_id, temp.ptr);
 }
 
 bool cfl_send_float_event(
     CFL_EVENT_QUEUE_T *queue_control,
     unsigned priority,
     unsigned node_id,
     unsigned event_id,
     cfl_float_t value) {
 
     CFL_EVENT_VALUE_T temp;
     temp.floating = value;
 
     return cfl_send_event(queue_control, priority, node_id,
         CFL_EVENT_TYPE_FLOAT, false, event_id, temp.ptr);
 }
 
 bool cfl_send_data_event(
     CFL_EVENT_QUEUE_T *queue_control,
     unsigned priority,
     unsigned node_id,
     bool malloc_flag,
     unsigned event_id,
     void *data) {
 
     return cfl_send_event(queue_control, priority, node_id,
         CFL_EVENT_TYPE_PTR, malloc_flag, event_id, data);
 }
 
 bool cfl_send_null_event(
     CFL_EVENT_QUEUE_T *queue_control,
     unsigned priority,
     unsigned node_id,
     unsigned event_id) {
 
     return cfl_send_event(queue_control, priority, node_id,
         CFL_EVENT_TYPE_NULL, false, event_id, NULL);
 }
 
 bool cfl_send_json_record_event(
     CFL_EVENT_QUEUE_T *queue_control,
     unsigned priority,
     unsigned node_id,
     unsigned event_id,
     uint32_t record_index) {
 
     CFL_EVENT_VALUE_T temp;
     temp.unsigned_val = record_index;
 
     return cfl_send_event(queue_control, priority, node_id,
         CFL_EVENT_TYPE_JSON_RECORD, false, event_id, temp.ptr);
 }
 
 bool cfl_send_node_id_event(
     CFL_EVENT_QUEUE_T *queue_control,
     unsigned priority,
     unsigned node_id,
     unsigned event_id,
     unsigned node_index) {
 
     CFL_EVENT_VALUE_T temp;
     temp.unsigned_val = node_index;
 
     return cfl_send_event(queue_control, priority, node_id,
         CFL_EVENT_TYPE_NODE_ID, false, event_id, temp.ptr);
 }
 
 bool cfl_send_streaming_data_event(
     CFL_EVENT_QUEUE_T *queue_control,
     unsigned priority,
     unsigned node_id,
     unsigned event_id,
     void *data) {
 
     return cfl_send_event(queue_control, priority, node_id,
         CFL_EVENT_TYPE_STREAMING_DATA, false, event_id, data);
 }
 
 bool cfl_send_streaming_collected_packets_event(
     CFL_EVENT_QUEUE_T *queue_control,
     unsigned priority,
     unsigned node_id,
     unsigned event_id,
     void *data) {
 
     return cfl_send_event(queue_control, priority, node_id,
         CFL_EVENT_TYPE_STREAMING_COLLECTED_PACKETS, false, event_id, data);
 }