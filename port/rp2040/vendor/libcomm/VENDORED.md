# vendored: libcomm host codec

These files are a verbatim copy of the SAME frame codec the firmware and the
Pi-side `linux/bus_controller` run, so the USB host wire format matches by
construction:

    frame.c / frame.h   SLIP framing + CRC-8/AUTOSAR + byte-ring
    comm.h              address space + link-control opcode catalogue
    bus_config.h        tunable sizes (COMM_PAYLOAD_MAX, ...)
    opcodes.h           OP_* command catalogue (REGISTER / SHELL_EXEC / BUS_* ...)

Source of truth: `motioncore-prototype/samd21/apps/register_dongle/vendor/libcomm/`
(itself lifted from the `ros_planner_ii_mqtt_robot` libcomm tree).

**Do not edit here — re-lift upstream.** Only `frame.c` is compiled; the rest
are headers. This is the *northbound* (host) codec; the *southbound* 9-bit bus
codec is `core/bus_frame.c` (BC-2 frame), a separate format.
