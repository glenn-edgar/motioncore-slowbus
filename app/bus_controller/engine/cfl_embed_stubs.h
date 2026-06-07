/* Force-included when cross-building the chain_tree runtime .a for the target:
 * the runtime's diagnostic printf/fprintf/puts would corrupt the binary USB
 * frame stream, so compile them out. Functional output goes via OP_DBG_LOG. */
#ifndef CFL_EMBED_STUBS_H
#define CFL_EMBED_STUBS_H
#include <stdio.h>
#undef printf
#undef fprintf
#undef puts
#define printf(...)   ((void)0)
#define fprintf(...)  ((void)0)
#define puts(s)       ((void)0)
#endif
