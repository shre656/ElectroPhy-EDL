#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "pico/error.h"
#include "hardware/i2c.h" 

#define LOOP_RATE_US 10000 
#define MAX_REGS 32
#define MAX_INSTRUCTIONS 64

#define I2C_PORT i2c0
#define REG_PWR_MGMT_1 0x6B
#define REG_ACCEL_XOUT_H 0x3B

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

// --- SMART SCAN VARIABLES ---
bool sensor_connected = false;
uint8_t active_address = 0x68; 

void setup_mpu6050() {
    printf("[DEBUG] Initializing I2C pins (4 and 5) at 400kHz...\n");
    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(4, GPIO_FUNC_I2C);
    gpio_set_function(5, GPIO_FUNC_I2C);
    gpio_pull_up(4);
    gpio_pull_up(5);

    uint8_t rxdata;
    printf("[DEBUG] Pinging MPU6050 at 0x68...\n");
    if (i2c_read_timeout_us(I2C_PORT, 0x68, &rxdata, 1, false, 50000) >= 0) {
        active_address = 0x68;
        sensor_connected = true;
        printf("[DEBUG] SUCCESS: MPU6050 found at 0x68!\n");
    } 
    else {
        printf("[DEBUG] Failed at 0x68. Pinging 0x69...\n");
        if (i2c_read_timeout_us(I2C_PORT, 0x69, &rxdata, 1, false, 50000) >= 0) {
            active_address = 0x69;
            sensor_connected = true;
            printf("[DEBUG] SUCCESS: MPU6050 found at 0x69!\n");
        } else {
            printf("[DEBUG] FATAL: No MPU6050 found. Check SDA/SCL wiring. Bypassing hardware.\n");
        }
    }

    if (sensor_connected) {
        printf("[DEBUG] Waking up MPU6050...\n");
        uint8_t wakeup[] = {REG_PWR_MGMT_1, 0x00};
        i2c_write_timeout_us(I2C_PORT, active_address, wakeup, 2, false, 50000);
        sleep_ms(100); 
        printf("[DEBUG] MPU6050 Ready!\n");
    }
}

int main() {
    stdio_init_all();
    
    for (int i = 5; i > 0; i--) {
        sleep_ms(1000);
    }

    printf("\n[DEBUG] === MPU6050 VM START ===\n");
    setup_mpu6050();

    uint32_t last_time = time_us_32();

    while (true) {
        uint32_t current_time = time_us_32();

        if (current_time - last_time >= LOOP_RATE_US) {
            last_time = current_time;

            for (int i = 0; i < program_length; i++) {
                Instruction_t *inst = &program[i];

                switch (inst->opcode) {
                    case 0x02: { // READ SENSOR DATA
                        if (sensor_connected) {
                            uint8_t reg = REG_ACCEL_XOUT_H;
                            uint8_t data[14]; 

                            int ret = i2c_write_timeout_us(I2C_PORT, active_address, &reg, 1, true, 5000);
                            if (ret > 0) {
                                i2c_read_timeout_us(I2C_PORT, active_address, data, 14, false, 5000);
                                
                                int16_t ax = (int16_t)((data[0] << 8) | data[1]);
                                int16_t ay = (int16_t)((data[2] << 8) | data[3]);
                                int16_t az = (int16_t)((data[4] << 8) | data[5]);
                                
                                int16_t gx = (int16_t)((data[8] << 8) | data[9]);
                                int16_t gy = (int16_t)((data[10] << 8) | data[11]);
                                int16_t gz = (int16_t)((data[12] << 8) | data[13]);

                                // Output to VM Registers (Converted from G to m/s^2)
                                if (inst->out_regs[0] != 255) registers[inst->out_regs[0]] = ((float)ax / 16384.0f) * 9.81f;
                                if (inst->out_regs[1] != 255) registers[inst->out_regs[1]] = ((float)ay / 16384.0f) * 9.81f;
                                if (inst->out_regs[2] != 255) registers[inst->out_regs[2]] = ((float)az / 16384.0f) * 9.81f;
                                
                                // Gyroscope remains in degrees/sec
                                if (inst->out_regs[3] != 255) registers[inst->out_regs[3]] = (float)gx / 131.0f;
                                if (inst->out_regs[4] != 255) registers[inst->out_regs[4]] = (float)gy / 131.0f;
                                if (inst->out_regs[5] != 255) registers[inst->out_regs[5]] = (float)gz / 131.0f;
                            }
                        }
                        break;
                    }

                    case 0x10: { // CLAMP/LIMIT
                        if (inst->in_regs[0] != 255 && inst->out_regs[0] != 255) {
                            float val = registers[inst->in_regs[0]];
                            registers[inst->out_regs[0]] = (val > inst->param) ? inst->param : val;
                        }
                        break;
                    }

                    case 0x30: { // LOG TO SERIAL
                        float v0 = (inst->in_regs[0] != 255) ? registers[inst->in_regs[0]] : 0.0f;
                        float v1 = (inst->in_regs[1] != 255) ? registers[inst->in_regs[1]] : 0.0f;
                        float v2 = (inst->in_regs[2] != 255) ? registers[inst->in_regs[2]] : 0.0f;
                        float v3 = (inst->in_regs[3] != 255) ? registers[inst->in_regs[3]] : 0.0f;
                        float v4 = (inst->in_regs[4] != 255) ? registers[inst->in_regs[4]] : 0.0f;
                        float v5 = (inst->in_regs[5] != 255) ? registers[inst->in_regs[5]] : 0.0f;
                        
                        printf("%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n", v0, v1, v2, v3, v4, v5);
                        break;
                    }
                }
            }
        }

        int c = getchar_timeout_us(0);
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
                        printf("[DEBUG] VM_READY: Bytecode Loaded.\n");
                        rx_state = WAIT_SYNC; 
                    }
                    break;
            }
        }
    }
}