/* slow_bus benchmark user handlers.
 *
 * Trivial one-shot handler bodies referenced by the benchmark image (the
 * watchdog timeout action). A real BC implements the interlock response here.
 * one_shot signature: void (*)(void *handle, unsigned node_index).
 */

void ila_timeout_one_shot_fn(void *handle, unsigned node_index) {
    (void)handle;
    (void)node_index;
    /* interlock-alpha watchdog timeout action would go here */
}
