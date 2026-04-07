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

// --- Global PIO Variables for Wi-Fi ---
PIO pio_inst = pio0;
uint sm_t, sm_r;
bool wifi_transparent_active = false;

// --- Function Prototypes ---
void wprintf(const char *format, ...);
int wgetchar(uint32_t timeout_us);
bool init_wifi_transparent();
bool send_at_cmd(const char* cmd, const char* expected, uint32_t timeout_ms);

void run_i2c_scan();
void test_mpu6050();
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
            case '1': run_i2c_scan();    break;
            case '2': test_mpu6050();    break;
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

void run_i2c_scan() {
    wprintf("\n--- SCANNING I2C BUS ---\n");
    int devices = 0;
    uint8_t rxdata;
    for (int addr = 0; addr < 128; ++addr) {
        if ((addr & 0x78) == 0 || (addr & 0x78) == 0x78) continue;
        if (i2c_read_timeout_us(I2C_PORT, addr, &rxdata, 1, false, 25000) >= 0) {
            devices++;
            wprintf("Found device at 0x%02X\n", addr);
        }
    }
    if (devices == 0) wprintf("No devices found.\n");
    else              wprintf("Scan complete. %d device(s) found.\n", devices);
}

void test_mpu6050() {
    wprintf("\n--- STREAMING MPU-6050 ---\n");

    // Wake the MPU-6050 from sleep
    uint8_t wake_cmd[] = {MPU6050_PWR_MGMT_1, 0x00};
    i2c_write_timeout_us(I2C_PORT, ADDR_MPU6050, wake_cmd, 2, false, 25000);
    sleep_ms(100);

    // FIX 1: Throw away the leftover "Enter" key from the menu selection
    while (wgetchar(0) != PICO_ERROR_TIMEOUT); 

    wprintf("Press ANY key in terminal to stop stream...\n\n");

    while (wgetchar(0) == PICO_ERROR_TIMEOUT) {
        uint8_t accel_reg = MPU6050_ACCEL_XOUT_H;
        uint8_t buffer[6];
        i2c_write_timeout_us(I2C_PORT, ADDR_MPU6050, &accel_reg, 1, true, 25000);
        i2c_read_timeout_us(I2C_PORT, ADDR_MPU6050, buffer, 6, false, 25000);

        float ax = (int16_t)((buffer[0] << 8) | buffer[1]) / 16384.0f;
        float ay = (int16_t)((buffer[2] << 8) | buffer[3]) / 16384.0f;
        float az = (int16_t)((buffer[4] << 8) | buffer[5]) / 16384.0f;

        // FIX 2: Added \n at the end to force the Mac Terminal to print it!
        wprintf("Ax: %6.2f g | Ay: %6.2f g | Az: %6.2f g\n", ax, ay, az); 
        sleep_ms(100);
    }
    wprintf("\nMPU Stream Stopped.\n");
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

void test_microphone() {
    wprintf("\n--- STREAMING SPH064 MICROPHONE ---\n");
    gpio_init(MIC_CLK_PIN); gpio_set_dir(MIC_CLK_PIN, GPIO_OUT);
    gpio_init(MIC_DAT_PIN); gpio_set_dir(MIC_DAT_PIN, GPIO_IN);

    // FIX 1: Throw away leftover keys
    while (wgetchar(0) != PICO_ERROR_TIMEOUT); 
    
    wprintf("Press ANY key in terminal to stop stream...\n\n");
    int absolute_max_peak = 0;

    while (wgetchar(0) == PICO_ERROR_TIMEOUT) {
        int local_max = 0;
        for (int snapshot = 0; snapshot < 20; snapshot++) {
            int high_count = 0;
            for (int j = 0; j < 1000; j++) {
                gpio_put(MIC_CLK_PIN, 0); __asm volatile ("nop\nnop\n");
                gpio_put(MIC_CLK_PIN, 1);
                if (gpio_get(MIC_DAT_PIN)) high_count++;
                __asm volatile ("nop\nnop\n");
            }
            int deviation = high_count - 500;
            if (deviation < 0) deviation = -deviation;
            int score = (deviation * 100) / 250;
            if (score > local_max) local_max = score;
        }
        if (local_max > absolute_max_peak) absolute_max_peak = local_max;

        wprintf("Vol [");
        for (int v = 0; v < 20; v++) {
            if (v < local_max) wprintf("#"); else wprintf(" ");
        }
        // FIX 2: Use \n instead of \r
        wprintf("] Current: %2d | PEAK: %2d\n", local_max, absolute_max_peak);
        sleep_ms(50);
    }
    wprintf("\nMic Stream Stopped.\n");
}