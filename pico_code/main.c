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

// #include "vl53l0x.h"

// Declare your device struct globally so the whole VM can see it
// vl53l0x_t my_tof; 
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
#define LOOP_RATE_US 40000     // 50Hz Execution Loop
#define MAX_REGS 64            
#define MAX_INSTRUCTIONS 64

#define I2C_PORT i2c1
#define SDA_PIN 2
#define SCL_PIN 3
#define LDR_PIN 26
#define MIC_CLK_PIN 22
#define MIC_DAT_PIN 23



// --- BNO055 REGISTERS ---
#define ADDR_BNO055 0x28              // Change to 0x29 if using an alternate breakout
#define BNO055_OPR_MODE_REG 0x3D      
#define BNO055_MODE_NDOF 0x0C         
#define BNO055_EULER_H_LSB_REG 0x1A

// --- INA219 REGISTERS ---
#define ADDR_INA219  0x45

// ---  DSP CONSTANTS ---
#define FFT_SIZE 256
#define PI 3.14159265358979323846

typedef struct __attribute__((packed)) {
    uint8_t opcode;
    uint8_t in_regs[6];
    uint8_t out_regs[6];
    float param;
} Instruction_t;

float registers[MAX_REGS];                 
Instruction_t program[MAX_INSTRUCTIONS];   
uint8_t program_length = 0;

int16_t sample_buffer[FFT_SIZE]; 
volatile int current_mic_peak = 0;
volatile float current_dominant_freq = 0.0f;             
float filter_state = 0.0f;

typedef enum { WAIT_SYNC, WAIT_COUNT, READ_PAYLOAD } SerialState;
SerialState rx_state = WAIT_SYNC;
uint8_t expected_instructions = 0;
uint8_t rx_buffer[MAX_INSTRUCTIONS * sizeof(Instruction_t)];
uint16_t rx_index = 0;




// ==========================================
// DSP ALGORITHMS (DC OFFSET & FFT)
// ==========================================
void remove_dc_offset(int16_t *buffer, int num_samples) {
    long sum = 0;
    for (int i = 0; i < num_samples; i++) sum += buffer[i];
    int16_t mean = (int16_t)(sum / num_samples);
    for (int i = 0; i < num_samples; i++) buffer[i] -= mean;
}

void compute_fft_magnitude(int16_t* pcm_data, float* magnitudes, int n) {
    float data_re[FFT_SIZE], data_im[FFT_SIZE];

    for (int i = 0; i < n; i++) {
        float multiplier = 0.5f * (1.0f - cos(2.0f * PI * i / (n - 1)));
        data_re[i] = pcm_data[i] * multiplier;
        data_im[i] = 0.0f;
    }

    int j = 0;
    for (int i = 0; i < n - 1; i++) {
        if (i < j) {
            float temp_re = data_re[i], temp_im = data_im[i];
            data_re[i] = data_re[j]; data_im[i] = data_im[j];
            data_re[j] = temp_re; data_im[j] = temp_im;
        }
        int m = n / 2;
        while (m >= 1 && j >= m) { j -= m; m /= 2; }
        j += m;
    }

    for (int k = 1; k < n; k *= 2) {
        float step_re = cos(-PI / k), step_im = sin(-PI / k);
        for (int i = 0; i < n; i += 2 * k) {
            float w_re = 1.0f, w_im = 0.0f;
            for (int current_j = 0; current_j < k; current_j++) {
                float u_re = data_re[i + current_j], u_im = data_im[i + current_j];
                float v_re = data_re[i + current_j + k] * w_re - data_im[i + current_j + k] * w_im;
                float v_im = data_re[i + current_j + k] * w_im + data_im[i + current_j + k] * w_re;
                
                data_re[i + current_j] = u_re + v_re;
                data_im[i + current_j] = u_im + v_im;
                data_re[i + current_j + k] = u_re - v_re;
                data_im[i + current_j + k] = u_im - v_im;

                float next_w_re = w_re * step_re - w_im * step_im;
                w_im = w_re * step_im + w_im * step_re;
                w_re = next_w_re;
            }
        }
    }

    for (int i = 0; i < n / 2; i++) {
        magnitudes[i] = sqrt(data_re[i] * data_re[i] + data_im[i] * data_im[i]);
    }
}

void on_pdm_samples_ready() {
    int samples = pdm_microphone_read(sample_buffer, FFT_SIZE);
    
    // 1. Clean the audio to center the wave on 0
    remove_dc_offset(sample_buffer, samples);

    float alpha = 0.25f; 
    
    for (int i = 0; i < samples; i++) {
        // Smooth the current sample against the previous ones
        filter_state = (alpha * sample_buffer[i]) + ((1.0f - alpha) * filter_state);
        sample_buffer[i] = (int16_t)filter_state;
    }

    // ==========================================
    // NEW: DIGITAL GAIN (SOFTWARE PRE-AMP)
    // ==========================================
    // Tweak this multiplier! 
    // Start at 15. If it's still too quiet, try 30 or 50. 
    // If it's too staticky/distorted, drop it to 5 or 10.
    int gain_multiplier = 5; 
    
    for (int i = 0; i < samples; i++) {
        int32_t boosted = (int32_t)sample_buffer[i] * gain_multiplier;
        
        // Hard-clipping to prevent 16-bit integer overflow
        if (boosted > 32767) boosted = 32767;
        else if (boosted < -32768) boosted = -32768;
        
        sample_buffer[i] = (int16_t)boosted;
    }
    // ==========================================

    // 2. Calculate Peak Volume (Now using the boosted audio)
    int local_peak = 0;
    for (int i = 0; i < samples; i++) {
        int amplitude = abs(sample_buffer[i]);
        if (amplitude > local_peak) local_peak = amplitude;
    }
    current_mic_peak = local_peak;

    // 3. Run FFT and find the Dominant Frequency (Now using the boosted audio)
    float magnitudes[FFT_SIZE / 2];
    compute_fft_magnitude(sample_buffer, magnitudes, FFT_SIZE);

    float max_mag = 0;
    int dominant_bin = 0;
    
    // Start at bin 1 to ignore DC offset
    for (int i = 1; i < FFT_SIZE / 2; i++) { 
        if (magnitudes[i] > max_mag) {
            max_mag = magnitudes[i];
            dominant_bin = i;
        }
    }
    
    current_dominant_freq = (float)dominant_bin * (16000.0f / FFT_SIZE);
}

// ==========================================
// CUSTOM DUAL-ROUTING I/O FUNCTIONS
// ==========================================

// Prints to BOTH USB and Wi-Fi simultaneously
// ==========================================
// CUSTOM DUAL-ROUTING I/O FUNCTIONS
// ==========================================
void vm_printf(const char *format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (wifi_transparent_active) {
        // WE ARE WIRELESS: Route exclusively to the ESP-12F antenna.
        pio_uart_tx_puts(pio_inst, sm_t, buffer); 
    } else {
        // WE ARE WIRED: Route to the Mac's USB Serial Monitor.
        printf("%s", buffer); 
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
    sleep_ms(2000);

    printf("2/5 Setting Station Mode...\n");
    send_at_cmd("AT+CWMODE=1\r\n", "OK", 2000);

    printf("2b/5 Forcing single-connection mode...\n");
    send_at_cmd("AT+CIPMUX=0\r\n", "OK", 2000);

    printf("3/5 Connecting to '%s'...\n", WIFI_SSID);
    char join_cmd[128];
    sprintf(join_cmd, "AT+CWJAP=\"%s\",\"%s\"\r\n", WIFI_SSID, WIFI_PASS);
    if (!send_at_cmd(join_cmd, "WIFI GOT IP", 20000)) return false;
    sleep_ms(2000); 

    printf("4/5 Enabling transparent pipe mode...\n");
    if (!send_at_cmd("AT+CIPMODE=1\r\n", "OK", 2000)) return false;

    send_at_cmd("AT+CIPCLOSE\r\n", "OK", 2000); 

    printf("4b/5 Opening TCP to Mac (%s:%s)...\n", MAC_IP, MAC_PORT);
    char tcp_cmd[128];
    sprintf(tcp_cmd, "AT+CIPSTART=\"TCP\",\"%s\",%s\r\n", MAC_IP, MAC_PORT);
    if (!send_at_cmd(tcp_cmd, "CONNECT", 5000)) return false;
    sleep_ms(2000);

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

    /// 2. I2C Sensors (BNO055 & INA219)
    i2c_init(I2C_PORT, 100 * 1000); // FIXED: 100kHz for BNO055 stability
    gpio_set_function(SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(SDA_PIN);
    gpio_pull_up(SCL_PIN);

    // // 2b. Initialize ToF Sensor (Requires I2C, so after the above init)
    // // Initialize the ToF struct and bind it to i2c1
    // vl53l0x_init_struct(&my_tof, i2c1);
    
    // // Boot it up with a safe 40ms timeout to prevent Wi-Fi crashes
    // if (vl53l0x_init(&my_tof, true)) {
    //     printf("[HARDWARE] ToF Sensor Found!\n");
    //     vl53l0x_set_timeout(&my_tof, 40); 
    //     vl53l0x_start_continuous(&my_tof, 0);
    // } else {
    //     printf("[ERROR] ToF Sensor NOT FOUND on I2C bus!\n");
    // }

    // Boot BNO055 into NDOF Fusion Mode
    uint8_t mode_cmd[] = {BNO055_OPR_MODE_REG, BNO055_MODE_NDOF};
    i2c_write_timeout_us(I2C_PORT, ADDR_BNO055, mode_cmd, 2, false, 25000);
    sleep_ms(50); // Give the ARM Cortex-M0 time to start

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
                        // --- UPDATED FOR 9-AXIS BNO055 FUSION ---
                        uint8_t euler_reg = BNO055_EULER_H_LSB_REG; // 0x1A
                        uint8_t lacc_reg  = 0x28;                   // Linear Accel Start Register
                        
                        uint8_t euler_data[6] = {0, 0, 0, 0, 0, 0}; 
                        uint8_t lacc_data[6]  = {0, 0, 0, 0, 0, 0}; 
                        
                        // 1. Read Euler Angles (Heading, Roll, Pitch)
                        if (i2c_write_timeout_us(I2C_PORT, ADDR_BNO055, &euler_reg, 1, true, 5000) > 0) {
                            sleep_us(50); // Clock stretch buffer
                            if (i2c_read_timeout_us(I2C_PORT, ADDR_BNO055, euler_data, 6, false, 5000) > 0) {
                                // Scale: 1 degree = 16 LSB
                                if (inst->out_regs[0] != 255) registers[inst->out_regs[0]] = (int16_t)((euler_data[1] << 8) | euler_data[0]) / 16.0f; 
                                if (inst->out_regs[1] != 255) registers[inst->out_regs[1]] = (int16_t)((euler_data[3] << 8) | euler_data[2]) / 16.0f; 
                                if (inst->out_regs[2] != 255) registers[inst->out_regs[2]] = (int16_t)((euler_data[5] << 8) | euler_data[4]) / 16.0f; 
                            }
                        }

                        // 2. Read Linear Acceleration (Gravity removed by fusion algorithm)
                        if (i2c_write_timeout_us(I2C_PORT, ADDR_BNO055, &lacc_reg, 1, true, 5000) > 0) {
                            sleep_us(50);
                            if (i2c_read_timeout_us(I2C_PORT, ADDR_BNO055, lacc_data, 6, false, 5000) > 0) {
                                // Scale: 1 m/s^2 = 100 LSB
                                if (inst->out_regs[3] != 255) registers[inst->out_regs[3]] = (int16_t)((lacc_data[1] << 8) | lacc_data[0]) / 100.0f; 
                                if (inst->out_regs[4] != 255) registers[inst->out_regs[4]] = (int16_t)((lacc_data[3] << 8) | lacc_data[2]) / 100.0f; 
                                if (inst->out_regs[5] != 255) registers[inst->out_regs[5]] = (int16_t)((lacc_data[5] << 8) | lacc_data[4]) / 100.0f; 
                            }
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
                        // Register 0: The Volume Peak
                        if (inst->out_regs[0] != 255) {
                            registers[inst->out_regs[0]] = (float)current_mic_peak;
                        }
                        // Register 1: The Dominant Frequency (Hz)
                        if (inst->out_regs[1] != 255) {
                            registers[inst->out_regs[1]] = current_dominant_freq;
                        }
                        break;
                    }

                    // case 0x05: { 
                    //     // Opcode 0x05: Time of Flight Sensor
                    //     if (inst->out_regs[0] != 255) {
                    //         // Read the distance using the new pure C driver
                    //         uint16_t dist = vl53l0x_read_range_continuous_millimeters(&my_tof);
                            
                    //         // If it times out or points at open sky, clamp to 2000mm
                    //         if (vl53l0x_timeout_occurred(&my_tof) || dist > 8000) {
                    //             dist = 2000; 
                    //         }
                            
                    //         registers[inst->out_regs[0]] = (float)dist;
                    //     }
                    //     break;
                    // }
                    

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