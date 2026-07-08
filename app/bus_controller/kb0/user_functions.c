/* KB0 user functions (embed). The one-shots the generated chain calls; their
 * bodies are weak hooks the firmware (main.c) overrides to talk to the inter-core
 * up-queue. Default no-ops keep the engine link-complete (e.g. host link tests). */
#include <stdint.h>
#include "cfl_runtime.h"

__attribute__((weak)) void kb0_on_ping(void *handle, unsigned node_index)        { (void)handle; (void)node_index; }
__attribute__((weak)) void kb0_on_snapshot(void *handle, unsigned node_index)    { (void)handle; (void)node_index; }
__attribute__((weak)) void kb0_on_cmd_timeout(void *handle, unsigned node_index) { (void)handle; (void)node_index; }
__attribute__((weak)) void kb1_on_adc(void *handle, unsigned node_index)         { (void)handle; (void)node_index; }
__attribute__((weak)) void kbapp_on_echo(void *handle, unsigned node_index)      { (void)handle; (void)node_index; }
__attribute__((weak)) void kbapp_on_echo_to(void *handle, unsigned node_index)   { (void)handle; (void)node_index; }
__attribute__((weak)) void kbapp_on_il_clear(void *handle, unsigned node_index)  { (void)handle; (void)node_index; }
__attribute__((weak)) void kbapp_on_op(void *handle, unsigned node_index)        { (void)handle; (void)node_index; }

void mon_ping_reply_one_shot_fn(void *handle, unsigned node_index) { kb0_on_ping(handle, node_index); }
void mon_snapshot_one_shot_fn(void *handle, unsigned node_index)   { kb0_on_snapshot(handle, node_index); }
void mon_cmd_timeout_one_shot_fn(void *handle, unsigned node_index){ kb0_on_cmd_timeout(handle, node_index); }
void adc_read_one_shot_fn(void *handle, unsigned node_index)       { kb1_on_adc(handle, node_index); }
void app_echo_one_shot_fn(void *handle, unsigned node_index)       { kbapp_on_echo(handle, node_index); }
void app_echo_to_one_shot_fn(void *handle, unsigned node_index)    { kbapp_on_echo_to(handle, node_index); }
void app_il_clear_one_shot_fn(void *handle, unsigned node_index)   { kbapp_on_il_clear(handle, node_index); }
void app_op_one_shot_fn(void *handle, unsigned node_index)         { kbapp_on_op(handle, node_index); }
