cmake_minimum_required(VERSION 3.13)

# Pico W (RP2040). Matches the motioncore firmware/pico setup.
set(PICO_BOARD pico_w CACHE STRING "Board type")
set(PICO_PLATFORM rp2040 CACHE STRING "Platform")

include(pico_sdk_import.cmake)

if(NOT DEFINED ENV{FREERTOS_KERNEL_PATH})
    message(FATAL_ERROR "FREERTOS_KERNEL_PATH must point to the Raspberry Pi FreeRTOS-Kernel fork")
endif()
set(FREERTOS_KERNEL_PATH $ENV{FREERTOS_KERNEL_PATH})
include(${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/RP2040/FreeRTOS_Kernel_import.cmake)

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

# --- RP2040 port (PHY + board glue) -----------------------------------------
set(PORT_DIR ${CMAKE_CURRENT_LIST_DIR}/port/rp2040)
set(PORT_SRCS
    ${PORT_DIR}/board.c
    ${PORT_DIR}/phy_pio_rs485.c
)

set(LIBCOMM_DIR ${PORT_DIR}/vendor/libcomm)
set(COMMON_INC ${CORE_DIR} ${PORT_DIR} ${CMAKE_CURRENT_LIST_DIR})

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
add_executable(bus_controller
    ${CMAKE_CURRENT_LIST_DIR}/app/bus_controller/main.c
    ${CMAKE_CURRENT_LIST_DIR}/core/host_link.c
    ${CMAKE_CURRENT_LIST_DIR}/app/chassis/commission.c
    ${PORT_DIR}/phy_pio_rs485.c
    ${CORE_DIR}/bus_frame.c
    ${CORE_DIR}/bus_crc8.c
    ${LIBCOMM_DIR}/frame.c)
pico_generate_pio_header(bus_controller ${PORT_DIR}/rs485.pio)
target_include_directories(bus_controller PRIVATE
    ${COMMON_INC} ${LIBCOMM_DIR} ${CMAKE_CURRENT_LIST_DIR}/app/chassis)
target_compile_definitions(bus_controller PRIVATE
    PICO_PRINTF_SUPPORT_FLOAT=0 PICO_STDIO_ENABLE_CRLF_SUPPORT=0)
target_compile_options(bus_controller PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(bus_controller
    pico_stdlib pico_multicore hardware_watchdog hardware_flash pico_unique_id
    hardware_pio hardware_clocks pico_async_context_freertos
    FreeRTOS-Kernel FreeRTOS-Kernel-Heap4)
pico_enable_stdio_usb(bus_controller 1)
pico_enable_stdio_uart(bus_controller 0)
pico_add_extra_outputs(bus_controller)

# Slave node: null uplink, node role, WiFi off.
slow_bus_image(slave
    SRCS ${CMAKE_CURRENT_LIST_DIR}/app/slave/main.c
         ${PORT_DIR}/uplink_none.c
    DEFS ROLE_SLAVE=1)

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
# PIO PHY internal-loopback self-test (gating task): RX SM samples the TX pin
# (PHY_RX_PIN=20=GP20) so the 9-bit PHY loops back on-chip, no external wiring.
# ---------------------------------------------------------------------------
add_executable(phy_test
    ${CMAKE_CURRENT_LIST_DIR}/app/phy_test/main.c
    ${PORT_DIR}/phy_pio_rs485.c)
pico_generate_pio_header(phy_test ${PORT_DIR}/rs485.pio)
target_include_directories(phy_test PRIVATE ${COMMON_INC})
target_compile_definitions(phy_test PRIVATE PICO_PRINTF_SUPPORT_FLOAT=0)  # RX=BUS_PIN_RO(GP15): external GP16->GP15 jumper
target_compile_options(phy_test PRIVATE -Wall -Wextra)
target_link_libraries(phy_test pico_stdlib hardware_pio hardware_clocks)
pico_enable_stdio_usb(phy_test 1)
pico_enable_stdio_uart(phy_test 0)
pico_add_extra_outputs(phy_test)

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
