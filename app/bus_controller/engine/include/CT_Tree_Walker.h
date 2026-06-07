/**
 * CT_Tree_Walker.h
 * 
 * Memory-efficient tree walker for embedded systems
 * Uses iterative DFS with explicit stack to avoid recursion
 * All -qualified for O2 optimization safety with interrupt-driven systems
 */

 #ifndef CT_TREE_WALKER_H
 #define CT_TREE_WALKER_H
 
 #include <stdint.h>
 #include <stdbool.h>
 
 #ifdef __cplusplus
 extern "C" {
 #endif
 
 /* ============================================================================
  * RETURN CODES
  * ============================================================================ */
 
 typedef enum {
     CT_CONTINUE = 0,        /* Continue normal traversal */
     CT_SKIP_CHILDREN = 1,   /* Skip children of current node */
     CT_STOP_BRANCH = 2,     /* Stop current branch, continue siblings */
     CT_STOP_SIBLINGS = 3,   /* Stop siblings, return to parent */
     CT_STOP_LEVEL = 4,      /* Stop at current level */
     CT_STOP_ALL = 5         /* Stop entire walk */
 } CT_ReturnCode;
 
 /* ============================================================================
  * FLAG DEFINITIONS
  * ============================================================================ */
 
 /* Engine flags (lower 4 bits) */
 #define CT_FLAG_VISITED    0x01  /* Node has been visited */
 #define CT_FLAG_RESERVED1  0x02  /* Reserved for future use */
 #define CT_FLAG_RESERVED2  0x04  /* Reserved for future use */
 #define CT_FLAG_RESERVED3  0x08  /* Reserved for future use */
 
 /* User flags (upper 4 bits) - available for application use */
 #define CT_FLAG_USER0      0x10
 #define CT_FLAG_USER1      0x20
 #define CT_FLAG_USER2      0x40
 #define CT_FLAG_USER3      0x80
 
 /* Masks */
 #define CT_FLAG_ENGINE_MASK  0x0F
 #define CT_FLAG_USER_MASK    0xF0
 
 /* ============================================================================
  * CALLBACK FUNCTION TYPES
  * ============================================================================ */
 
 /**
  * Get children callback
  * Returns number of children written to children_out array
  * Must not return more than max_children
  */
 typedef unsigned int (*CT_GetChildrenFunc)(
     void* user_handle,
     unsigned int node_id,
     unsigned int* children_out,
     unsigned int max_children
 );
 
 /**
  * Apply function callback
  * Called for each visited node
  * Returns CT_ReturnCode to control traversal
  */
 typedef CT_ReturnCode (*CT_ApplyFunc)(
     void* user_handle,
     unsigned int node_id,
     unsigned int level,
     uint8_t* flags
 );
 
 /* ============================================================================
  * STACK ENTRY
  * ============================================================================ */
 
 typedef struct {
     unsigned int node_id;
     unsigned int level;
     unsigned int child_index;
 } CT_StackEntry;
 
 /* ============================================================================
  * TREE WALKER STRUCTURE
  * ============================================================================ */
 
 typedef struct CT_TreeWalker {
     void* user_handle;              /* User-provided context */
     unsigned int max_nodes;         /* Maximum number of nodes in tree */
      uint8_t* flags;        /* Flags array ( for interrupt safety) */
     CT_GetChildrenFunc get_children;/* Function to get children */
     CT_ApplyFunc apply_func;        /* Function to apply to each node */
     unsigned int max_level;         /* Maximum level to traverse */
     unsigned int max_node_id;       /* Maximum valid node ID for this walk */
     bool stop_all;                  /* Global stop flag */
 } CT_TreeWalker;
 
 /* ============================================================================
  * WALKER CONTEXT (for save/restore)
  * ============================================================================ */
 
 typedef struct {
      uint8_t* saved_flags;
     bool saved_stop_all;
     unsigned int saved_max_level;
     unsigned int saved_max_node_id;
     CT_ApplyFunc saved_apply_func;
 } CT_WalkerContext;
 
 /* ============================================================================
  * PUBLIC API
  * ============================================================================ */
 
 /**
  * Initialize tree walker
  * 
  * @param walker Pointer to walker structure ( for interrupt safety)
  * @param max_nodes Maximum number of nodes in tree
  * @param flags Pointer to flags array (, must be max_nodes in size)
  * @param get_children Function to get children of a node
  * @param apply_func Function to apply to each visited node
  * @return true on success, exception on failure
  */
 bool ct_walker_init(
      CT_TreeWalker* walker,
     unsigned int max_nodes,
      uint8_t* flags,
     CT_GetChildrenFunc get_children,
     CT_ApplyFunc apply_func
 );
 
 /**
  * Execute tree walk starting from root_id
  * 
  * @param walker Pointer to walker structure ()
  * @param user_handle User context to pass to callbacks
  * @param root_id Starting node ID
  * @param stack Pointer to stack array ( for interrupt safety)
  * @param stack_capacity Size of stack array
  * @param max_level Maximum level to traverse (0xFFFF for unlimited)
  * @param max_node_id Maximum valid node ID for this walk
  * @return CT_ReturnCode indicating how walk terminated
  */
 CT_ReturnCode ct_walker_walk(
      CT_TreeWalker* walker,
     void* user_handle,
     unsigned int root_id,
      CT_StackEntry* stack,
     unsigned int stack_capacity,
     unsigned int max_level,
     unsigned int max_node_id
 );
 
 /**
  * Reset walker state (clears visited flags and stop_all)
  * 
  * @param walker Pointer to walker structure ()
  */
 void ct_walker_reset( CT_TreeWalker* walker);
 
 /**
  * Check if a node has been visited
  * 
  * @param walker Pointer to walker structure ()
  * @param node_id Node ID to check
  * @return true if visited, false otherwise
  */
 bool ct_walker_is_visited( const CT_TreeWalker* walker, unsigned int node_id);
 
 /**
  * Set user flags for a node (preserves engine flags)
  * 
  * @param walker Pointer to walker structure ()
  * @param node_id Node ID
  * @param flags User flags to set (only upper 4 bits used)
  */
 void ct_walker_set_user_flags( CT_TreeWalker* walker, unsigned int node_id, uint8_t flags);
 
 /**
  * Get user flags for a node
  * 
  * @param walker Pointer to walker structure ()
  * @param node_id Node ID
  * @return User flags (upper 4 bits only)
  */
 uint8_t ct_walker_get_user_flags( const CT_TreeWalker* walker, unsigned int node_id);
 
 /**
  * Update walker functions (allows changing callbacks between walks)
  * 
  * @param walker Pointer to walker structure ()
  * @param apply_func New apply function (NULL to keep current)
  * @param get_children New get_children function (NULL to keep current)
  */
 void ct_walker_update_functions(
      CT_TreeWalker* walker,
     CT_ApplyFunc apply_func,
     CT_GetChildrenFunc get_children
 );
 
 /**
  * Save walker context for nested walks
  * 
  * @param walker Pointer to walker structure ()
  * @param context Pointer to context structure to save to ()
  * @param backup_flags_buffer Buffer for backing up flags (, must be max_nodes in size)
  * @return true on success
  */
 bool ct_walker_save_context(
      CT_TreeWalker* walker,
      CT_WalkerContext* context,
      uint8_t* backup_flags_buffer
 );
 
 /**
  * Restore walker context after nested walk
  * 
  * @param walker Pointer to walker structure ()
  * @param context Pointer to context structure to restore from ()
  */
 void ct_walker_restore_context(
      CT_TreeWalker* walker,
      const CT_WalkerContext* context
 );
 
 #ifdef __cplusplus
 }
 #endif
 
 #endif /* CT_TREE_WALKER_H */