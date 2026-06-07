/**
 * CT_Tree_Walker.c
 * 
 * Implementation of the C tree walker with exception handling
 * Uses iterative DFS for memory-efficient traversal in embedded environments
 * Properly handles  parameters for O2 optimization safety
 */

 #include "CT_Tree_Walker.h"
 #include "cfl_exception.h"
 #include <string.h>
 #include <stdio.h>
 
 /* Maximum children buffer size */
 #define MAX_CHILDREN_BUFFER 256
 
 /* ============================================================================
  * PRIVATE HELPER FUNCTIONS
  * ============================================================================ */
 
 /**
  * Clear all engine flags for all nodes
  * Properly handles  flags array
  */
 static void clear_engine_flags( CT_TreeWalker* walker) {
     /* Read  members once */
     unsigned int max_nodes = walker->max_nodes;
      uint8_t* flags = walker->flags;
     
     for (unsigned int i = 0; i < max_nodes; i++) {
         /* Volatile read, modify,  write */
         uint8_t current_flags = flags[i];
         flags[i] = current_flags & CT_FLAG_USER_MASK;  /* Keep user flags, clear engine flags */
     }
 }
 
 /**
  * Iterative DFS implementation using explicit stack
  * Memory usage: O(max_tree_depth) - optimal for embedded systems
  * Iteration count bounded appropriately for current algorithm
  * 
  * Properly handles  parameters for interrupt-driven systems
  */
 static CT_ReturnCode walk_iterative(
      CT_TreeWalker* walker,
     unsigned int root_id,
      CT_StackEntry* stack,
     unsigned int stack_capacity
 ) {
     if (stack_capacity == 0) {
         EXCEPTION("walk_iterative: stack_capacity is 0");
     }
     
     /* Read  walker members once at start for bounds checking */
     unsigned int max_nodes = walker->max_nodes;
     unsigned int max_node_id = walker->max_node_id;
      uint8_t* flags = walker->flags;
     
     /* Check root bounds against walker limits */
     if (root_id >= max_nodes) {
         EXCEPTION("walk_iterative: root_id exceeds max_nodes");
     }
     
     /* Check root bounds against walk-specific limits */
     if (root_id > max_node_id) {
         EXCEPTION("walk_iterative: root_id exceeds max_node_id");
     }
     
     /* Initialize stack with root -  writes */
     int stack_top = 0;
     stack[0].node_id = root_id;
     stack[0].level = 0;
     stack[0].child_index = 0;
     
     /* Iteration counter for runaway protection */
     /* Current algorithm iterates multiple times per node (once per child) */
     /* Max iterations = (nodes + 1) * (max_children + 1) */
     unsigned int iteration_count = 0;
     const unsigned int max_iterations = (max_node_id + 1) * (MAX_CHILDREN_BUFFER + 1);
     
     while (stack_top >= 0) {
         /* Check iteration count */
         iteration_count++;
         if (iteration_count > max_iterations) {
             EXCEPTION("walk_iterative: iteration count exceeded limit - possible infinite loop");
         }
         
         /* Check stop flag -  read */
         if (walker->stop_all) {
             return CT_STOP_ALL;
         }
         
         /* Pop current entry from  stack */
         CT_StackEntry current;
         current.node_id = stack[stack_top].node_id;
         current.level = stack[stack_top].level;
         current.child_index = stack[stack_top].child_index;
         
         /* Check bounds against walker limits */
         if (current.node_id >= max_nodes) {
             EXCEPTION("walk_iterative: node_id exceeds max_nodes during traversal");
         }
         
         /* Check bounds against walk-specific limits */
         if (current.node_id > max_node_id) {
             EXCEPTION("walk_iterative: node_id exceeds max_node_id during traversal");
         }
         
         /* Check if already visited -  read */
         uint8_t node_flags = flags[current.node_id];
         
         if (node_flags & CT_FLAG_VISITED) {
             if (current.child_index == 0) {
                 /* Already fully processed */
                 stack_top--;
                 continue;
             }
         } else {
             /* First visit - mark as visited with  write */
             flags[current.node_id] = node_flags | CT_FLAG_VISITED;
             
             /* Read max_level ( read) */
             unsigned int current_max_level = walker->max_level;
             
             /* Check max level */
             if (current.level > current_max_level) {
                 stack_top--;
                 continue;
             }
             
             /* Apply function - cast away  for callback since callbacks
              * typically don't expect  parameters */
             CT_ReturnCode ret = walker->apply_func(
                 walker->user_handle,
                 current.node_id,
                 current.level,
                 (uint8_t*)flags
             );
 
             if (ret == CT_STOP_ALL) {
                 walker->stop_all = true;
                 return ret;
             }
 
             if (ret == CT_STOP_BRANCH) {
                stack_top--;
                continue;
            }
            
            if (ret == CT_STOP_SIBLINGS) {
                stack_top--;  /* Pop current node */
                if (stack_top >= 0) {
                    stack_top--;  /* Pop parent too - this stops siblings */
                }
                continue;
            }
 
             if (ret == CT_STOP_LEVEL) {
                 walker->max_level = current.level;
                 stack_top--;
                 continue;
             }
 
             if (ret == CT_SKIP_CHILDREN) {
                 stack_top--;
                 continue;
             }
         }
         
         /* Get children - callback reads from user data structures */
         unsigned int children[MAX_CHILDREN_BUFFER];
         unsigned int num_children = walker->get_children(
             walker->user_handle,
             current.node_id,
             children,
             MAX_CHILDREN_BUFFER
         );
         
         /* Check for buffer overflow */
         if (num_children > MAX_CHILDREN_BUFFER) {
             EXCEPTION("walk_iterative: get_children returned too many children");
         }
         
         /* Check if we have more children to process */
         if (current.child_index >= num_children) {
             stack_top--;
             continue;
         }
         
         /* Get the child to process */
         unsigned int child_id = children[current.child_index];
         
         /* Update current entry to process next child later -  write */
         stack[stack_top].child_index = current.child_index + 1;
         
         /* Validate child ID against walker limits */
         if (child_id >= max_nodes) {
             EXCEPTION("walk_iterative: child_id exceeds max_nodes from get_children");
         }
         
         /* Validate child ID against walk-specific limits */
         if (child_id > max_node_id) {
             EXCEPTION("walk_iterative: child_id exceeds max_node_id from get_children");
         }
         
         /* Skip if already visited -  read */
         if (flags[child_id] & CT_FLAG_VISITED) {
             continue;  /* Stay at same stack level, process next child */
         }
         
         /* Push next child onto stack */
         if (stack_top + 1 >= (int)stack_capacity) {
             /* Stack overflow - fail hard */
             EXCEPTION("walk_iterative: stack overflow - increase stack_capacity");
         }
         
         stack_top++;
         /* Volatile writes to stack */
         stack[stack_top].node_id = child_id;
         stack[stack_top].level = current.level + 1;
         stack[stack_top].child_index = 0;
     }
     
     return CT_CONTINUE;
 }
 
 /* ============================================================================
  * PUBLIC API IMPLEMENTATION
  * ============================================================================ */
 
 bool ct_walker_init(
      CT_TreeWalker* walker,
     unsigned int max_nodes,
      uint8_t* flags,
     CT_GetChildrenFunc get_children,
     CT_ApplyFunc apply_func
 ) {
     if (!walker) {
         EXCEPTION("ct_walker_init: walker is NULL");
     }
     
     if (!flags) {
         EXCEPTION("ct_walker_init: flags is NULL");
     }
     
     if (!get_children) {
         EXCEPTION("ct_walker_init: get_children is NULL");
     }
     
     if (!apply_func) {
         EXCEPTION("ct_walker_init: apply_func is NULL");
     }
     
     if (max_nodes == 0) {
         EXCEPTION("ct_walker_init: max_nodes is 0");
     }
     
     /* Initialize walker -  writes */
     walker->user_handle = NULL;
     walker->max_nodes = max_nodes;
     walker->flags = flags;
     walker->get_children = get_children;
     walker->apply_func = apply_func;
     walker->max_level = 0xFFFF;
     walker->max_node_id = max_nodes - 1;  /* Initialize to max possible */
     walker->stop_all = false;
     
     /* Clear engine flags */
     clear_engine_flags(walker);
     
     return true;
 }
 
 CT_ReturnCode ct_walker_walk(
      CT_TreeWalker* walker,
     void* user_handle,
     unsigned int root_id,
      CT_StackEntry* stack,
     unsigned int stack_capacity,
     unsigned int max_level,
     unsigned int max_node_id
 ) {
     if (!walker) {
         EXCEPTION("ct_walker_walk: walker is NULL");
     }
     
     if (!stack) {
         EXCEPTION("ct_walker_walk: stack is NULL");
     }
     
     if (stack_capacity == 0) {
         EXCEPTION("ct_walker_walk: stack_capacity is 0");
     }
     
     /* Read max_nodes for validation -  read */
     unsigned int walker_max_nodes = walker->max_nodes;
     
     if (root_id >= walker_max_nodes) {
         EXCEPTION("ct_walker_walk: root_id exceeds max_nodes");
     }
     
     /* Validate max_node_id parameter */
     if (max_node_id > walker_max_nodes) {
         EXCEPTION("ct_walker_walk: max_node_id exceeds walker max_nodes");
     }
     
     /* Set user handle, max level, and max node ID -  writes */
     walker->user_handle = user_handle;
     walker->max_level = max_level;
     walker->max_node_id = max_node_id;
     walker->stop_all = false;
     
     /* Clear engine flags before walk */
     clear_engine_flags(walker);
     
     /* Execute iterative DFS traversal */
     return walk_iterative(walker, root_id, stack, stack_capacity);
 }
 
 void ct_walker_reset( CT_TreeWalker* walker) {
     if (!walker) {
         EXCEPTION("ct_walker_reset: walker is NULL");
     }
     
     clear_engine_flags(walker);
     walker->stop_all = false;
 }
 
 bool ct_walker_is_visited( const CT_TreeWalker* walker, unsigned int node_id) {
     if (!walker) {
         EXCEPTION("ct_walker_is_visited: walker is NULL");
     }
     
     /* Read  members */
     unsigned int max_nodes = walker->max_nodes;
      const uint8_t* flags = walker->flags;
     
     if (node_id >= max_nodes) {
         EXCEPTION("ct_walker_is_visited: node_id out of bounds");
     }
     
     /* Volatile read of flag */
     return (flags[node_id] & CT_FLAG_VISITED) != 0;
 }
 
 void ct_walker_set_user_flags( CT_TreeWalker* walker, unsigned int node_id, uint8_t flags_to_set) {
     if (!walker) {
         EXCEPTION("ct_walker_set_user_flags: walker is NULL");
     }
     
     /* Read  members */
     unsigned int max_nodes = walker->max_nodes;
      uint8_t* flags = walker->flags;
     
     if (node_id >= max_nodes) {
         EXCEPTION("ct_walker_set_user_flags: node_id out of bounds");
     }
     
     /* Volatile read, modify,  write */
     uint8_t current_flags = flags[node_id];
     current_flags &= ~CT_FLAG_USER_MASK;  /* Clear old user flags */
     current_flags |= (flags_to_set & CT_FLAG_USER_MASK);  /* Set new user flags */
     flags[node_id] = current_flags;
 }
 
 uint8_t ct_walker_get_user_flags( const CT_TreeWalker* walker, unsigned int node_id) {
     if (!walker) {
         EXCEPTION("ct_walker_get_user_flags: walker is NULL");
     }
     
     /* Read  members */
     unsigned int max_nodes = walker->max_nodes;
      const uint8_t* flags = walker->flags;
     
     if (node_id >= max_nodes) {
         EXCEPTION("ct_walker_get_user_flags: node_id out of bounds");
     }
     
     /* Volatile read of flag */
     return flags[node_id] & CT_FLAG_USER_MASK;
 }
 
 void ct_walker_update_functions(
      CT_TreeWalker* walker,
     CT_ApplyFunc apply_func,
     CT_GetChildrenFunc get_children
 ) {
     if (!walker) {
         EXCEPTION("ct_walker_update_functions: walker is NULL");
     }
     
     /* Volatile writes */
     if (apply_func != NULL) {
         walker->apply_func = apply_func;
     }
     
     if (get_children != NULL) {
         walker->get_children = get_children;
     }
 }
 
 bool ct_walker_save_context(
      CT_TreeWalker* walker,
      CT_WalkerContext* context,
      uint8_t* backup_flags_buffer
 ) {
     if (!walker) {
         EXCEPTION("ct_walker_save_context: walker is NULL");
     }
     
     if (!context) {
         EXCEPTION("ct_walker_save_context: context is NULL");
     }
     
     if (!backup_flags_buffer) {
         EXCEPTION("ct_walker_save_context: backup_flags_buffer is NULL");
     }
     
     /* Read  members */
     unsigned int max_nodes = walker->max_nodes;
      uint8_t* flags = walker->flags;
     
     /* Copy current flags to backup buffer -  reads and writes */
     for (unsigned int i = 0; i < max_nodes; i++) {
         backup_flags_buffer[i] = flags[i];
     }
     
     /* Save walker state -  reads and writes */
     context->saved_flags = backup_flags_buffer;
     context->saved_stop_all = walker->stop_all;
     context->saved_max_level = walker->max_level;
     context->saved_max_node_id = walker->max_node_id;
     context->saved_apply_func = walker->apply_func;
     
     return true;
 }
 
 void ct_walker_restore_context(
      CT_TreeWalker* walker,
      const CT_WalkerContext* context
 ) {
     if (!walker) {
         EXCEPTION("ct_walker_restore_context: walker is NULL");
     }
     
     if (!context) {
         EXCEPTION("ct_walker_restore_context: context is NULL");
     }
     
     /* Read saved flags pointer -  read */
      const uint8_t* saved_flags = context->saved_flags;
     
     if (!saved_flags) {
         EXCEPTION("ct_walker_restore_context: saved_flags is NULL");
     }
     
     /* Read  members */
     unsigned int max_nodes = walker->max_nodes;
      uint8_t* flags = walker->flags;
     
     /* Restore flags -  reads and writes */
     for (unsigned int i = 0; i < max_nodes; i++) {
         flags[i] = saved_flags[i];
     }
     
     /* Restore walker state -  reads and writes */
     walker->stop_all = context->saved_stop_all;
     walker->max_level = context->saved_max_level;
     walker->max_node_id = context->saved_max_node_id;
     walker->apply_func = context->saved_apply_func;
 }