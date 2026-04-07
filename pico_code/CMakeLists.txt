cmake_minimum_required(VERSION 3.13)

# Include the Pico SDK build system
include(pico_sdk_import.cmake)

project(ElectroPhy_Workspace C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the Pico SDK
pico_sdk_init()

# ==============================================================================
# TARGET 1: Production Firmware (For the final RP2350A board)
# ==============================================================================
add_executable(electrophy_main 
    main.c
)
target_link_libraries(electrophy_main 
    pico_stdlib 
    m
    hardware_i2c
    hardware_flash
)
pico_enable_stdio_usb(electrophy_main 1)
pico_enable_stdio_uart(electrophy_main 0)
pico_add_extra_outputs(electrophy_main)


# ==============================================================================
# TARGET 2: On-Board Hardware Debug Suite
# ==============================================================================
add_executable(hw_debug 
    hw_debug/debug_main.c
)

# Tell CMake to compile the custom PIO UART scripts into C headers
pico_generate_pio_header(hw_debug ${CMAKE_CURRENT_LIST_DIR}/hw_debug/uart_tx.pio)
pico_generate_pio_header(hw_debug ${CMAKE_CURRENT_LIST_DIR}/hw_debug/uart_rx.pio)

target_link_libraries(hw_debug 
    pico_stdlib 
    hardware_i2c 
    hardware_adc 
    hardware_spi 
    hardware_uart
    hardware_pio   # <--- Added PIO hardware library
)
pico_enable_stdio_usb(hw_debug 1)
pico_enable_stdio_uart(hw_debug 0)
pico_add_extra_outputs(hw_debug)

# ==============================================================================
# TARGET 3: External Pico W Sensor Tester (Now with Wi-Fi!)
# ==============================================================================
add_executable(test_sensors 
    test_sensors.c
)

# Tell CMake to compile the custom PIO UART scripts for this target too!
pico_generate_pio_header(test_sensors ${CMAKE_CURRENT_LIST_DIR}/hw_debug/uart_tx.pio)
pico_generate_pio_header(test_sensors ${CMAKE_CURRENT_LIST_DIR}/hw_debug/uart_rx.pio)

target_link_libraries(test_sensors 
    pico_stdlib 
    hardware_i2c
    hardware_adc
    hardware_pwm
    hardware_pio     # <--- THE FIX IS RIGHT HERE!
)
pico_enable_stdio_usb(test_sensors 1)
pico_enable_stdio_uart(test_sensors 0)
pico_add_extra_outputs(test_sensors)