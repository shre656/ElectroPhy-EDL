cmake_minimum_required(VERSION 3.13)

# Include the Pico SDK build system
include(pico_sdk_import.cmake)

project(ElectroPhy_Workspace C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the Pico SDK
pico_sdk_init()
add_subdirectory(pico-microphone)

## ==============================================================================
# TARGET 1: Production Firmware (For the final RP2350A board)
# ==============================================================================
add_executable(electrophy_main 
    main.c
)


# Add these two lines right here!
pico_generate_pio_header(electrophy_main ${CMAKE_CURRENT_LIST_DIR}/hw_debug/uart_tx.pio)
pico_generate_pio_header(electrophy_main ${CMAKE_CURRENT_LIST_DIR}/hw_debug/uart_rx.pio)

target_link_libraries(electrophy_main 
    pico_stdlib 
    m
    hardware_i2c
    hardware_flash
    hardware_adc            # <--- ADDED: For the LDR Light Sensor
    hardware_pio            # <--- ADDED: Required for the mic's hardware clock
    pico_pdm_microphone     # <--- ADDED: The DMA audio library
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
    pico_pdm_microphone   
)
pico_enable_stdio_usb(hw_debug 1)
pico_enable_stdio_uart(hw_debug 0)
pico_add_extra_outputs(hw_debug)

# ==============================================================================
# TARGET 3: External Sensor Tester (Now with Wi-Fi!)
# ==============================================================================
add_executable(test_sensors 
    test_sensors.c
)
target_include_directories(test_sensors PRIVATE ${CMAKE_CURRENT_LIST_DIR}/pico-microphone/include)
# Tell CMake to compile the custom PIO UART scripts for this target too!
pico_generate_pio_header(test_sensors ${CMAKE_CURRENT_LIST_DIR}/hw_debug/uart_tx.pio)
pico_generate_pio_header(test_sensors ${CMAKE_CURRENT_LIST_DIR}/hw_debug/uart_rx.pio)



target_link_libraries(test_sensors 
    pico_stdlib 
    hardware_i2c
    hardware_adc
    hardware_pwm
    hardware_pio     
    pico_pdm_microphone
)
pico_enable_stdio_usb(test_sensors 1)
pico_enable_stdio_uart(test_sensors 0)
pico_add_extra_outputs(test_sensors)

# ==============================================================================
# TARGET 4: USB Audio and DSP Debugger (Microphone & FFT)
# ==============================================================================
add_executable(mic_usb_debug 
    hw_debug/mic_usb_debug.c
)

target_include_directories(mic_usb_debug PRIVATE ${CMAKE_CURRENT_LIST_DIR}/pico-microphone/include)

target_link_libraries(mic_usb_debug 
    pico_stdlib 
    hardware_pio     
    pico_pdm_microphone
    m   # <-- IMPORTANT: Links the math library required for FFT sine/cosine
)

# Force USB only
pico_enable_stdio_usb(mic_usb_debug 1)
pico_enable_stdio_uart(mic_usb_debug 0)
pico_add_extra_outputs(mic_usb_debug)