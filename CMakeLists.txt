cmake_minimum_required(VERSION 3.13)

# Target board/platform. DEFAULT = Pico W (RP2040), matching the motioncore setup.
# Build for Pico 2 W (RP2350) from a SEPARATE build dir:
#   cmake -DPICO_BOARD=pico2_w -DPICO_PLATFORM=rp2350 ...
set(PICO_BOARD pico_w CACHE STRING "Board type")
set(PICO_PLATFORM rp2040 CACHE STRING "Platform")

include(pico_sdk_import.cmake)

if(NOT DEFINED ENV{FREERTOS_KERNEL_PATH})
    message(FATAL_ERROR "FREERTOS_KERNEL_PATH must point to the Raspberry Pi FreeRTOS-Kernel fork")
endif()
set(FREERTOS_KERNEL_PATH $ENV{FREERTOS_KERNEL_PATH})
# Pick the FreeRTOS kernel port to match the platform: RP2350 = Cortex-M33 (ARM, no-TrustZone).
if(PICO_PLATFORM MATCHES "rp2350")
    include(${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/RP2350_ARM_NTZ/FreeRTOS_Kernel_import.cmake)
    message(STATUS "slow_bus: FreeRTOS port = RP2350_ARM_NTZ (board=${PICO_BOARD})")
else()
    include(${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/RP2040/FreeRTOS_Kernel_import.cmake)
    message(STATUS "slow_bus: FreeRTOS port = RP2040 (board=${PICO_BOARD})")
endif()

project(slow_bus C CXX ASM)
pico_sdk_init()

# --- chip-independent protocol core -----------------------------------------
set(CORE_DIR ${CMAKE_CURRENT_LIST_DIR}/core)
set(CORE_SRCS
    ${CORE_DIR}/bus_crc8.c
    ${CORE_DIR}/bus_frame.c
    ${CORE_DIR}/bus_roster.c
    ${CORE_DIR}/bus_sched.c
    ${CORE_DIR}/bus_node.c
)

# --- port layer: SHARED PHY/board .c (platform-agnostic PIO) + per-board header -
# board.c / phy_pio_rs485.c / rs485.pio work on both RP2040 and RP2350 (PIO + SDK
# clock APIs auto-adapt). Only board.h (pin map) + lwipopts.h are per-board.
set(PORT_DIR ${CMAKE_CURRENT_LIST_DIR}/port/rp2040)        # shared port sources
if(PICO_PLATFORM MATCHES "rp2350")
    set(BOARD_DIR ${CMAKE_CURRENT_LIST_DIR}/port/rp2350)   # Pico 2 W board.h
else()
    set(BOARD_DIR ${PORT_DIR})                             # Pico W board.h
endif()
set(PORT_SRCS
    ${PORT_DIR}/board.c
    ${PORT_DIR}/phy_pio_rs485.c
)

set(LIBCOMM_DIR ${PORT_DIR}/vendor/libcomm)
# BOARD_DIR first so #include "board.h" resolves to the active board's pin map.
set(COMMON_INC ${CORE_DIR} ${BOARD_DIR} ${PORT_DIR} ${CMAKE_CURRENT_LIST_DIR})

# bus_controller is the "Pico W equivalent" image: Pico W and Pico 2 W share the
# SAME physical pinout, so it always uses the RP2040 board.h (in PORT_DIR) and
# builds unchanged on RP2350 — no BOARD_DIR (that's the vib_node-only pin map).
set(BC_INC ${CORE_DIR} ${PORT_DIR} ${CMAKE_CURRENT_LIST_DIR})

# Helper: define a firmware image for a role with its own uplink + extra srcs.
# WIFI=1 links the CYW43 stack (BC + UPLINK=wifi); slaves never set it.
function(slow_bus_image name)
    cmake_parse_arguments(A "WIFI" "" "SRCS;DEFS" ${ARGN})
    add_executable(${name} ${A_SRCS} ${CORE_SRCS} ${PORT_SRCS})
    pico_generate_pio_header(${name} ${PORT_DIR}/rs485.pio)   # rs485.pio.h for the PHY
    target_include_directories(${name} PRIVATE ${COMMON_INC})
    target_compile_definitions(${name} PRIVATE ${A_DEFS})
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(${name}
        pico_stdlib
        hardware_pio
        hardware_dma
        hardware_pwm
        hardware_adc
        hardware_spi
        hardware_i2c
        FreeRTOS-Kernel
        FreeRTOS-Kernel-Heap4
    )
    if(A_WIFI)
        target_link_libraries(${name} pico_cyw43_arch_lwip_sys_freertos)
    else()
        target_link_libraries(${name} pico_async_context_freertos)
    endif()
    pico_enable_stdio_usb(${name} 1)
    pico_enable_stdio_uart(${name} 0)
    pico_add_extra_outputs(${name})
endfunction()

# Bus controller — FreeRTOS-SMP (core0 = bus_control + uplink threads), USB-CDC
# libcomm host link + autonomous poll sweep + liveness. Dedicated target (not
# slow_bus_image): brings host_link, the commission block, and the chassis
# hardening, not the bus_sched/bus_node skeleton. WiFi uplink + core1 app deferred.
# core1 chain_tree engine (static-link) + the KB0 generated chain (gc-sections trims).
set(BC_DIR ${CMAKE_CURRENT_LIST_DIR}/app/bus_controller)
# Shared node stack (identity/config/roster/CBOR), used by BOTH the BC and slave.
set(NODE_DIR ${CMAKE_CURRENT_LIST_DIR}/node)
file(GLOB KB0_ENGINE_SRCS CONFIGURE_DEPENDS ${BC_DIR}/engine/src/*.c)
file(GLOB KB0_INCR_SRCS   CONFIGURE_DEPENDS ${BC_DIR}/kb0/incr/*.c)

set(BC_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/app/bus_controller/main.c
    ${NODE_DIR}/boot_identity.c
    ${NODE_DIR}/boot_hwio.c          # frozen HIL pin-role map ('hwio' config file)
    ${NODE_DIR}/boot_netcfg.c        # WiFi creds + zenoh-agent endpoint ('neti' config file)
    ${NODE_DIR}/boot_roster.c
    ${NODE_DIR}/cfg_file.c
    ${NODE_DIR}/interlock/interlock.c       # Thread 2: ported SAMD21 interlock framework
    ${NODE_DIR}/interlock/interlock_dsl.c   #           DSL parser ('ilcf' text)
    ${NODE_DIR}/interlock/il_hal_rp2040.c   #           pin HAL (RP2040 SIO + shared ADC)
    ${NODE_DIR}/interlock/il_pin_table.c    #           DSL pin-name resolution
    ${NODE_DIR}/node_role.c          # slave/node role (one image, role from config)
    ${CORE_DIR}/bus_node.c           # node responder core (used by the slave role)
    ${CMAKE_CURRENT_LIST_DIR}/core/host_link.c
    ${CMAKE_CURRENT_LIST_DIR}/app/chassis/commission.c
    ${PORT_DIR}/board.c              # board_init (used by the node/slave role)
    ${PORT_DIR}/phy_pio_rs485.c
    ${PORT_DIR}/cfl_timer_rp2040.c
    ${CORE_DIR}/bus_frame.c
    ${CORE_DIR}/bus_crc8.c
    ${LIBCOMM_DIR}/frame.c
    ${KB0_ENGINE_SRCS}
    ${KB0_INCR_SRCS}
    ${BC_DIR}/kb0/user_functions.c)

# One definition for both images. WIFI -> CYW43/lwIP + UPLINK_WIFI (host_link over TCP
# to the zenoh-agent); without WIFI -> USB-CDC uplink + the plain async context.
function(define_bus_controller name)
    cmake_parse_arguments(A "WIFI" "" "" ${ARGN})
    add_executable(${name} ${BC_SRCS})
    pico_generate_pio_header(${name} ${PORT_DIR}/rs485.pio)
    pico_generate_pio_header(${name} ${PORT_DIR}/servo_bank.pio)              # 8-servo bank
    target_include_directories(${name} PRIVATE
        ${BC_INC} ${NODE_DIR} ${NODE_DIR}/interlock ${LIBCOMM_DIR} ${CMAKE_CURRENT_LIST_DIR}/app/chassis
        ${BC_DIR}/engine/include ${BC_DIR}/kb0/incr)
    target_compile_definitions(${name} PRIVATE
        PICO_PRINTF_SUPPORT_FLOAT=0 PICO_STDIO_ENABLE_CRLF_SUPPORT=0 CFL_32BIT=1)
    # Force-include the printf-stub so the chain_tree runtime's diagnostics can't
    # corrupt the binary USB frame stream. -Wno-* keeps the vendored engine quiet.
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wno-unused-parameter
        $<$<COMPILE_LANGUAGE:C>:-include>
        $<$<COMPILE_LANGUAGE:C>:${BC_DIR}/engine/cfl_embed_stubs.h>)
    target_link_options(${name} PRIVATE -Wl,--gc-sections)
    target_link_libraries(${name}
        pico_stdlib pico_multicore hardware_watchdog hardware_flash pico_unique_id
        hardware_pio hardware_clocks hardware_adc hardware_pwm hardware_i2c pico_i2c_slave
        FreeRTOS-Kernel FreeRTOS-Kernel-Heap4)
    # §dual-transport (2026-06-29): CYW43/lwIP ALWAYS linked -> ONE image does USB-CDC, WiFi/UDP,
    # or standalone, selected at runtime (USB preempts). Was two targets (bus_controller [USB] +
    # bus_controller_wifi [WIFI]); collapsed to one. cyw43_arch_lwip_sys_freertos brings its own
    # FreeRTOS async context (replaces pico_async_context_freertos).
    target_link_libraries(${name} pico_cyw43_arch_lwip_sys_freertos)
    pico_enable_stdio_usb(${name} 1)
    pico_enable_stdio_uart(${name} 0)
    pico_add_extra_outputs(${name})
endfunction()

define_bus_controller(bus_controller)            # §dual-transport: USB + WiFi + standalone, runtime-selected
# Opt-in i2c0->i2c1 loopback self-test (claims GP20/21 as i2c0 slave 0x42).
# Configure with -DI2C_SELFTEST=ON to build it in.
option(I2C_SELFTEST "i2c0->i2c1 loopback self-test fixture" OFF)
if(I2C_SELFTEST)
    target_compile_definitions(bus_controller PRIVATE I2C_SELFTEST)
endif()

# ---- W2a: standalone WiFi smoke test (CYW43 + lwIP join from 'neti') --------
# De-risks the brand-new WiFi stack independent of the BC image. Text stdio over
# USB-CDC. lwipopts.h lives in PORT_DIR (on COMMON_INC). Build: pi-build.sh wifi_test
add_executable(wifi_test
    ${CMAKE_CURRENT_LIST_DIR}/app/wifi_test/main.c
    ${NODE_DIR}/boot_netcfg.c
    ${NODE_DIR}/cfg_file.c
    ${CORE_DIR}/bus_crc8.c)
target_include_directories(wifi_test PRIVATE ${COMMON_INC} ${NODE_DIR})
target_link_libraries(wifi_test
    pico_stdlib pico_cyw43_arch_lwip_sys_freertos hardware_flash
    FreeRTOS-Kernel FreeRTOS-Kernel-Heap4)
pico_enable_stdio_usb(wifi_test 1)
pico_enable_stdio_uart(wifi_test 0)
pico_add_extra_outputs(wifi_test)

# ---- Thread 2 (interlock) compile-check ------------------------------------
# Builds the ported interlock module in ISOLATION so the main image stays green
# while the port is staged. An OBJECT library (no link step), so the platform-
# seam externs (il_plat_*) are resolved later when Thread 2 is wired into the
# image. Build with: tools/pi-build.sh il_thread2_check
add_library(il_thread2_check OBJECT
    ${NODE_DIR}/interlock/il_hal_rp2040.c
    ${NODE_DIR}/interlock/interlock.c
    ${NODE_DIR}/interlock/interlock_dsl.c
    ${NODE_DIR}/interlock/il_pin_table.c)
target_include_directories(il_thread2_check PRIVATE ${NODE_DIR}/interlock)
target_link_libraries(il_thread2_check PRIVATE pico_stdlib hardware_gpio hardware_watchdog)
target_compile_options(il_thread2_check PRIVATE -Wall -Wextra -Werror)

# NOTE (2026-06-22): the separate `slave` target is GONE. One image now serves
# both roles -- the `bus_controller` target above builds the unified binary, and
# main() dispatches to the slave node role (node/node_role.c) at boot when the
# config 'vr' is a non-master variant. Same .uf2 on every board; only the flashed
# config (idnt: variant + address) differs. (The old app/slave/main.c was folded
# into node/node_role.c.)

# ---------------------------------------------------------------------------
# Chassis bring-up image (Pico W / RP2040): pure FreeRTOS-SMP + heartbeat-gated
# HW watchdog with stub threads. No bus core / PHY yet — validates cores +
# threads + 10 ms tick + watchdog in isolation before the PHY/engine are wired.
# ---------------------------------------------------------------------------
set(CHASSIS_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/app/chassis/main.c
    ${CMAKE_CURRENT_LIST_DIR}/app/chassis/commission.c)

# Embedded-friendly printf (no float/exponential). Optional extra DEFS per image.
function(chassis_image name)
    cmake_parse_arguments(A "" "" "DEFS" ${ARGN})
    add_executable(${name} ${CHASSIS_SRCS})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_LIST_DIR})  # FreeRTOSConfig.h
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    target_compile_definitions(${name} PRIVATE
        PICO_PRINTF_SUPPORT_FLOAT=0 PICO_PRINTF_SUPPORT_EXPONENTIAL=0 ${A_DEFS})
    target_link_libraries(${name}
        pico_stdlib pico_multicore hardware_watchdog hardware_flash
        pico_async_context_freertos FreeRTOS-Kernel FreeRTOS-Kernel-Heap4)
    pico_enable_stdio_usb(${name} 1)
    pico_enable_stdio_uart(${name} 0)
    pico_add_extra_outputs(${name})
endfunction()

chassis_image(chassis)                                    # production chassis
chassis_image(chassis_stall      DEFS CHASSIS_STALL_TEST=1)       # WDT negative test
chassis_image(chassis_commission DEFS CHASSIS_COMMISSION_TEST=1)  # writes a test identity

# ---------------------------------------------------------------------------
# PIO PHY internal-loopback self-test: PHY_RX_PIN=BUS_PIN_DI makes the RX SM
# sample the TX pin on-chip, so the 9-bit PHY loops back with NO external wiring
# (works on both RP2040 and RP2350 — board.h comes from BOARD_DIR).
# ---------------------------------------------------------------------------
add_executable(phy_test
    ${CMAKE_CURRENT_LIST_DIR}/app/phy_test/main.c
    ${PORT_DIR}/phy_pio_rs485.c)
pico_generate_pio_header(phy_test ${PORT_DIR}/rs485.pio)
target_include_directories(phy_test PRIVATE ${COMMON_INC})
target_compile_definitions(phy_test PRIVATE PICO_PRINTF_SUPPORT_FLOAT=0 PHY_RX_PIN=BUS_PIN_DI)
target_compile_options(phy_test PRIVATE -Wall -Wextra)
target_link_libraries(phy_test pico_stdlib hardware_pio hardware_clocks)
pico_enable_stdio_usb(phy_test 1)
pico_enable_stdio_uart(phy_test 0)
pico_add_extra_outputs(phy_test)

# ---------------------------------------------------------------------------
# vib_node — Pico 2 W (RP2350) vibration/condition-monitoring node. NEW app that
# reuses the shared bus core + node_role responder (bus_controller is untouched)
# AND the vendored CMSIS-DSP measurement toolkit (FFT/cepstrum + FIR decimation).
# RP2350-ONLY (CMSIS-DSP targets the M33 DSP ext) -> guarded so the RP2040 build
# never compiles it. Build:  cmake -DPICO_BOARD=pico2_w -DPICO_PLATFORM=rp2350
# ---------------------------------------------------------------------------
if(PICO_PLATFORM MATCHES "rp2350")
# CMSIS-DSP subset (f32 FIR decimate + arm_rfft_fast_f32), vendored from xiao_blocks.
set(CMSIS_DSP_DIR ${CMAKE_CURRENT_LIST_DIR}/vendor/cmsis_dsp)
set(CMSIS_DSP_SRCS
    ${CMSIS_DSP_DIR}/Source/FilteringFunctions/arm_fir_decimate_f32.c
    ${CMSIS_DSP_DIR}/Source/FilteringFunctions/arm_fir_decimate_init_f32.c
    ${CMSIS_DSP_DIR}/Source/TransformFunctions/arm_rfft_fast_f32.c
    ${CMSIS_DSP_DIR}/Source/TransformFunctions/arm_rfft_fast_init_f32.c
    ${CMSIS_DSP_DIR}/Source/TransformFunctions/arm_cfft_f32.c
    ${CMSIS_DSP_DIR}/Source/TransformFunctions/arm_cfft_init_f32.c
    ${CMSIS_DSP_DIR}/Source/TransformFunctions/arm_cfft_radix8_f32.c
    ${CMSIS_DSP_DIR}/Source/TransformFunctions/arm_bitreversal2.c
    ${CMSIS_DSP_DIR}/Source/CommonTables/arm_common_tables.c
    ${CMSIS_DSP_DIR}/Source/CommonTables/arm_const_structs.c)
set(DSP_DIR ${CMAKE_CURRENT_LIST_DIR}/port/rp2350/dsp)
add_executable(vib_node
    ${CMAKE_CURRENT_LIST_DIR}/app/vib_node/main.c
    ${NODE_DIR}/node_role.c
    ${NODE_DIR}/boot_identity.c
    ${NODE_DIR}/cfg_file.c
    ${NODE_DIR}/interlock/interlock.c       # Thread-2 COMMON interlock engine
    ${NODE_DIR}/interlock/interlock_dsl.c   #           DSL parser ('ilcN' config)
    ${NODE_DIR}/interlock/il_hal_rp2040.c   #           pin HAL (SDK gpio; works on RP2350)
    ${NODE_DIR}/interlock/il_pin_table.c    #           DSL pin-name resolution
    ${CORE_DIR}/bus_node.c
    ${CORE_DIR}/bus_frame.c
    ${CORE_DIR}/bus_crc8.c
    ${DSP_DIR}/spectral.c
    ${DSP_DIR}/pipeline.c
    ${CMAKE_CURRENT_LIST_DIR}/port/rp2350/adc_capture.c
    ${CMSIS_DSP_SRCS}
    ${PORT_SRCS})
pico_generate_pio_header(vib_node ${PORT_DIR}/rs485.pio)
target_include_directories(vib_node PRIVATE ${COMMON_INC} ${NODE_DIR} ${NODE_DIR}/interlock ${DSP_DIR} ${CMSIS_DSP_DIR}/Include)
target_compile_definitions(vib_node PRIVATE PICO_PRINTF_SUPPORT_FLOAT=0)
target_compile_options(vib_node PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(vib_node
    pico_stdlib pico_multicore hardware_pio hardware_adc hardware_pwm
    hardware_watchdog hardware_flash
    cmsis_core FreeRTOS-Kernel FreeRTOS-Kernel-Heap4)
pico_enable_stdio_usb(vib_node 1)
pico_enable_stdio_uart(vib_node 0)
pico_add_extra_outputs(vib_node)
endif()

# ---------------------------------------------------------------------------
# 2-node bring-up: BC polls the SAMD21 slave (addr 3) on the real wire (RX=GP21).
# ---------------------------------------------------------------------------
add_executable(bus2_test
    ${CMAKE_CURRENT_LIST_DIR}/app/bus2_test/main.c
    ${PORT_DIR}/phy_pio_rs485.c
    ${CORE_DIR}/bus_frame.c
    ${CORE_DIR}/bus_crc8.c)
pico_generate_pio_header(bus2_test ${PORT_DIR}/rs485.pio)
target_include_directories(bus2_test PRIVATE ${COMMON_INC})
target_compile_definitions(bus2_test PRIVATE PICO_PRINTF_SUPPORT_FLOAT=0)
target_compile_options(bus2_test PRIVATE -Wall -Wextra)
target_link_libraries(bus2_test pico_stdlib hardware_pio hardware_clocks)
pico_enable_stdio_usb(bus2_test 1)
pico_enable_stdio_uart(bus2_test 0)
pico_add_extra_outputs(bus2_test)

# DATA path: BC sends CMD_ECHO to slave 3, ACK + reply round-trip.
add_executable(bus_data_test
    ${CMAKE_CURRENT_LIST_DIR}/app/bus_data_test/main.c
    ${PORT_DIR}/phy_pio_rs485.c
    ${CORE_DIR}/bus_frame.c
    ${CORE_DIR}/bus_crc8.c)
pico_generate_pio_header(bus_data_test ${PORT_DIR}/rs485.pio)
target_include_directories(bus_data_test PRIVATE ${COMMON_INC})
target_compile_definitions(bus_data_test PRIVATE PICO_PRINTF_SUPPORT_FLOAT=0)
target_compile_options(bus_data_test PRIVATE -Wall -Wextra)
target_link_libraries(bus_data_test pico_stdlib hardware_pio hardware_clocks)
pico_enable_stdio_usb(bus_data_test 1)
pico_enable_stdio_uart(bus_data_test 0)
pico_add_extra_outputs(bus_data_test)

# ---------------------------------------------------------------------------
# Uplink bring-up: USB-CDC libcomm host link (northbound) + inline bus bridge
# (southbound). The Pi-side linux/bus_controller drives the sync ladder, sees
# identity=bus_controller, and bridges commands through to SAMD21 slave 3.
# Binary frames on USB -> CRLF translation compiled out; raw byte I/O only.
# ---------------------------------------------------------------------------
add_executable(uplink_test
    ${CMAKE_CURRENT_LIST_DIR}/app/uplink_test/main.c
    ${CMAKE_CURRENT_LIST_DIR}/core/host_link.c
    ${LIBCOMM_DIR}/frame.c
    ${PORT_DIR}/phy_pio_rs485.c
    ${CORE_DIR}/bus_frame.c
    ${CORE_DIR}/bus_crc8.c)
pico_generate_pio_header(uplink_test ${PORT_DIR}/rs485.pio)
target_include_directories(uplink_test PRIVATE ${COMMON_INC} ${LIBCOMM_DIR})
target_compile_definitions(uplink_test PRIVATE
    PICO_PRINTF_SUPPORT_FLOAT=0 PICO_STDIO_ENABLE_CRLF_SUPPORT=0)
target_compile_options(uplink_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(uplink_test pico_stdlib hardware_pio hardware_clocks pico_unique_id)
pico_enable_stdio_usb(uplink_test 1)
pico_enable_stdio_uart(uplink_test 0)
pico_add_extra_outputs(uplink_test)

# Full HIL API suite over the bus (echo/sysinfo/stack/gpio/dac-adc/interlocks).
add_executable(bus_api_test
    ${CMAKE_CURRENT_LIST_DIR}/app/bus_api_test/main.c
    ${PORT_DIR}/phy_pio_rs485.c
    ${CORE_DIR}/bus_frame.c
    ${CORE_DIR}/bus_crc8.c)
pico_generate_pio_header(bus_api_test ${PORT_DIR}/rs485.pio)
target_include_directories(bus_api_test PRIVATE ${COMMON_INC})
target_compile_definitions(bus_api_test PRIVATE PICO_PRINTF_SUPPORT_FLOAT=0)
target_compile_options(bus_api_test PRIVATE -Wall -Wextra)
target_link_libraries(bus_api_test pico_stdlib hardware_pio hardware_clocks)
pico_enable_stdio_usb(bus_api_test 1)
pico_enable_stdio_uart(bus_api_test 0)
pico_add_extra_outputs(bus_api_test)
