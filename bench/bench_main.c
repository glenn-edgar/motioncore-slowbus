/* slow_bus M0+ BC benchmark host harness.
 *
 * Loads the embedded chain_tree image, registers ONLY the functions it uses via
 * the generated per-image stub (cfl_register_image_functions), and runs the 3
 * KBs. Links against the core engine + functions lib ONLY — deliberately NOT
 * json_packets / cbor_packets, to prove the registration stub broke the
 * controlled-node/packet coupling.
 */
#include <stdio.h>
#include <stdbool.h>

#include "cfl_runtime.h"
#include "cfl_image_loader.h"
#include "cfl_file_loader.h"   /* cfl_embedded_load */
#include "bc_benchmark_image.h"
#include "bc_benchmark_registration.h"

static cfl_perm_t perm;
static char perm_buffer[0xffff];   /* 64 KB static permanent pool */

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);   /* line-buffered so output survives */
    cfl_image_loader_t img;
    int rc = cfl_embedded_load(bc_benchmark_image, BC_BENCHMARK_IMAGE_SIZE, &img);
    if (rc != CFL_IMAGE_OK) { printf("image load failed: %d\n", rc); return 1; }
    printf("loaded embedded image: %u bytes\n", BC_BENCHMARK_IMAGE_SIZE);

    int missing = cfl_register_image_functions(&img);
    printf("registration missing: %d\n", missing);
    if (cfl_image_validate(&img) > 0) { printf("validate: unresolved functions\n"); return 1; }

    const cfl_chaintree_handle_t *h = cfl_image_get_handle(&img);
    printf("node_count=%d kb_count=%d\n", h->node_count, h->kb_count);

    cfl_runtime_create_params_t *p = cfl_runtime_create_params_create();
    p->perm = &perm;
    p->perm_buffer = perm_buffer;
    p->perm_buffer_size = (uint16_t)sizeof(perm_buffer);
    p->heap_size = 4096;
    p->max_allocator_count = cfl_calculate_arrena_number(h);
    p->total_node_count = h->node_count;
    p->allocator_0_size = 50;
    p->event_queue_high_priority_size = 8;
    p->event_queue_low_priority_size = 64;
    p->delta_time = 0.1;

    cfl_runtime_handle_t *handle = cfl_runtime_create(&perm, p, h);
    cfl_runtime_create_params_destroy(p);
    if (!handle) { printf("runtime create failed\n"); return 1; }

    cfl_runtime_reset(handle);
    cfl_add_test_by_index(handle, 0);   /* api_measurements */
    cfl_add_test_by_index(handle, 1);   /* interlock_alpha  */
    cfl_add_test_by_index(handle, 2);   /* interlock_beta   */

    bool result = cfl_runtime_run(handle);
    printf("run result=%d  perm_used=%u / %u bytes\n",
           result, cfl_perm_used_bytes(&perm), (unsigned)sizeof(perm_buffer));

    cfl_image_free(&img);
    return result ? 0 : 1;
}
