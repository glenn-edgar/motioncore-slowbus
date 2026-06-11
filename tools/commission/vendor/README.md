# Vendored host-tool dependencies (commissioning)

Self-contained so the repo needs no external paths or system installs.

- **ltree/** — SQLite ltree extension (PostgreSQL-style hierarchical path
  matching). Source `ltree_sqlite.c` + `Makefile`; build with `make` →
  `ltree.so` (init symbol `sqlite3_ltree_init`). Needs `libsqlite3-dev`.
- **construct_kb/** — ChainTree `KnowledgeBaseManager` / `Construct_KB`
  (stack-based, ltree-indexed KB builder). Pure stdlib (sqlite3/json/typing).

Provenance: `~/knowledge_base/kb_modules/kb_python` (MIT). config_tree.py loads
both from here via `tools/commission/vendor/` on sys.path.
