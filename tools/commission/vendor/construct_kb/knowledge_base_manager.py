import sqlite3
import json
from typing import Optional, Dict, Any, List, Tuple


class KnowledgeBaseManager:
    def __init__(self, table_name: str, db_path: str, ltree_extension_path: str = None, upload_flag: bool = False):
        """
        Initialize the KnowledgeBaseManager with SQLite database path.
        
        Args:
            table_name: Base name for the knowledge base tables
            db_path: Path to the SQLite database file
            ltree_extension_path: Path to the ltree extension WITHOUT extension suffix
                                 (e.g., './ltree' or '/usr/local/lib/ltree')
                                 SQLite automatically adds .so/.dll/.dylib
                                 If None, will search common locations
            reset: If True, delete existing tables before creating new ones (default: False)
        """
        self.db_path = db_path
        self.table_name = table_name
        
        # Determine ltree extension path
        if ltree_extension_path is None:
            # Search common locations
            import sys
            search_paths = [
                './ltree',
                '/usr/local/lib/ltree',
                '/usr/lib/ltree',
            ]
            
            # Check which file exists
            suffix = '.dylib' if sys.platform == 'darwin' else '.so'
            for path in search_paths:
                import os
                if os.path.exists(path + suffix):
                    ltree_extension_path = path
                    break
            else:
                # Default to ./ltree if none found
                ltree_extension_path = './ltree'
        
        self.ltree_extension_path = ltree_extension_path
        self._connect()
        self.upload_flag = upload_flag
        if self.upload_flag == False:
            self._create_tables()
        
    def _connect(self):
        """Establish database connection and load ltree extension."""
        try:
            self.conn = sqlite3.connect(self.db_path)
            self.conn.row_factory = sqlite3.Row  # Enable column access by name
            self.cursor = self.conn.cursor()
            
            # Enable foreign keys support
            self.cursor.execute("PRAGMA foreign_keys = ON")
            
            # Load ltree extension
            # Note: load_extension() automatically appends .so/.dll/.dylib
            # so we need to strip the extension from the path
            self.conn.enable_load_extension(True)
            try:
                import os
                # Strip file extension since load_extension adds it automatically
                ext_path = os.path.splitext(self.ltree_extension_path)[0]
                self.conn.load_extension(ext_path)
                print(f"Loaded ltree extension from: {ext_path}")
            except sqlite3.Error as e:
                print(f"Warning: Could not load ltree extension from {self.ltree_extension_path}: {e}")
                print("Ltree-specific query methods will not be available.")
            finally:
                self.conn.enable_load_extension(False)
            
            self.conn.commit()
        except sqlite3.Error as e:
            print(f"Error connecting to database: {e}")
            raise
            
    def disconnect(self):
        """Close database connection."""
        if self.cursor:
            self.cursor.close()
        if self.conn:
            self.conn.close()
            
    def _delete_table(self, table_name: str):
        """
        Deletes a specified table from the SQLite database.
        
        Args:
            table_name: Name of the table to delete.
        """
        try:
            drop_query = f"DROP TABLE IF EXISTS {table_name}"
            self.cursor.execute(drop_query)
            self.conn.commit()
        except sqlite3.Error as e:
            print(f"Error deleting table {table_name}: {e}")
            raise
            
    def _create_tables(self):
        """
        Create knowledge base tables with the supplied table name.
        If reset=True, delete existing tables first.
        
        Args:
            reset: If True, delete existing tables before creating (default: False)
        """
        
        
        self._delete_table(self.table_name)
        self._delete_table(f"{self.table_name}_info")
        self._delete_table(f"{self.table_name}_link")
        self._delete_table(f"{self.table_name}_link_mount")
        
        try:
            # Create knowledge base table (conditionally)
            kb_table_query = f"""
                CREATE TABLE IF NOT EXISTS {self.table_name} (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    knowledge_base TEXT NOT NULL,
                    label TEXT NOT NULL,
                    name TEXT NOT NULL,
                    properties TEXT,
                    data TEXT,
                    has_link INTEGER DEFAULT 0,
                    has_link_mount INTEGER DEFAULT 0,
                    path TEXT UNIQUE
                )
            """
            
            # Create information table (conditionally)
            info_table_query = f"""
                CREATE TABLE IF NOT EXISTS {self.table_name}_info (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    knowledge_base TEXT NOT NULL UNIQUE,
                    description TEXT
                )
            """
            
            # Create link table (conditionally)
            link_table_query = f"""
                CREATE TABLE IF NOT EXISTS {self.table_name}_link (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    link_name TEXT NOT NULL,
                    parent_node_kb TEXT NOT NULL,
                    parent_path TEXT NOT NULL,
                    created_at TEXT DEFAULT CURRENT_TIMESTAMP,
                    UNIQUE(link_name, parent_node_kb, parent_path)
                )
            """
            
            # Create link mount table (conditionally)
            link_mount_table_query = f"""
                CREATE TABLE IF NOT EXISTS {self.table_name}_link_mount (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    link_name TEXT NOT NULL UNIQUE,
                    knowledge_base TEXT NOT NULL,
                    mount_path TEXT NOT NULL,
                    description TEXT,
                    created_at TEXT DEFAULT CURRENT_TIMESTAMP,
                    UNIQUE(knowledge_base, mount_path)
                )
            """
            
            # Execute table creation
            self.cursor.execute(kb_table_query)
            self.cursor.execute(info_table_query)
            self.cursor.execute(link_table_query)
            self.cursor.execute(link_mount_table_query)
            
            # Create indexes for better performance
            indexes = [
                # === Main Knowledge Base Table Indexes ===
                f"CREATE INDEX IF NOT EXISTS idx_{self.table_name}_kb ON {self.table_name} (knowledge_base)",
                f"CREATE INDEX IF NOT EXISTS idx_{self.table_name}_path ON {self.table_name} (path)",
                f"CREATE INDEX IF NOT EXISTS idx_{self.table_name}_label ON {self.table_name} (label)",
                f"CREATE INDEX IF NOT EXISTS idx_{self.table_name}_name ON {self.table_name} (name)",
                f"CREATE INDEX IF NOT EXISTS idx_{self.table_name}_has_link ON {self.table_name} (has_link)",
                f"CREATE INDEX IF NOT EXISTS idx_{self.table_name}_has_link_mount ON {self.table_name} (has_link_mount)",
                f"CREATE INDEX IF NOT EXISTS idx_{self.table_name}_kb_path ON {self.table_name} (knowledge_base, path)",
                
                # === Info Table Indexes ===
                f"CREATE INDEX IF NOT EXISTS idx_{self.table_name}_info_kb ON {self.table_name}_info (knowledge_base)",
                
                # === Link Table Indexes ===
                f"CREATE INDEX IF NOT EXISTS idx_{self.table_name}_link_name ON {self.table_name}_link (link_name)",
                f"CREATE INDEX IF NOT EXISTS idx_{self.table_name}_link_parent_kb ON {self.table_name}_link (parent_node_kb)",
                f"CREATE INDEX IF NOT EXISTS idx_{self.table_name}_link_parent_path ON {self.table_name}_link (parent_path)",
                f"CREATE INDEX IF NOT EXISTS idx_{self.table_name}_link_created ON {self.table_name}_link (created_at)",
                f"CREATE INDEX IF NOT EXISTS idx_{self.table_name}_link_composite ON {self.table_name}_link (link_name, parent_node_kb)",
                
                # === Link Mount Table Indexes ===
                f"CREATE INDEX IF NOT EXISTS idx_{self.table_name}_mount_link_name ON {self.table_name}_link_mount (link_name)",
                f"CREATE INDEX IF NOT EXISTS idx_{self.table_name}_mount_kb ON {self.table_name}_link_mount (knowledge_base)",
                f"CREATE INDEX IF NOT EXISTS idx_{self.table_name}_mount_path ON {self.table_name}_link_mount (mount_path)",
                f"CREATE INDEX IF NOT EXISTS idx_{self.table_name}_mount_created ON {self.table_name}_link_mount (created_at)",
                f"CREATE INDEX IF NOT EXISTS idx_{self.table_name}_mount_composite ON {self.table_name}_link_mount (knowledge_base, mount_path)",
            ]
            
            for index_query in indexes:
                self.cursor.execute(index_query)
            
            self.conn.commit()
            
        except sqlite3.Error as e:
            self.conn.rollback()
            print(f"Error creating tables: {e}")
            raise
            
    def add_kb(self, kb_name: str, description: Optional[str] = None):
        """
        Add a knowledge base entry to the information table.
        
        Args:
            kb_name: Name of the knowledge base
            description: Optional description of the knowledge base
        """
        if not isinstance(kb_name, str):
            raise TypeError("kb_name must be a string")
        if description is not None and not isinstance(description, str):
            raise TypeError("description must be a string")
        
        try:
            info_table = f"{self.table_name}_info"
            query = f"""
                INSERT OR IGNORE INTO {info_table} (knowledge_base, description)
                VALUES (?, ?)
            """
            
            self.cursor.execute(query, (kb_name, description))
            self.conn.commit()
            
        except sqlite3.Error as e:
            self.conn.rollback()
            print(f"Error adding knowledge base: {e}")
            raise
            
    def add_node(self, kb_name: str, label: str, name: str, 
                 properties: Optional[Dict] = None, data: Optional[Dict] = None, 
                 path: str = ''):
        """
        Add a node to the knowledge base.
        
        Args:
            kb_name: Name of the knowledge base
            label: Label for the node
            name: Name of the node
            properties: Optional JSON properties
            data: Optional JSON data
            path: LTREE path for the node
        """
        if not isinstance(kb_name, str):
            raise TypeError("kb_name must be a string")
        if not isinstance(label, str):
            raise TypeError("label must be a string")
        if not isinstance(name, str):
            raise TypeError("name must be a string")
        if not isinstance(path, str):
            raise TypeError("path must be a string")
        if properties is not None and not isinstance(properties, dict):
            raise TypeError("properties must be a dictionary")
        if data is not None and not isinstance(data, dict):
            raise TypeError("data must be a dictionary")
        
        try:
            # Check if kb_name exists in info table
            info_table = f"{self.table_name}_info"
            check_query = f"SELECT 1 FROM {info_table} WHERE knowledge_base = ?"
            
            self.cursor.execute(check_query, (kb_name,))
            if not self.cursor.fetchone():
                raise ValueError(f"Knowledge base '{kb_name}' not found in info table")
            
            # Convert dictionaries to JSON strings
            properties_json = json.dumps(properties) if properties else None
            data_json = json.dumps(data) if data else None
            
            # Insert node
            insert_query = f"""
                INSERT INTO {self.table_name} (knowledge_base, label, name, properties, data, has_link, path)
                VALUES (?, ?, ?, ?, ?, ?, ?)
            """
            
            self.cursor.execute(insert_query, 
                              (kb_name, label, name, properties_json, data_json, 0, path))
            self.conn.commit()
            
        except sqlite3.Error as e:
            self.conn.rollback()
            print(f"Error adding node: {e}")
            raise
        except ValueError as e:
            print(f"Validation error: {e}")
            raise
            
    def add_link(self, parent_kb: str, parent_path: str, link_name: str):
        """
        Add a link between two nodes in the knowledge base.
        
        Args:
            parent_kb: Parent node's knowledge base name
            parent_path: Parent node's path
            link_name: Name of the link
        """
        if not isinstance(parent_kb, str):
            raise TypeError("parent_kb must be a string")
        if not isinstance(parent_path, str):
            raise TypeError("parent_path must be a string")
        if not isinstance(link_name, str):
            raise TypeError("link_name must be a string")
        
        try:
            # Check if knowledge base exists
            info_table = f"{self.table_name}_info"
            kb_check_query = f"SELECT knowledge_base FROM {info_table} WHERE knowledge_base = ?"
            
            self.cursor.execute(kb_check_query, (parent_kb,))
            if not self.cursor.fetchone():
                raise ValueError(f"Parent knowledge base '{parent_kb}' not found")
            
            # Check if parent node exists in the knowledge base
            node_check_query = f"SELECT path FROM {self.table_name} WHERE path = ?"
            
            self.cursor.execute(node_check_query, (parent_path,))
            if not self.cursor.fetchone():
                raise ValueError(f"Parent node with path '{parent_path}' not found")
            
            # Check if link name already exists
            link_name_exists_query = f"SELECT link_name FROM {self.table_name}_link_mount WHERE link_name = ?"
            
            self.cursor.execute(link_name_exists_query, (link_name,))
            if not self.cursor.fetchone():
                raise ValueError(f"Link name '{link_name}' not found in link_mount table")
            
            # Insert link
            link_table = f"{self.table_name}_link"
            link_insert_query = f"""
                INSERT INTO {link_table} (parent_node_kb, parent_path, link_name)
                VALUES (?, ?, ?)
            """
            
            self.cursor.execute(link_insert_query, (parent_kb, parent_path, link_name))
            
            # Update has_link flag for parent node
            update_query = f"UPDATE {self.table_name} SET has_link = 1 WHERE path = ?"
            
            self.cursor.execute(update_query, (parent_path,))
            
            self.conn.commit()
            
        except sqlite3.Error as e:
            self.conn.rollback()
            print(f"Error adding link: {e}")
            raise
        except ValueError as e:
            print(f"Validation error: {e}")
            raise

    def add_link_mount(self, knowledge_base: str, path: str, link_mount_name: str, 
                      description: str = ""):
        """
        Add a link node by verifying prerequisites and updating the has_link_mount flag.
        
        Args:
            knowledge_base: The knowledge base identifier
            path: The LTREE path in the knowledge base
            link_mount_name: The name of the link mount
            description: The description of the link mount
            
        Returns:
            tuple: (knowledge_base, mount_path) on successful completion
            
        Raises:
            ValueError: If any validation fails
            RuntimeError: If database operations fail
            sqlite3.Error: If database connection/transaction fails
        """
        if not isinstance(knowledge_base, str):
            raise TypeError("knowledge_base must be a string")
        if not isinstance(path, str):
            raise TypeError("path must be a string")
        if not isinstance(link_mount_name, str):
            raise TypeError("link_mount_name must be a string")
        if not isinstance(description, str):
            raise TypeError("description must be a string")
        
        try:
            # Step 1: Verify that knowledge_base exists in info table
            info_check_query = f"SELECT knowledge_base FROM {self.table_name}_info WHERE knowledge_base = ?"
            
            self.cursor.execute(info_check_query, (knowledge_base,))
            if not self.cursor.fetchone():
                raise ValueError(f"Knowledge base '{knowledge_base}' does not exist in info table")
            
            # Step 2: Verify that the path exists for the given knowledge base in main table
            path_check_query = f"SELECT id FROM {self.table_name} WHERE knowledge_base = ? AND path = ?"
            
            self.cursor.execute(path_check_query, (knowledge_base, path))
            node_record = self.cursor.fetchone()
            if not node_record:
                raise ValueError(f"Path '{path}' does not exist for knowledge base '{knowledge_base}'")
            
            # Step 3: Verify that link_name does not already exist in link_mount table
            link_name_exists_query = f"SELECT link_name FROM {self.table_name}_link_mount WHERE link_name = ?"
            
            self.cursor.execute(link_name_exists_query, (link_mount_name,))
            if self.cursor.fetchone():
                raise ValueError(f"Link name '{link_mount_name}' already exists in link_mount table")
            
            # Step 4: Insert a record in the link_mount table
            insert_link_mount_record_query = f"""
                INSERT INTO {self.table_name}_link_mount (link_name, knowledge_base, mount_path, description)
                VALUES (?, ?, ?, ?)
            """
            
            self.cursor.execute(insert_link_mount_record_query, 
                              (link_mount_name, knowledge_base, path, description))
            
            if self.cursor.rowcount == 0:
                raise RuntimeError(f"Failed to insert record with link_name '{link_mount_name}'")
            
            # Step 5: Verify that entry with knowledge_base and mount_path exists in main table
            mount_entry_check_query = f"SELECT id FROM {self.table_name} WHERE knowledge_base = ? AND path = ?"
            
            self.cursor.execute(mount_entry_check_query, (knowledge_base, path))
            mount_entry_record = self.cursor.fetchone()
            if not mount_entry_record:
                raise ValueError(f"Entry with knowledge_base '{knowledge_base}' and mount_path '{path}' does not exist")
            
            # Step 6: Set the has_link_mount field to true for the original node
            update_query = f"""
                UPDATE {self.table_name} SET has_link_mount = 1 
                WHERE knowledge_base = ? AND path = ?
            """
            
            self.cursor.execute(update_query, (knowledge_base, path))
            
            # Check if the update was successful
            if self.cursor.rowcount == 0:
                raise RuntimeError(f"No rows were updated for knowledge_base '{knowledge_base}' and path '{path}'")
            
            # Commit the transaction
            self.conn.commit()
            
            return (knowledge_base, path)
            
        except sqlite3.Error as e:
            self.conn.rollback()
            print(f"Database error in add_link_mount: {e}")
            raise
        except Exception as e:
            self.conn.rollback()
            print(f"Unexpected error in add_link_mount: {e}")
            raise

    # === LTREE Query Methods ===
    
    def find_by_pattern(self, pattern: str, kb_name: Optional[str] = None) -> List[sqlite3.Row]:
        """
        Find all nodes matching an ltree pattern.
        
        Supported patterns:
        - Exact: 'kb.test.node'
        - Wildcard: 'kb.*.node' or '*.*.*'
        - Prefix: 'kb.*.GATE*.*'
        - Quantified: 'kb.*{2}.node' or 'kb.*{1,3}.node' or 'kb.*{2,}.node' or 'kb.*{,2}.node'
        
        Args:
            pattern: ltree pattern to match
            kb_name: Optional filter by knowledge base
            
        Returns:
            List of matching rows
        """
        try:
            if kb_name:
                query = f"""
                    SELECT * FROM {self.table_name}
                    WHERE knowledge_base = ? AND ltree_match(path, ?)
                """
                self.cursor.execute(query, (kb_name, pattern))
            else:
                query = f"""
                    SELECT * FROM {self.table_name}
                    WHERE ltree_match(path, ?)
                """
                self.cursor.execute(query, (pattern,))
            
            return self.cursor.fetchall()
            
        except sqlite3.Error as e:
            print(f"Error in find_by_pattern: {e}")
            raise
    
    def find_descendants(self, parent_path: str, kb_name: Optional[str] = None) -> List[sqlite3.Row]:
        """
        Find all descendants of a given path.
        
        Args:
            parent_path: The parent path
            kb_name: Optional filter by knowledge base
            
        Returns:
            List of descendant rows
        """
        try:
            if kb_name:
                query = f"""
                    SELECT * FROM {self.table_name}
                    WHERE knowledge_base = ? AND ltree_descendant(?, path)
                """
                self.cursor.execute(query, (kb_name, parent_path))
            else:
                query = f"""
                    SELECT * FROM {self.table_name}
                    WHERE ltree_descendant(?, path)
                """
                self.cursor.execute(query, (parent_path,))
            
            return self.cursor.fetchall()
            
        except sqlite3.Error as e:
            print(f"Error in find_descendants: {e}")
            raise
    
    def find_ancestors(self, child_path: str, kb_name: Optional[str] = None) -> List[sqlite3.Row]:
        """
        Find all ancestors of a given path.
        
        Args:
            child_path: The child path
            kb_name: Optional filter by knowledge base
            
        Returns:
            List of ancestor rows
        """
        try:
            if kb_name:
                query = f"""
                    SELECT * FROM {self.table_name}
                    WHERE knowledge_base = ? AND ltree_ancestor(?, path)
                """
                self.cursor.execute(query, (kb_name, child_path))
            else:
                query = f"""
                    SELECT * FROM {self.table_name}
                    WHERE ltree_ancestor(?, path)
                """
                self.cursor.execute(query, (child_path,))
            
            return self.cursor.fetchall()
            
        except sqlite3.Error as e:
            print(f"Error in find_ancestors: {e}")
            raise
    
    def get_node_depth(self, path: str) -> int:
        """
        Get the depth of a path.
        
        Args:
            path: The path to measure
            
        Returns:
            Depth as integer
        """
        try:
            query = "SELECT ltree_depth(?)"
            self.cursor.execute(query, (path,))
            result = self.cursor.fetchone()
            return result[0] if result else 0
            
        except sqlite3.Error as e:
            print(f"Error in get_node_depth: {e}")
            raise
    
    def find_by_depth(self, depth: int, kb_name: Optional[str] = None) -> List[sqlite3.Row]:
        """
        Find all nodes at a specific depth.
        
        Args:
            depth: The depth to search for
            kb_name: Optional filter by knowledge base
            
        Returns:
            List of rows at the specified depth
        """
        try:
            if kb_name:
                query = f"""
                    SELECT * FROM {self.table_name}
                    WHERE knowledge_base = ? AND ltree_depth(path) = ?
                """
                self.cursor.execute(query, (kb_name, depth))
            else:
                query = f"""
                    SELECT * FROM {self.table_name}
                    WHERE ltree_depth(path) = ?
                """
                self.cursor.execute(query, (depth,))
            
            return self.cursor.fetchall()
            
        except sqlite3.Error as e:
            print(f"Error in find_by_depth: {e}")
            raise
    
    def find_children(self, parent_path: str, kb_name: Optional[str] = None) -> List[sqlite3.Row]:
        """
        Find immediate children of a path (depth = parent_depth + 1).
        
        Args:
            parent_path: The parent path
            kb_name: Optional filter by knowledge base
            
        Returns:
            List of immediate child rows
        """
        try:
            parent_depth = self.get_node_depth(parent_path)
            child_depth = parent_depth + 1
            
            if kb_name:
                query = f"""
                    SELECT * FROM {self.table_name}
                    WHERE knowledge_base = ? 
                    AND ltree_descendant(?, path)
                    AND ltree_depth(path) = ?
                """
                self.cursor.execute(query, (kb_name, parent_path, child_depth))
            else:
                query = f"""
                    SELECT * FROM {self.table_name}
                    WHERE ltree_descendant(?, path)
                    AND ltree_depth(path) = ?
                """
                self.cursor.execute(query, (parent_path, child_depth))
            
            return self.cursor.fetchall()
            
        except sqlite3.Error as e:
            print(f"Error in find_children: {e}")
            raise


# Example usage
if __name__ == "__main__":
    # SQLite database path
    db_path = "knowledge_base.db"
    
    # Extension path - can be:
    # - None (auto-detect from common locations)
    # - './ltree' (current directory)
    # - '/usr/local/lib/ltree' (installed location)
    # Note: Don't include .so/.dylib - SQLite adds it automatically
    
    # Auto-detect (checks ./ltree, /usr/local/lib/ltree, /usr/lib/ltree)
    # By default, reset=False so existing data is preserved
    kb_manager = KnowledgeBaseManager('knowledge_base', db_path)
    
    # To reset and start fresh, use:
    # kb_manager = KnowledgeBaseManager('knowledge_base', db_path, reset=True)
    
    print("Starting unit test")
    
    try:
        # Add knowledge bases
        kb_manager.add_kb('kb1', 'First knowledge base')
        kb_manager.add_kb('kb2', 'Second knowledge base')
        
        # Add nodes with hierarchical paths
        kb_manager.add_node('kb1', 'person', 'John Doe',
                           {'age': 30}, {'email': 'john@example.com'}, 'people.john')
        kb_manager.add_node('kb1', 'person', 'Jane Doe',
                           {'age': 28}, {'email': 'jane@example.com'}, 'people.jane')
        kb_manager.add_node('kb1', 'child', 'Little John',
                           {'age': 5}, {'parent': 'john'}, 'people.john.children.little_john')
        
        kb_manager.add_node('kb2', 'gate', 'Root Gate',
                           {'type': 'selector'}, {}, 'kb.second_test.GATE_root._0')
        kb_manager.add_node('kb2', 'collection', 'Wait Collection',
                           {'type': 'wait'}, {}, 'kb.second_test.GATE_root._0.COL_wait._1')
        
        # Add link mount
        kb_manager.add_link_mount('kb1', 'people.john', 'link1', 'link1 description')
        
        # Add link
        kb_manager.add_link('kb1', 'people.john', 'link1')
        
        print("\n=== Testing ltree queries ===")
        
        # Test pattern matching
        print("\n1. Find all nodes with 'people' in path:")
        results = kb_manager.find_by_pattern('people.*', 'kb1')
        for row in results:
            print(f"   {row['path']} - {row['name']}")
        
        # Test wildcards
        print("\n2. Find nodes matching kb.*.GATE*.*:")
        results = kb_manager.find_by_pattern('kb.*.GATE*.*', 'kb2')
        for row in results:
            print(f"   {row['path']} - {row['name']}")
        
        # Test descendants
        print("\n3. Find all descendants of 'people.john':")
        results = kb_manager.find_descendants('people.john', 'kb1')
        for row in results:
            print(f"   {row['path']} - {row['name']}")
        
        # Test depth
        print("\n4. Get depth of 'people.john.children.little_john':")
        depth = kb_manager.get_node_depth('people.john.children.little_john')
        print(f"   Depth: {depth}")
        
        # Test find by depth
        print("\n5. Find all nodes at depth 2:")
        results = kb_manager.find_by_depth(2, 'kb1')
        for row in results:
            print(f"   {row['path']} - {row['name']}")
        
        # Test find children
        print("\n6. Find immediate children of 'people':")
        results = kb_manager.find_children('people', 'kb1')
        for row in results:
            print(f"   {row['path']} - {row['name']}")
        
        print("\nUnit test completed successfully")
        
    except Exception as e:
        print(f"Unit test failed: {e}")
        import traceback
        traceback.print_exc()
        
    finally:
        kb_manager.disconnect()
        print("Ending unit test")