cmake_minimum_required(VERSION 3.13)

# Include the Pico SDK build system
include(pico_sdk_import.cmake)

project(electrophy_mock C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the Pico SDK
pico_sdk_init()

# Add our main executable
add_executable(electrophy_mock
    main.c
)

# Pull in the standard libraries and math library for our sine wave
# target_link_libraries(electrophy_mock pico_stdlib m)
target_link_libraries(electrophy_mock pico_stdlib hardware_i2c

    
    hardware_flash
    pico_cyw43_arch_lwip_threadsafe_background
    )
# CRITICAL: Enable USB output, disable UART output
pico_enable_stdio_usb(electrophy_mock 1)
pico_enable_stdio_uart(electrophy_mock 0)

# Create the .uf2 file for dragging and dropping onto the Pico
pico_add_extra_outputs(electrophy_mock)