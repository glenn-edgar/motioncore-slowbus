import sqlite3
import json
from .knowledge_base_manager import KnowledgeBaseManager


class Construct_KB(KnowledgeBaseManager):
    """
    This class is designed to construct a knowledge base structure with header
    and info nodes, using a stack-based approach to manage the path. It also
    manages a connection to a SQLite database and sets up the schema.
    """

    def __init__(self, db_path, table_name="knowledge_base", ltree_extension_path=None, upload_flag=False):
        """
        Initializes the Construct_KB object and connects to the SQLite database.
        Also sets up the database schema.

        Args:
            db_path (str): Path to the SQLite database file
            table_name (str): Base name for the knowledge base tables (default: "knowledge_base")
            ltree_extension_path (str): Path to ltree extension (without .so/.dylib)
                                       If None, will auto-detect from common locations

        """
        self.path = {}  # Stack to keep track of the path (levels/nodes)
        self.path_values = {}
        self.conn = None  # Connection object
        self.cursor = None  # Cursor object
        self.table_name = table_name
        
        # Initialize parent class
        KnowledgeBaseManager.__init__(self, table_name, db_path, ltree_extension_path, upload_flag)
        
        
    def get_db_objects(self):
        """
        Returns both the database connection and cursor objects.

        Returns:
            tuple: A tuple containing (connection, cursor)
                - connection (sqlite3.Connection): The SQLite database connection
                - cursor (sqlite3.Cursor): The SQLite database cursor
        """
        return self.conn, self.cursor

    
    def add_kb(self, kb_name, description=""):
        if kb_name in self.path:
            raise ValueError(f"Knowledge base {kb_name} already exists")
        self.path[kb_name] = [kb_name]
        self.path_values[kb_name] = {}
        KnowledgeBaseManager.add_kb(self, kb_name, description)
      
    def select_kb(self, kb_name):
        if kb_name not in self.path:
            raise ValueError(f"Knowledge base {kb_name} does not exist")
        
        self.working_kb = kb_name
        
        
        
    
   
    def add_header_node(self, link, node_name, node_properties, node_data, description=""):
        """
        Adds a header node to the knowledge base.

        Args:
            link: The link associated with the header node.
            node_name: The name of the header node.
            node_properties: Properties associated with the header node.
            node_data: Data associated with the header node.
            description: Optional description for the node
        """
        
        
        if not isinstance(description, str):
            raise TypeError("description must be a string")
        if not isinstance(node_properties, dict):
            raise TypeError("node_properties must be a dictionary")
        
        if description != "":
            node_properties["description"] = description
            
       

        self.path[self.working_kb].append(link)
        self.path[self.working_kb].append(node_name)
        node_path = ".".join(self.path[self.working_kb])
        

        if node_path in self.path_values[self.working_kb]:
            raise ValueError(f"Path {node_path} already exists in knowledge base")
        
        self.path_values[self.working_kb][node_path] = True
       
        path = ".".join(self.path[self.working_kb])
        print("path", path)
        KnowledgeBaseManager.add_node(self, self.working_kb, link, node_name, node_properties, node_data, path)
       

    def add_info_node(self, link, node_name, node_properties, node_data, description=""):
        self.add_header_node(link, node_name, node_properties, node_data, description)
     
        self.path[self.working_kb].pop()  # Remove node_name
        self.path[self.working_kb].pop()  # Remove link
        
    
    def leave_header_node(self, label, name):
        """
        Leaves a header node, verifying the label and name.
        If an error occurs, the knowledge_base table is deleted if it exists
        and the SQLite connection is terminated.

        Args:
            label: The expected link of the header node.
            name: The expected name of the header node.
        """
        # Try to pop the expected values
        if not self.path:
            raise ValueError("Cannot leave a header node: path is empty")
        
        ref_name = self.path[self.working_kb].pop()
        if not self.path[self.working_kb]:
            # Put the name back and raise an error
            self.path[self.working_kb].append(ref_name)
            raise ValueError("Cannot leave a header node: not enough elements in path")
            
        ref_label = self.path[self.working_kb].pop()
        
        # Verify the popped values
        if ref_name != name or ref_label != label:
            # Create a descriptive error message
            error_msg = []
            if ref_name != name:
                error_msg.append(f"Expected name '{name}', but got '{ref_name}'")
            if ref_label != label:
                error_msg.append(f"Expected label '{label}', but got '{ref_label}'")
                
            raise AssertionError(", ".join(error_msg))
        
        
    def add_link_node(self, link_name):
        KnowledgeBaseManager.add_link(self, self.working_kb, ".".join(self.path[self.working_kb]), link_name)
   
    def add_link_mount(self, link_mount_name, description=""):
        KnowledgeBaseManager.add_link_mount(self, self.working_kb, ".".join(self.path[self.working_kb]), 
                                            link_mount_name, description)
    
    def check_installation(self):
        """
        Checks if the installation is correct by verifying that the path is empty.
        If the path is not empty, the knowledge_base table is deleted if present,
        the database connection is closed, and an exception is raised.
        If the path is empty, the database connection is closed normally.

        Returns:
            bool: True if installation check passed

        Raises:
            RuntimeError: If the path is not empty
        """
        for kb_name in self.path:
            if len(self.path[kb_name]) != 1:
                
                raise RuntimeError(f"Installation check failed: Path is not empty for knowledge base {kb_name}. Path: {self.path[kb_name]}")
            if self.path[kb_name][0] != kb_name:
                raise RuntimeError(f"Installation check failed: Path is not empty for knowledge base {kb_name}. Path: {self.path[kb_name]}")
       

if __name__ == '__main__':
    # Example Usage with SQLite
    DB_PATH = "knowledge_base.db"
    DB_TABLE = "knowledge_base"
    
    # Optional: specify ltree extension path
    # LTREE_EXT = "/usr/local/lib/ltree"  # Will auto-detect if None
    
    print("starting unit test")
    
    # Initialize with auto-detection of ltree extension
    kb = Construct_KB(DB_PATH, DB_TABLE)

    # Test with first knowledge base
    kb.add_kb("kb1", "First knowledge base")
    kb.select_kb("kb1")
    kb.add_header_node("header1_link", "header1_name", {"prop1": "val1"}, {"data": "header1_data"})
   
    kb.add_info_node("info1_link", "info1_name", {"prop2": "val2"}, {"data": "info1_data"})

    kb.leave_header_node("header1_link", "header1_name")
 
    kb.add_header_node("header2_link", "header2_name", {"prop3": "val3"}, {"data": "header2_data"})
    kb.add_info_node("info2_link", "info2_name", {"prop4": "val4"}, {"data": "info2_data"})
    kb.add_link_mount("link1", "link1 description")
    kb.leave_header_node("header2_link", "header2_name")
  
    # Test with second knowledge base
    kb.add_kb("kb2", "Second knowledge base")
    kb.select_kb("kb2")
    kb.add_header_node("header1_link", "header1_name", {"prop1": "val1"}, {"data": "header1_data"})
   
    kb.add_info_node("info1_link", "info1_name", {"prop2": "val2"}, {"data": "info1_data"})

    kb.leave_header_node("header1_link", "header1_name")
   
    kb.add_header_node("header2_link", "header2_name", {"prop3": "val3"}, {"data": "header2_data"})
    kb.add_info_node("info2_link", "info2_name", {"prop4": "val4"}, {"data": "info2_data"})
    kb.add_link_node("link1")
    kb.leave_header_node("header2_link", "header2_name")

    # Example of check_installation
    try:
        kb.check_installation()
        print("✓ Installation check passed")
        kb.disconnect()
        print("✓ Database connection closed")
    except RuntimeError as e:
        print(f"Error during installation check: {e}")
    
    print("ending unit test")
