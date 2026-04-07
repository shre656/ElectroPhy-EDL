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
# TARGET 2: On-Board Hardware Debug Suite (For the final RP2350A board)
# ==============================================================================
# Note: Pointing exactly to debug_main.c as seen in your folder structure
add_executable(hw_debug 
    hw_debug/debug_main.c
)
target_link_libraries(hw_debug 
    pico_stdlib 
    hardware_i2c 
    hardware_adc 
    hardware_spi 
    hardware_uart
)
pico_enable_stdio_usb(hw_debug 1)
pico_enable_stdio_uart(hw_debug 0)
pico_add_extra_outputs(hw_debug)


# ==============================================================================
# TARGET 3: External Pico W Sensor Tester (For jumper-wire testing)
# ==============================================================================
add_executable(test_sensors 
    test_sensors.c
)
target_link_libraries(test_sensors 
    pico_stdlib 
    hardware_i2c
    hardware_adc
    hardware_pwm
)
pico_enable_stdio_usb(test_sensors 1)
pico_enable_stdio_uart(test_sensors 0)
pico_add_extra_outputs(test_sensors)