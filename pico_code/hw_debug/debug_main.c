#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/uart.h"

// --- Data Logging Flash (U7) SPI Pins ---
#define SPI_PORT spi0
#define FLASH_RX_PIN 16 // DO/IO1 (MISO)
#define FLASH_CS_PIN 17 // HOLD/RESET
#define FLASH_SCK_PIN 18 // CLK
#define FLASH_TX_PIN 19 // DI/IO0 (MOSI)

// --- WiFi/ESP12F UART Pins ---
#define WIFI_UART uart0
#define WIFI_TX_PIN 0
#define WIFI_RX_PIN 1
#define WIFI_BAUD 115200

void test_data_flash();
void test_wifi_module();
void gpio_bounce_test();
void check_esp_alive();

int main() {
    stdio_init_all();
    sleep_ms(3000); // Give USB serial time to connect

    printf("\n========================================\n");
    printf("   ELECTROPHY CORE SYSTEM DEBUG\n");
    printf("========================================\n");

    while (true) {
        printf("\nSelect a subsystem to verify:\n");
        printf("1. Test Data Logging Flash (SPI JEDEC ID Check)\n");
        printf("2. Test ESP12F WiFi Module (UART AT Command Ping)\n");
        printf("3. GPIO Bounce Test (For Multimeter Probing)\n");
        printf("4. Check ESP12F Alive Status\n");
        printf("> ");

        int choice = getchar_timeout_us(0xFFFFFFFF);
        
        switch(choice) {
            case '1': test_data_flash(); break;
            case '2': test_wifi_module(); break;
            case '3': gpio_bounce_test(); break;
            case '4': check_esp_alive(); break;
            case '\r':
            case '\n': break; 
            default: printf("\n[INPUT ERROR] Invalid selection.\n");
        }
    }
    return 0;
}

void test_data_flash() {
    printf("\n--- TESTING DATA LOGGING FLASH (U7) ---\n");
    
    // Initialize SPI at a safe 1MHz for testing
    spi_init(SPI_PORT, 1000 * 1000);
    gpio_set_function(FLASH_RX_PIN, GPIO_FUNC_SPI);
    gpio_set_function(FLASH_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(FLASH_TX_PIN, GPIO_FUNC_SPI);
    
    // Initialize CS pin manually 
    gpio_init(FLASH_CS_PIN);
    gpio_set_dir(FLASH_CS_PIN, GPIO_OUT);
    gpio_put(FLASH_CS_PIN, 1); // Deselect (High)

    sleep_ms(10);

    // Send JEDEC ID Command (0x9F)
    uint8_t tx_buf[] = {0x9F, 0x00, 0x00, 0x00};
    uint8_t rx_buf[4] = {0};

    gpio_put(FLASH_CS_PIN, 0); // Select (Low)
    spi_write_read_blocking(SPI_PORT, tx_buf, rx_buf, 4);
    gpio_put(FLASH_CS_PIN, 1); // Deselect (High)

    printf("JEDEC ID Response: Mfg: 0x%02X, Type: 0x%02X, Capacity: 0x%02X\n", rx_buf[1], rx_buf[2], rx_buf[3]);

    if (rx_buf[1] == 0x00 || rx_buf[1] == 0xFF) {
        printf("[ERROR] Invalid Manufacturer ID.\n");
        printf("HINT: Check for solder bridges on pins 16-19. Ensure U7 is receiving 3.3V.\n");
    } else {
        printf("[SUCCESS] U7 SPI Flash Memory responded correctly!\n");
    }

    spi_deinit(SPI_PORT); 
}

void test_wifi_module() {
    printf("\n--- TESTING ESP12F WIFI MODULE ---\n");
    
    uart_init(WIFI_UART, WIFI_BAUD);
    gpio_set_function(WIFI_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(WIFI_RX_PIN, GPIO_FUNC_UART);

    printf("Sending 'AT' command to ESP12F at %d baud...\n", WIFI_BAUD);
    
    // Flush RX buffer
    while (uart_is_readable(WIFI_UART)) uart_getc(WIFI_UART);

    // Send AT Command
    uart_puts(WIFI_UART, "AT\r\n");

    // Wait up to 500ms for a response
    absolute_time_t timeout = make_timeout_time_ms(500);
    char response[64];
    int idx = 0;

    while (absolute_time_diff_us(get_absolute_time(), timeout) > 0 && idx < 63) {
        if (uart_is_readable(WIFI_UART)) {
            response[idx++] = uart_getc(WIFI_UART);
        }
    }
    response[idx] = '\0'; 

    if (idx > 0) {
        printf("[REPLY]: %s\n", response);
        if (strstr(response, "OK") != NULL) {
            printf("[SUCCESS] ESP12F is alive and responding!\n");
        } else {
            printf("[WARNING] Received data, but no 'OK'. Module might be in bootloader mode.\n");
        }
    } else {
        printf("[ERROR] No response from ESP12F.\n");
        printf("HINT: Is the ESP EN pin pulled high? Are TX/RX swapped on the schematic?\n");
    }

    uart_deinit(WIFI_UART); 
}

void gpio_bounce_test() {
    printf("\n--- GPIO BOUNCE TEST ---\n");
    printf("Toggling GPIOs 10, 11, 12, 13 High and Low every 2 seconds.\n");
    printf("Use your multimeter to probe these pins. Press any key to stop.\n");

    uint gpios_to_test[] = {10, 11, 12, 13}; // Adjust these to whatever open pins you want to probe
    for(int i=0; i<4; i++) {
        gpio_init(gpios_to_test[i]);
        gpio_set_dir(gpios_to_test[i], GPIO_OUT);
    }

    bool state = false;
    while (getchar_timeout_us(0) == PICO_ERROR_TIMEOUT) {
        state = !state;
        for(int i=0; i<4; i++) gpio_put(gpios_to_test[i], state);
        
        if(state) printf("\rPins HIGH (3.3V) ");
        else printf("\rPins LOW (0.0V)  ");
        fflush(stdout);
        
        sleep_ms(2000);
    }
    printf("\nTest stopped.\n");
}

void check_esp_alive() {
    printf("\n--- PROVING ESP12F IS ALIVE ---\n");
    
    // Turn GPIO0 into an Input so it stops fighting the ESP's TX line
    gpio_init(0);
    gpio_set_dir(0, GPIO_IN);

    printf("Listening to GPIO0...\n");
    printf("--> PRESS THE 'SW3' (ESP RESET) BUTTON ON YOUR BOARD NOW! <--\n\n");

    int transitions = 0;
    bool last_state = gpio_get(0);
    
    // Listen for 10 seconds
    absolute_time_t timeout = make_timeout_time_ms(10000);
    while (absolute_time_diff_us(get_absolute_time(), timeout) > 0) {
        bool current_state = gpio_get(0);
        if (current_state != last_state) {
            transitions++;
            last_state = current_state;
        }
    }

    if (transitions > 50) {
        printf("[SUCCESS] Detected %d data toggles on GPIO0!\n", transitions);
        printf("The ESP12F is 100%% alive and transmitting, but the lines are crossed.\n");
    } else {
        printf("[ERROR] No data toggles detected. Transitions: %d\n", transitions);
    }
}