/* KB0 user functions (embed). Bodies wired to the inter-core up-queue during
 * app_engine integration; for now they prove the binding + dispatch. */
#include <stdint.h>
#include "cfl_runtime.h"

/* weak hooks the firmware overrides; host link-test uses these no-op defaults. */
__attribute__((weak)) void kb0_on_ping(void *handle, unsigned node_index) {
    (void)handle; (void)node_index;
}
__attribute__((weak)) void kb0_on_cmd_timeout(void *handle, unsigned node_index) {
    (void)handle; (void)node_index;
}

void mon_ping_reply_one_shot_fn(void *handle, unsigned node_index) {
    kb0_on_ping(handle, node_index);
}
void mon_cmd_timeout_one_shot_fn(void *handle, unsigned node_index) {
    kb0_on_cmd_timeout(handle, node_index);
}
