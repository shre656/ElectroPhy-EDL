#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/spi.h"
#include "hardware/uart.h"
#include "hardware/pio.h"     
#include "uart_tx.pio.h"      
#include "uart_rx.pio.h"      

// ==========================================
// CHANGE THESE TO YOUR ACTUAL DETAILS! 
// ==========================================
#define WIFI_SSID "Aaaaaa"
#define WIFI_PASS "12345678"
#define TARGET_IP "10.51.21.194"  // <-- Put your Mac's IP address here!
#define TARGET_PORT "8080"        // We will listen on port 8080
// ==========================================

// --- MPU6050 I2C Pins (i2c1) ---
#define I2C_PORT i2c1
#define SDA_PIN 2
#define SCL_PIN 3
#define ADDR_MPU6050 0x68

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

// Function Prototypes
void test_data_flash();
void test_wifi_module();
void gpio_bounce_test();
void check_esp_alive();
void pio_uart_passthrough();
void wifi_mpu_stream();
bool send_at_cmd(PIO pio, uint sm_tx, uint sm_rx, const char* cmd, const char* expected, uint32_t timeout_ms);

int main() {
    stdio_init_all();
    sleep_ms(3000); 

    // Initialize I2C for the MPU
    i2c_init(I2C_PORT, 100 * 1000);
    gpio_set_function(SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(SDA_PIN);
    gpio_pull_up(SCL_PIN);

    printf("\n========================================\n");
    printf("   ELECTROPHY CORE SYSTEM DEBUG\n");
    printf("========================================\n");

    while (true) {
        printf("\nSelect a subsystem to verify:\n");
        printf("1. Test Data Logging Flash (SPI JEDEC ID Check)\n");
        printf("2. Test ESP12F WiFi Module (Automated PIO Ping)\n");
        printf("3. GPIO Bounce Test (For Multimeter Probing)\n");
        printf("4. Check ESP12F Alive Status\n");
        printf("5. Live WiFi Passthrough Terminal (Type to ESP12F)\n");
        printf("6. LIVE WI-FI SENSOR STREAM (MPU -> Mac)\n");
        printf("> ");

        int choice = getchar_timeout_us(0xFFFFFFFF);
        
        switch(choice) {
            case '1': test_data_flash(); break;
            case '2': test_wifi_module(); break;
            case '3': gpio_bounce_test(); break;
            case '4': check_esp_alive(); break;
            case '5': pio_uart_passthrough(); break;
            case '6': wifi_mpu_stream(); break;
            case '\r':
            case '\n': break; 
            default: printf("\n[INPUT ERROR] Invalid selection.\n");
        }
    }
    return 0;
}

// Helper function to send AT commands and wait for a specific response
bool send_at_cmd(PIO pio, uint sm_tx, uint sm_rx, const char* cmd, const char* expected, uint32_t timeout_ms) {
    while (pio_uart_rx_readable(pio, sm_rx)) pio_uart_rx_getc(pio, sm_rx); // Flush buffer
    pio_uart_tx_puts(pio, sm_tx, cmd); // Send command
    
    absolute_time_t timeout = make_timeout_time_ms(timeout_ms);
    char buf[512]; 
    int idx = 0;

    while (absolute_time_diff_us(get_absolute_time(), timeout) > 0) {
        if (pio_uart_rx_readable(pio, sm_rx)) {
            buf[idx] = pio_uart_rx_getc(pio, sm_rx);
            idx++;
            if (idx >= 511) break;
            buf[idx] = '\0';
            if (strstr(buf, expected) != NULL) {
                return true; // Found the expected string!
            }
            if (strstr(buf, "ERROR") != NULL) {
                return false; // ESP rejected the command
            }
        }
    }
    return false; // Timed out
}

void wifi_mpu_stream() {
    printf("\n--- INITIALIZING WI-FI SENSOR STREAM ---\n");

    // 1. Setup PIO UART
    PIO pio = pio0;
    uint sm_tx = pio_claim_unused_sm(pio, true);
    uint sm_rx = pio_claim_unused_sm(pio, true);
    uint offset_tx = pio_add_program(pio, &uart_tx_program);
    uint offset_rx = pio_add_program(pio, &uart_rx_program);
    pio_uart_tx_init(pio, sm_tx, offset_tx, WIFI_RX_PIN, WIFI_BAUD); 
    pio_uart_rx_init(pio, sm_rx, offset_rx, WIFI_TX_PIN, WIFI_BAUD);

    // 2. Wake up the MPU-6050
    uint8_t wake_cmd[] = {0x6B, 0x00};
    i2c_write_timeout_us(I2C_PORT, ADDR_MPU6050, wake_cmd, 2, false, 25000);

    // 3. Configure ESP12F
    printf("1/4 Resetting Wi-Fi Module...\n");
    send_at_cmd(pio, sm_tx, sm_rx, "AT+RST\r\n", "ready", 5000);
    sleep_ms(1000);

    printf("2/4 Setting Station Mode...\n");
    send_at_cmd(pio, sm_tx, sm_rx, "AT+CWMODE=1\r\n", "OK", 2000);

    printf("3/4 Connecting to Wi-Fi (%s). This takes a few seconds...\n", WIFI_SSID);
    char join_cmd[128];
    sprintf(join_cmd, "AT+CWJAP=\"%s\",\"%s\"\r\n", WIFI_SSID, WIFI_PASS);
    if (!send_at_cmd(pio, sm_tx, sm_rx, join_cmd, "WIFI GOT IP", 10000)) {
        printf("[ERROR] Failed to connect to Wi-Fi. Check SSID/Password.\n");
        return;
    }

    printf("4/4 Opening UDP Socket to Mac (%s:%s)...\n", TARGET_IP, TARGET_PORT);
    char udp_cmd[128];
    sprintf(udp_cmd, "AT+CIPSTART=\"UDP\",\"%s\",%s\r\n", TARGET_IP, TARGET_PORT);
    if (!send_at_cmd(pio, sm_tx, sm_rx, udp_cmd, "OK", 3000)) {
        printf("[ERROR] Failed to open socket.\n");
        return;
    }

    printf("\n[SUCCESS] STREAMING LIVE DATA! Open the listener on your Mac!\n");
    printf("Press any key to stop...\n\n");

    // 4. The Live Streaming Loop
    while (getchar_timeout_us(0) == PICO_ERROR_TIMEOUT) {
        // Read MPU Data
        uint8_t accel_reg = 0x3B;
        uint8_t buffer[6];
        i2c_write_timeout_us(I2C_PORT, ADDR_MPU6050, &accel_reg, 1, true, 25000);
        i2c_read_timeout_us(I2C_PORT, ADDR_MPU6050, buffer, 6, false, 25000);

        int16_t raw_ax = (buffer[0] << 8) | buffer[1];
        int16_t raw_ay = (buffer[2] << 8) | buffer[3];
        int16_t raw_az = (buffer[4] << 8) | buffer[5];
        float ax = raw_ax / 16384.0f;
        float ay = raw_ay / 16384.0f;
        float az = raw_az / 16384.0f;

        // Format the Payload
        char payload[64];
        sprintf(payload, "Ax: %.2f  |  Ay: %.2f  |  Az: %.2f\n", ax, ay, az);
        int len = strlen(payload);

        // Tell ESP how many bytes we are about to send
        char send_cmd[32];
        sprintf(send_cmd, "AT+CIPSEND=%d\r\n", len);
        
        if (send_at_cmd(pio, sm_tx, sm_rx, send_cmd, ">", 1000)) {
            // ESP is ready (> prompt received), send the actual sensor data
            pio_uart_tx_puts(pio, sm_tx, payload);
            
            // Wait for ESP to confirm it was sent over the air
            send_at_cmd(pio, sm_tx, sm_rx, "", "SEND OK", 1000);
        }

        printf("\rSent via Wi-Fi: %s", payload); // Remove newline for clean terminal printing
        printf("\033[A"); // Move cursor up to overwrite the same line in your serial monitor
        
        sleep_ms(100); // 10Hz transmit rate
    }

    printf("\n\nStopping Stream...\n");
    send_at_cmd(pio, sm_tx, sm_rx, "AT+CIPCLOSE\r\n", "OK", 2000);
    
    // Cleanup PIO
    pio_sm_set_enabled(pio, sm_tx, false);
    pio_sm_set_enabled(pio, sm_rx, false);
    pio_remove_program(pio, &uart_tx_program, offset_tx);
    pio_remove_program(pio, &uart_rx_program, offset_rx);
}

void test_data_flash() {
    printf("\n--- TESTING DATA LOGGING FLASH (U7) ---\n");
    
    spi_init(SPI_PORT, 1000 * 1000);
    gpio_set_function(FLASH_RX_PIN, GPIO_FUNC_SPI);
    gpio_set_function(FLASH_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(FLASH_TX_PIN, GPIO_FUNC_SPI);
    
    gpio_init(FLASH_CS_PIN);
    gpio_set_dir(FLASH_CS_PIN, GPIO_OUT);
    gpio_put(FLASH_CS_PIN, 1); 

    sleep_ms(10);

    uint8_t tx_buf[] = {0x9F, 0x00, 0x00, 0x00};
    uint8_t rx_buf[4] = {0};

    gpio_put(FLASH_CS_PIN, 0); 
    spi_write_read_blocking(SPI_PORT, tx_buf, rx_buf, 4);
    gpio_put(FLASH_CS_PIN, 1); 

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
    printf("\n--- TESTING ESP12F WIFI MODULE (VIA PIO HACK) ---\n");
    
    PIO pio = pio0;
    uint sm_tx = pio_claim_unused_sm(pio, true);
    uint sm_rx = pio_claim_unused_sm(pio, true);
    uint offset_tx = pio_add_program(pio, &uart_tx_program);
    uint offset_rx = pio_add_program(pio, &uart_rx_program);

    pio_uart_tx_init(pio, sm_tx, offset_tx, WIFI_RX_PIN, WIFI_BAUD); 
    pio_uart_rx_init(pio, sm_rx, offset_rx, WIFI_TX_PIN, WIFI_BAUD);

    printf("Sending 'AT\\r\\n' command through crossed wires...\n");
    
    while (pio_uart_rx_readable(pio, sm_rx)) pio_uart_rx_getc(pio, sm_rx);

    pio_uart_tx_puts(pio, sm_tx, "AT\r\n");

    absolute_time_t timeout = make_timeout_time_ms(500);
    char response[128];
    int idx = 0;

    while (absolute_time_diff_us(get_absolute_time(), timeout) > 0 && idx < 127) {
        if (pio_uart_rx_readable(pio, sm_rx)) {
            response[idx++] = pio_uart_rx_getc(pio, sm_rx);
        }
    }
    response[idx] = '\0'; 

    if (idx > 0) {
        printf("[REPLY]:\n%s\n", response);
        if (strstr(response, "OK") != NULL) {
            printf("[SUCCESS] PIO Hack successful! ESP12F responded!\n");
        } else {
            printf("[WARNING] Received data, but no 'OK'.\n");
        }
    } else {
        printf("[ERROR] No response. The crossing didn't work or baud rate mismatch.\n");
    }

    pio_sm_set_enabled(pio, sm_tx, false);
    pio_sm_set_enabled(pio, sm_rx, false);
    pio_remove_program(pio, &uart_tx_program, offset_tx);
    pio_remove_program(pio, &uart_rx_program, offset_rx);
}

void gpio_bounce_test() {
    printf("\n--- GPIO BOUNCE TEST ---\n");
    printf("Toggling GPIOs 10, 11, 12, 13 High and Low every 2 seconds.\n");
    printf("Use your multimeter to probe these pins. Press any key to stop.\n");

    uint gpios_to_test[] = {10, 11, 12, 13}; 
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
    
    gpio_init(0);
    gpio_set_dir(0, GPIO_IN);

    printf("Listening to GPIO0...\n");
    printf("--> PRESS THE 'SW3' (ESP RESET) BUTTON ON YOUR BOARD NOW! <--\n\n");

    int transitions = 0;
    bool last_state = gpio_get(0);
    
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

void pio_uart_passthrough() {
    printf("\n--- PIO LIVE PASSTHROUGH TERMINAL ---\n");
    printf("SUCCESS: The PIO Crossover is working!\n");
    printf("Type 'AT' and hit Enter. To exit, restart the board.\n\n");

    PIO pio = pio0;
    uint sm_tx = pio_claim_unused_sm(pio, true);
    uint sm_rx = pio_claim_unused_sm(pio, true);
    uint offset_tx = pio_add_program(pio, &uart_tx_program);
    uint offset_rx = pio_add_program(pio, &uart_rx_program);

    // Apply the wire-swap fix
    pio_uart_tx_init(pio, sm_tx, offset_tx, WIFI_RX_PIN, WIFI_BAUD); 
    pio_uart_rx_init(pio, sm_rx, offset_rx, WIFI_TX_PIN, WIFI_BAUD);

    while (true) {
        // 1. Read incoming data from ESP12F (This handles the echo natively now)
        if (pio_uart_rx_readable(pio, sm_rx)) {
            putchar(pio_uart_rx_getc(pio, sm_rx));
        }

        // 2. Read keyboard and send to ESP12F silently
        int c = getchar_timeout_us(0);
        if (c != PICO_ERROR_TIMEOUT) {
            if (c == '\r') {
                // When you press Enter, send exactly one valid carriage return / line feed combo
                pio_uart_tx_putc(pio, sm_tx, '\r');
                pio_uart_tx_putc(pio, sm_tx, '\n');
            } else if (c != '\n') {
                // Send normal characters
                pio_uart_tx_putc(pio, sm_tx, (char)c);
            }
        }
    }
}