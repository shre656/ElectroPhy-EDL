#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/adc.h"
#include "hardware/pwm.h"
#include "hardware/pio.h"
#include "uart_tx.pio.h"
#include "uart_rx.pio.h"
#include "pico/pdm_microphone.h"
#include <stdlib.h> // For abs()

// ==========================================
// 🛑 CHANGE THESE TO YOUR 2.4GHz HOTSPOT DETAILS!
// ==========================================
#define WIFI_SSID "Karthik's Hotspot"
#define WIFI_PASS "mera hai"
#define MAC_IP    "172.27.34.212"
#define MAC_PORT  "8080"
// ==========================================

// --- I2C / Sensor Pins ---
#define I2C_PORT i2c1
#define SDA_PIN 2
#define SCL_PIN 3
#define MIC_CLK_PIN 22
#define MIC_DAT_PIN 23



// --- WiFi/ESP12F PIO Pins ---
#define WIFI_TX_PIN 0
#define WIFI_RX_PIN 1
#define WIFI_BAUD 115200

// --- Sensor Addresses ---
#define ADDR_MPU6050 0x68
#define ADDR_INA219  0x45

// --- MPU6050 Registers ---
#define MPU6050_PWR_MGMT_1   0x6B
#define MPU6050_ACCEL_XOUT_H 0x3B

#define ADDR_BNO055 0x28              // BNO055 default I2C address (can be 0x29 on some breakout boards)
#define BNO055_OPR_MODE_REG 0x3D      // Operating mode register
#define BNO055_MODE_NDOF 0x0C         // 9-DOF Sensor Fusion Mode
#define BNO055_EULER_H_LSB_REG 0x1A   // Start of Euler angles (Heading/Yaw)

// --- Global PIO Variables for Wi-Fi ---
PIO pio_inst = pio0;
uint sm_t, sm_r;
bool wifi_transparent_active = false;

// --- Function Prototypes ---
void wprintf(const char *format, ...);
int wgetchar(uint32_t timeout_us);
bool init_wifi_transparent();
bool send_at_cmd(const char* cmd, const char* expected, uint32_t timeout_ms);

void scan_i2c_bus();
void test_bno055();
void test_ina219();
void test_ldr();
void test_microphone();

// ==========================================
// CUSTOM WI-FI I/O FUNCTIONS
// ==========================================

void wprintf(const char *format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    printf("%s", buffer);
    if (wifi_transparent_active) {
        pio_uart_tx_puts(pio_inst, sm_t, buffer);
    }
}

int wgetchar(uint32_t timeout_us) {
    absolute_time_t t = make_timeout_time_us(timeout_us);
    do {
        if (wifi_transparent_active && pio_uart_rx_readable(pio_inst, sm_r)) {
            return pio_uart_rx_getc(pio_inst, sm_r);
        }
        int c = getchar_timeout_us(0);
        if (c != PICO_ERROR_TIMEOUT) {
            return c;
        }
        sleep_us(50);
    } while (timeout_us == 0xFFFFFFFF || absolute_time_diff_us(get_absolute_time(), t) > 0);
    return PICO_ERROR_TIMEOUT;
}

// ==========================================
// MAIN BOOT SEQUENCE
// ==========================================
int main() {
    stdio_init_all();
    sleep_ms(3000);

    // Setup I2C
    i2c_init(I2C_PORT, 100 * 1000);
    gpio_set_function(SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(SDA_PIN);
    gpio_pull_up(SCL_PIN);

    printf("\n========================================\n");
    printf("   ELECTROPHY WIRELESS DEBUGGER\n");
    printf("========================================\n");

    // Initialize PIO for Wi-Fi Crossover
    sm_t = pio_claim_unused_sm(pio_inst, true);
    sm_r = pio_claim_unused_sm(pio_inst, true);
    uint offset_tx = pio_add_program(pio_inst, &uart_tx_program);
    uint offset_rx = pio_add_program(pio_inst, &uart_rx_program);
    pio_uart_tx_init(pio_inst, sm_t, offset_tx, WIFI_RX_PIN, WIFI_BAUD);
    pio_uart_rx_init(pio_inst, sm_r, offset_rx, WIFI_TX_PIN, WIFI_BAUD);

    // Attempt to connect Wi-Fi
    if (init_wifi_transparent()) {
        wprintf("\n[NETWORK] ElectroPhy is now fully wireless!\n");
        wprintf("[NETWORK] You can unplug USB and power via battery now.\n");
    } else {
        printf("\n[FALLBACK] Wi-Fi failed. Running in USB-only mode.\n");
    }

    // Main Menu Loop
    while (true) {
        wprintf("\nSelect a remote test to run:\n");
        wprintf("1. Scan I2C Bus\n");
        wprintf("2. Stream MPU-6050 (Accel Data)\n");
        wprintf("3. Stream INA219 (Power Monitor)\n");
        wprintf("4. Stream LDR (Light Sensor)\n");
        wprintf("5. Stream SPH064 Microphone (PDM)\n");
        wprintf("> ");

        int choice = wgetchar(0xFFFFFFFF);

        switch(choice) {
            case '1': scan_i2c_bus();    break;
            case '2': test_bno055();    break;
            case '3': test_ina219();     break;
            case '4': test_ldr();        break;
            case '5': test_microphone(); break;
            case '\r':
            case '\n': break;
            default: wprintf("\nInvalid selection.\n");
        }
    }
    return 0;
}

// ==========================================
// WI-FI SETUP ROUTINES
// ==========================================

// FIX: Full AT echo logging so every ESP response is visible over USB
bool send_at_cmd(const char* cmd, const char* expected, uint32_t timeout_ms) {
    // Flush RX garbage before sending
    while (pio_uart_rx_readable(pio_inst, sm_r)) pio_uart_rx_getc(pio_inst, sm_r);

    printf("[AT >>>] %s", cmd);
    pio_uart_tx_puts(pio_inst, sm_t, cmd);

    absolute_time_t timeout = make_timeout_time_ms(timeout_ms);
    char buf[512]; int idx = 0;

    while (absolute_time_diff_us(get_absolute_time(), timeout) > 0) {
        if (pio_uart_rx_readable(pio_inst, sm_r)) {
            char c = pio_uart_rx_getc(pio_inst, sm_r);
            printf("%c", c); // Echo every ESP byte to USB for debugging
            buf[idx++] = c;
            if (idx >= 511) break;
            buf[idx] = '\0';
            if (strstr(buf, expected)               != NULL) return true;
            if (strstr(buf, "ERROR")                != NULL) return false;
            if (strstr(buf, "FAIL")                 != NULL) return false;
        }
    }
    printf("\n[AT TIMEOUT] Waited %lums for: '%s'\n", timeout_ms, expected);
    return false;
}

// FIX: Correct AT command sequence — CIPMUX=0 BEFORE CIPMODE=1, sleep after CONNECT
bool init_wifi_transparent() {
    printf("1/5 Waking ESP12F...\n");
    send_at_cmd("AT+RST\r\n", "ready", 5000);
    sleep_ms(1500); // Extra settling time after reset

    printf("2/5 Setting Station Mode...\n");
    send_at_cmd("AT+CWMODE=1\r\n", "OK", 2000);

    // FIX: Force single-connection mode FIRST — required before CIPMODE=1
    printf("2b/5 Forcing single-connection mode...\n");
    send_at_cmd("AT+CIPMUX=0\r\n", "OK", 2000);

    printf("3/5 Connecting to '%s' (Timeout: 20s)...\n", WIFI_SSID);
    char join_cmd[128];
    sprintf(join_cmd, "AT+CWJAP=\"%s\",\"%s\"\r\n", WIFI_SSID, WIFI_PASS);
    if (!send_at_cmd(join_cmd, "WIFI GOT IP", 20000)) {
        printf("[ERROR] WiFi join failed. Check SSID/password and 2.4GHz band.\n");
        return false;
    }
    sleep_ms(500); // Let IP assignment settle

    // FIX: CIPMODE=1 only after CIPMUX=0 is confirmed
    printf("4/5 Enabling transparent pipe mode...\n");
    if (!send_at_cmd("AT+CIPMODE=1\r\n", "OK", 2000)) {
        printf("[ERROR] CIPMODE=1 rejected — CIPMUX may not have applied.\n");
        return false;
    }

    // FIX: Close any stale TCP connection before opening a new one
    send_at_cmd("AT+CIPCLOSE\r\n", "OK", 2000); // Allowed to fail silently

    printf("4b/5 Opening TCP to Mac (%s:%s)...\n", MAC_IP, MAC_PORT);
    char tcp_cmd[128];
    sprintf(tcp_cmd, "AT+CIPSTART=\"TCP\",\"%s\",%s\r\n", MAC_IP, MAC_PORT);
    if (!send_at_cmd(tcp_cmd, "CONNECT", 5000)) {
        printf("[ERROR] TCP failed. On Mac, run: nc -l 0.0.0.0 8080\n");
        printf("[ERROR] Then verify: lsof -i :8080 shows *:8080, not 127.0.0.1\n");
        return false;
    }

    // FIX: Wait for TCP 3-way handshake to fully complete before CIPSEND
    sleep_ms(500);

    printf("5/5 Entering Transparent Pipe...\n");
    if (!send_at_cmd("AT+CIPSEND\r\n", ">", 3000)) {
        printf("[ERROR] CIPSEND failed — TCP may have closed immediately.\n");
        printf("[ERROR] Check Mac firewall: sudo pfctl -d\n");
        return false;
    }

    wifi_transparent_active = true;
    return true;
}

// ==========================================
// SENSOR TEST ROUTINES
// ==========================================

void scan_i2c_bus() {
    wprintf("\n--- AGGRESSIVE I2C BUS SCAN ---\n");
    
    // 1. Force the bus to a safe 100kHz speed (BNO055 hates 400kHz)
    uint baud = i2c_init(I2C_PORT, 100 * 1000);
    wprintf("[DEBUG] I2C Baudrate reset to: %d Hz\n", baud);
    
    // 2. Force internal pull-up resistors (Critical if you don't have physical ones)
    // NOTE: Replace 4 and 5 below with whatever your actual SDA and SCL pin numbers are!
    gpio_pull_up(4); // SDA Pin
    gpio_pull_up(5); // SCL Pin
    wprintf("[DEBUG] Internal Pull-ups enabled.\n");
    wprintf("[DEBUG] Searching all 127 addresses with 10ms timeouts...\n\n");

    int count = 0;
    for (int addr = 1; addr < 128; ++addr) {
        uint8_t rxdata;
        // Using timeout so a locked bus doesn't freeze the scan
        int ret = i2c_read_timeout_us(I2C_PORT, addr, &rxdata, 1, false, 10000);
        
        if (ret >= 0) {
            wprintf("[SUCCESS] Found device at 0x%02X!\n", addr);
            count++;
        } else if (ret == PICO_ERROR_GENERIC) {
            // This just means "nobody answered", perfectly normal for empty addresses
        } else if (ret == PICO_ERROR_TIMEOUT) {
            wprintf("[WARNING] Address 0x%02X stretched the clock and timed out!\n", addr);
        }
    }
    
    if (count == 0) {
        wprintf("\n[RESULT] 0 devices found. Check wiring and pin numbers!\n");
    } else {
        wprintf("\n[RESULT] Scan complete. Found %d devices.\n", count);
    }
}

void test_bno055() {
    wprintf("\n--- STREAMING BNO055 FUSION DATA ---\n");

    // 1. Put into NDOF Mode
    uint8_t mode_cmd[] = {BNO055_OPR_MODE_REG, BNO055_MODE_NDOF};
    int w_ret = i2c_write_timeout_us(I2C_PORT, ADDR_BNO055, mode_cmd, 2, false, 25000);
    
    if (w_ret < 0) {
        wprintf("[ERROR] BNO055 not found at 0x%02X! Is the address 0x29?\n", ADDR_BNO055);
        return;
    }

    // Give the BNO055 fusion engine 100ms to boot up
    sleep_ms(100); 

    // Flush leftover terminal keys
    while (wgetchar(0) != PICO_ERROR_TIMEOUT); 
    wprintf("Press ANY key in terminal to stop stream...\n\n");

    while (wgetchar(0) == PICO_ERROR_TIMEOUT) {
        uint8_t reg = BNO055_EULER_H_LSB_REG;
        
        // FIX 1: Force the array to be all zeros so we NEVER print RAM garbage!
        uint8_t buffer[6] = {0, 0, 0, 0, 0, 0}; 
        
        // Tell the BNO055 which register we want to read
        int write_success = i2c_write_timeout_us(I2C_PORT, ADDR_BNO055, &reg, 1, true, 25000);
        
        // FIX 2: The BNO055 needs a microsecond to breathe before coughing up the data
        sleep_us(50); 
        
        // Read the 6 bytes
        int read_success = i2c_read_timeout_us(I2C_PORT, ADDR_BNO055, buffer, 6, false, 25000);

        // FIX 3: If the sensor drops a packet, ignore it and try again instead of freezing!
        if (write_success < 0 || read_success < 0) {
            wprintf("\r[I2C WARNING] Packet dropped. Re-syncing...         ");
            sleep_ms(50);
            continue; 
        }

        // Calculate degrees
        float heading = (int16_t)((buffer[1] << 8) | buffer[0]) / 16.0f;
        float roll    = (int16_t)((buffer[3] << 8) | buffer[2]) / 16.0f;
        float pitch   = (int16_t)((buffer[5] << 8) | buffer[4]) / 16.0f;

        wprintf("\rHeading: %6.2f | Roll: %6.2f | Pitch: %6.2f       ", heading, roll, pitch); 
        sleep_ms(50); // 20Hz refresh
    }
    
    // Put sensor to sleep
    uint8_t sleep_cmd[] = {BNO055_OPR_MODE_REG, 0x00};
    i2c_write_timeout_us(I2C_PORT, ADDR_BNO055, sleep_cmd, 2, false, 25000);
    
    wprintf("\n\nBNO055 Stream Stopped.\n");
}

void test_ina219() {
    wprintf("\n--- STREAMING INA219 ---\n");
    
    // FIX 1: Throw away leftover keys
    while (wgetchar(0) != PICO_ERROR_TIMEOUT); 
    
    wprintf("Press ANY key in terminal to stop stream...\n\n");

    while (wgetchar(0) == PICO_ERROR_TIMEOUT) {
        uint8_t reg_bus = 0x02, reg_shunt = 0x01, buf[2];

        i2c_write_timeout_us(I2C_PORT, ADDR_INA219, &reg_bus, 1, true, 25000);
        i2c_read_timeout_us(I2C_PORT, ADDR_INA219, buf, 2, false, 25000);
        float bus_voltage = (((buf[0] << 8) | buf[1]) >> 3) * 0.004f; 

        i2c_write_timeout_us(I2C_PORT, ADDR_INA219, &reg_shunt, 1, true, 25000);
        i2c_read_timeout_us(I2C_PORT, ADDR_INA219, buf, 2, false, 25000);
        float shunt_voltage = (int16_t)((buf[0] << 8) | buf[1]) * 0.01f; 

        // FIX 2: Use \n instead of \r
        wprintf("Bus: %5.2f V | Shunt: %6.2f mV\n", bus_voltage, shunt_voltage);
        sleep_ms(200);
    }
    wprintf("\nINA219 Stream Stopped.\n");
}

void test_ldr() {
    wprintf("\n--- STREAMING LDR ---\n");
    adc_init();
    adc_gpio_init(26);
    adc_select_input(0);
    
    // FIX 1: Throw away leftover keys
    while (wgetchar(0) != PICO_ERROR_TIMEOUT); 
    
    wprintf("Press ANY key in terminal to stop stream...\n\n");

    while (wgetchar(0) == PICO_ERROR_TIMEOUT) {
        float voltage = adc_read() * 3.3f / 4096.0f;
        // FIX 2: Use \n instead of \r
        wprintf("LDR Voltage: %4.2f V\n", voltage);
        sleep_ms(200);
    }
    wprintf("\nLDR Stream Stopped.\n");
}

// We use a global buffer to catch the audio data in the background
int16_t sample_buffer[256];
volatile int samples_read = 0;
volatile bool new_data = false;

// This callback fires automatically in the background when the DMA buffer is full
void on_pdm_samples_ready() {
    samples_read = pdm_microphone_read(sample_buffer, 256);
    new_data = true;
}

void test_microphone() {
    wprintf("\n--- STREAMING SPH064 (VIA PIO & DMA) ---\n");

    // 1. Configure the Microphone Library
    struct pdm_microphone_config config = {
        .gpio_data = MIC_DAT_PIN,
        .gpio_clk = MIC_CLK_PIN,
        .pio = pio1, // Use pio1 so we don't conflict with your WiFi on pio0!
        .pio_sm = 0,
        .sample_rate = 16000,       // 16 kHz audio
        .sample_buffer_size = 256,
    };

    if (pdm_microphone_init(&config) < 0) {
        wprintf("[ERROR] Failed to initialize PDM microphone library.\n");
        return;
    }

    pdm_microphone_set_samples_ready_handler(on_pdm_samples_ready);
    pdm_microphone_start();

    // Flush leftover keys
    while (wgetchar(0) != PICO_ERROR_TIMEOUT); 
    wprintf("Mic is LIVE. Speak into it! Press ANY key to stop...\n\n");

    int absolute_max_peak = 0;

    while (wgetchar(0) == PICO_ERROR_TIMEOUT) {
        // Wait for the background hardware to hand us a new batch of audio
        if (new_data) {
            new_data = false;
            int local_peak = 0;

            // Find the loudest noise in this specific batch of audio (PCM data)
            for (int i = 0; i < samples_read; i++) {
                int amplitude = abs(sample_buffer[i]);
                if (amplitude > local_peak) {
                    local_peak = amplitude;
                }
            }

            // Scale the raw 16-bit PCM value down so it fits nicely on a terminal bar graph
            int scaled_peak = local_peak / 500; 
            if (scaled_peak > 25) scaled_peak = 25; // Cap it for the visualizer
            
            if (scaled_peak > absolute_max_peak) {
                absolute_max_peak = scaled_peak;
            }

            // Print the visualizer
            wprintf("Vol [");
            for (int v = 0; v < 25; v++) {
                if (v < scaled_peak) wprintf("#"); else wprintf(" ");
            }
            wprintf("] Peak: %5d\n", local_peak);
        }
        
        sleep_ms(10); // Tiny sleep, no CPU hogging!
    }

    // Cleanup
    pdm_microphone_stop();
    pdm_microphone_deinit();
    wprintf("\nMic Stream Stopped.\n");
}