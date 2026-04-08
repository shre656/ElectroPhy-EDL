#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>
#include <stdarg.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/error.h"
#include "hardware/i2c.h"
#include "hardware/adc.h"
#include "hardware/pio.h"
#include "pico/pdm_microphone.h"

// Your PIO UART headers
#include "uart_tx.pio.h"
#include "uart_rx.pio.h"

// ==========================================
//  WI-FI CONFIGURATION
// ==========================================
#define WIFI_SSID "Karthik's Hotspot"
#define WIFI_PASS "mera hai"
#define MAC_IP    "172.27.34.212"
#define MAC_PORT  "8080"

#define WIFI_TX_PIN 0
#define WIFI_RX_PIN 1
#define WIFI_BAUD 115200

PIO pio_inst = pio0;
uint sm_t, sm_r;
bool wifi_transparent_active = false;

// ==========================================
// VM & HARDWARE CONFIGURATION
// ==========================================
#define LOOP_RATE_US 20000     // 50Hz Execution Loop
#define MAX_REGS 64            
#define MAX_INSTRUCTIONS 64

#define I2C_PORT i2c1
#define SDA_PIN 2
#define SCL_PIN 3
#define LDR_PIN 26
#define MIC_CLK_PIN 22
#define MIC_DAT_PIN 23

#define ADDR_MPU6050 0x68
#define ADDR_INA219  0x45

typedef struct __attribute__((packed)) {
    uint8_t opcode;
    uint8_t in_regs[6];
    uint8_t out_regs[6];
    float param;
} Instruction_t;

float registers[MAX_REGS];                 
Instruction_t program[MAX_INSTRUCTIONS];   
uint8_t program_length = 0;                

typedef enum { WAIT_SYNC, WAIT_COUNT, READ_PAYLOAD } SerialState;
SerialState rx_state = WAIT_SYNC;
uint8_t expected_instructions = 0;
uint8_t rx_buffer[MAX_INSTRUCTIONS * sizeof(Instruction_t)];
uint16_t rx_index = 0;

int16_t sample_buffer[256];
volatile int current_mic_peak = 0;

void on_pdm_samples_ready() {
    int samples = pdm_microphone_read(sample_buffer, 256);
    int local_peak = 0;
    for (int i = 0; i < samples; i++) {
        int amplitude = abs(sample_buffer[i]);
        if (amplitude > local_peak) local_peak = amplitude;
    }
    current_mic_peak = local_peak;
}

// ==========================================
// CUSTOM DUAL-ROUTING I/O FUNCTIONS
// ==========================================

// Prints to BOTH USB and Wi-Fi simultaneously
void vm_printf(const char *format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    printf("%s", buffer); // Send to USB Serial Monitor
    if (wifi_transparent_active) {
        pio_uart_tx_puts(pio_inst, sm_t, buffer); // Send over ESP-12F
    }
}

// Reads from BOTH USB and Wi-Fi non-blockingly
int vm_getchar() {
    if (wifi_transparent_active && pio_uart_rx_readable(pio_inst, sm_r)) {
        return pio_uart_rx_getc(pio_inst, sm_r);
    }
    return getchar_timeout_us(0);
}

// ==========================================
// WI-FI INITIALIZATION
// ==========================================
bool send_at_cmd(const char* cmd, const char* expected, uint32_t timeout_ms) {
    while (pio_uart_rx_readable(pio_inst, sm_r)) pio_uart_rx_getc(pio_inst, sm_r);

    printf("[AT >>>] %s", cmd);
    pio_uart_tx_puts(pio_inst, sm_t, cmd);

    absolute_time_t timeout = make_timeout_time_ms(timeout_ms);
    char buf[512]; int idx = 0;

    while (absolute_time_diff_us(get_absolute_time(), timeout) > 0) {
        if (pio_uart_rx_readable(pio_inst, sm_r)) {
            char c = pio_uart_rx_getc(pio_inst, sm_r);
            printf("%c", c);
            buf[idx++] = c;
            if (idx >= 511) break;
            buf[idx] = '\0';
            if (strstr(buf, expected) != NULL) return true;
            if (strstr(buf, "ERROR")  != NULL) return false;
            if (strstr(buf, "FAIL")   != NULL) return false;
        }
    }
    printf("\n[AT TIMEOUT] Waited %lums for: '%s'\n", timeout_ms, expected);
    return false;
}

bool init_wifi_transparent() {
    printf("1/5 Waking ESP12F...\n");
    send_at_cmd("AT+RST\r\n", "ready", 5000);
    sleep_ms(1500);

    printf("2/5 Setting Station Mode...\n");
    send_at_cmd("AT+CWMODE=1\r\n", "OK", 2000);

    printf("2b/5 Forcing single-connection mode...\n");
    send_at_cmd("AT+CIPMUX=0\r\n", "OK", 2000);

    printf("3/5 Connecting to '%s'...\n", WIFI_SSID);
    char join_cmd[128];
    sprintf(join_cmd, "AT+CWJAP=\"%s\",\"%s\"\r\n", WIFI_SSID, WIFI_PASS);
    if (!send_at_cmd(join_cmd, "WIFI GOT IP", 20000)) return false;
    sleep_ms(500); 

    printf("4/5 Enabling transparent pipe mode...\n");
    if (!send_at_cmd("AT+CIPMODE=1\r\n", "OK", 2000)) return false;

    send_at_cmd("AT+CIPCLOSE\r\n", "OK", 2000); 

    printf("4b/5 Opening TCP to Mac (%s:%s)...\n", MAC_IP, MAC_PORT);
    char tcp_cmd[128];
    sprintf(tcp_cmd, "AT+CIPSTART=\"TCP\",\"%s\",%s\r\n", MAC_IP, MAC_PORT);
    if (!send_at_cmd(tcp_cmd, "CONNECT", 5000)) return false;
    sleep_ms(500);

    printf("5/5 Entering Transparent Pipe...\n");
    if (!send_at_cmd("AT+CIPSEND\r\n", ">", 3000)) return false;

    wifi_transparent_active = true;
    return true;
}

// ==========================================
// HARDWARE INITIALIZATION
// ==========================================
void setup_hardware() {
    printf("[DEBUG] VM Booting. Initializing Hardware...\n");

    // 1. Init PIO UART for Wi-Fi first
    sm_t = pio_claim_unused_sm(pio_inst, true);
    sm_r = pio_claim_unused_sm(pio_inst, true);
    uint offset_tx = pio_add_program(pio_inst, &uart_tx_program);
    uint offset_rx = pio_add_program(pio_inst, &uart_rx_program);
    pio_uart_tx_init(pio_inst, sm_t, offset_tx, WIFI_RX_PIN, WIFI_BAUD);
    pio_uart_rx_init(pio_inst, sm_r, offset_rx, WIFI_TX_PIN, WIFI_BAUD);

    // Attempt Wi-Fi Connection
    if (init_wifi_transparent()) {
        vm_printf("\n[NETWORK] ElectroPhy is now fully wireless!\n");
    } else {
        printf("\n[FALLBACK] Wi-Fi failed. Running in USB-only mode.\n");
    }

    // 2. I2C Sensors
    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(SDA_PIN);
    gpio_pull_up(SCL_PIN);

    uint8_t wake_cmd[] = {0x6B, 0x00};
    i2c_write_blocking(I2C_PORT, ADDR_MPU6050, wake_cmd, 2, false);

    // 3. LDR ADC
    adc_init();
    adc_gpio_init(LDR_PIN);
    adc_select_input(0);

    // 4. Microphone (Uses PIO1 to avoid conflicting with Wi-Fi on PIO0)
    struct pdm_microphone_config config = {
        .gpio_data = MIC_DAT_PIN, .gpio_clk = MIC_CLK_PIN,
        .pio = pio1, .pio_sm = 0,
        .sample_rate = 16000, .sample_buffer_size = 256,
    };
    pdm_microphone_init(&config);
    pdm_microphone_set_samples_ready_handler(on_pdm_samples_ready);
    pdm_microphone_start();

    vm_printf("[DEBUG] Hardware Ready. Waiting for React Flow Bytecode...\n");
}

// ==========================================
// MAIN LOOP
// ==========================================
int main() {
    stdio_init_all();
    sleep_ms(2000); 

    setup_hardware();
    uint32_t last_time = time_us_32();

    while (true) {
        uint32_t current_time = time_us_32();

        // 1. EXECUTE LOADED PROGRAM AT 50Hz
        if (current_time - last_time >= LOOP_RATE_US && program_length > 0) {
            last_time = current_time;

            for (int i = 0; i < program_length; i++) {
                Instruction_t *inst = &program[i];

                switch (inst->opcode) {
                    
                    case 0x01: { 
                        uint8_t reg = 0x3B; 
                        uint8_t data[14]; 
                        if (i2c_write_timeout_us(I2C_PORT, ADDR_MPU6050, &reg, 1, true, 5000) > 0) {
                            i2c_read_timeout_us(I2C_PORT, ADDR_MPU6050, data, 14, false, 5000);
                            if (inst->out_regs[0] != 255) registers[inst->out_regs[0]] = (int16_t)((data[0] << 8) | data[1]) / 16384.0f;
                            if (inst->out_regs[1] != 255) registers[inst->out_regs[1]] = (int16_t)((data[2] << 8) | data[3]) / 16384.0f;
                            if (inst->out_regs[2] != 255) registers[inst->out_regs[2]] = (int16_t)((data[4] << 8) | data[5]) / 16384.0f;
                            if (inst->out_regs[3] != 255) registers[inst->out_regs[3]] = (int16_t)((data[8] << 8) | data[9]) / 131.0f;
                            if (inst->out_regs[4] != 255) registers[inst->out_regs[4]] = (int16_t)((data[10] << 8) | data[11]) / 131.0f;
                            if (inst->out_regs[5] != 255) registers[inst->out_regs[5]] = (int16_t)((data[12] << 8) | data[13]) / 131.0f;
                        }
                        break;
                    }

                    case 0x02: {
                        uint8_t reg_bus = 0x02, reg_shunt = 0x01, buf[2];
                        if (i2c_write_timeout_us(I2C_PORT, ADDR_INA219, &reg_bus, 1, true, 5000) > 0) {
                            i2c_read_timeout_us(I2C_PORT, ADDR_INA219, buf, 2, false, 5000);
                            if (inst->out_regs[0] != 255) registers[inst->out_regs[0]] = (((buf[0] << 8) | buf[1]) >> 3) * 0.004f; 
                            i2c_write_timeout_us(I2C_PORT, ADDR_INA219, &reg_shunt, 1, true, 5000);
                            i2c_read_timeout_us(I2C_PORT, ADDR_INA219, buf, 2, false, 5000);
                            if (inst->out_regs[1] != 255) registers[inst->out_regs[1]] = (int16_t)((buf[0] << 8) | buf[1]) * 0.01f; 
                        }
                        break;
                    }

                    case 0x03: {
                        if (inst->out_regs[0] != 255) {
                            registers[inst->out_regs[0]] = adc_read() * 3.3f / 4096.0f;
                        }
                        break;
                    }

                    case 0x04: {
                        if (inst->out_regs[0] != 255) {
                            registers[inst->out_regs[0]] = (float)current_mic_peak;
                        }
                        break;
                    }

                    case 0x10: { 
                        if (inst->in_regs[0] != 255 && inst->out_regs[0] != 255) {
                            float val = registers[inst->in_regs[0]];
                            registers[inst->out_regs[0]] = (val > inst->param) ? inst->param : val;
                        }
                        break;
                    }

                    case 0x30: { 
                        for(int j=0; j<6; j++) {
                            if (inst->in_regs[j] != 255) {
                                // FIXED: Use vm_printf to push to Wi-Fi
                                vm_printf("%.3f%s", registers[inst->in_regs[j]], (j==5 || inst->in_regs[j+1] == 255) ? "" : ",");
                            } else {
                                break; 
                            }
                        }
                        vm_printf("\n");
                        break;
                    }
                }
            }
        }

        // 2. LISTEN FOR NEW BYTECODE (VIA USB AND WI-FI)
        int c = vm_getchar(); // FIXED: Listens to both streams non-blockingly
        if (c != PICO_ERROR_TIMEOUT) {
            uint8_t byte = (uint8_t)c;
            switch (rx_state) {
                case WAIT_SYNC:
                    if (byte == 0xAA) rx_state = WAIT_COUNT;
                    break;
                case WAIT_COUNT:
                    expected_instructions = byte;
                    rx_index = 0;
                    rx_state = (expected_instructions > 0 && expected_instructions <= MAX_INSTRUCTIONS) ? READ_PAYLOAD : WAIT_SYNC;
                    break;
                case READ_PAYLOAD:
                    rx_buffer[rx_index++] = byte;
                    if (rx_index >= (expected_instructions * sizeof(Instruction_t))) {
                        for (int i = 0; i < expected_instructions; i++) {
                            program[i] = ((Instruction_t *)rx_buffer)[i];
                        }
                        program_length = expected_instructions;
                        vm_printf("[DEBUG] VM FLASHED! Running %d blocks.\n", program_length);
                        rx_state = WAIT_SYNC; 
                    }
                    break;
            }
        }
    }
}