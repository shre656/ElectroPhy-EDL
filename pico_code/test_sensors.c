#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/adc.h"
#include "hardware/pwm.h" // NEW: Added for Microphone Clock

// --- I2C Pins for Native ElectroPhy PCB ---
#define I2C_PORT i2c1
#define SDA_PIN 2
#define SCL_PIN 3

// --- Microphone Pins ---
#define MIC_CLK_PIN 22
#define MIC_DAT_PIN 23

// --- Sensor I2C Addresses ---
#define ADDR_MPU6050_1 0x68
#define ADDR_MPU6050_2 0x69
#define ADDR_INA219    0x45

// --- MPU6050 Registers ---
#define MPU6050_WHO_AM_I     0x75
#define MPU6050_PWR_MGMT_1   0x6B
#define MPU6050_ACCEL_XOUT_H 0x3B
#define MPU6050_EXPECTED_ID  0x68

// --- INA219 Registers ---
#define INA219_REG_CONFIG    0x00

void run_i2c_scan();
void test_mpu6050();
void test_ina219();
void test_ldr();
void test_gpio_bounce();
void test_microphone();

int main() {
    stdio_init_all();
    sleep_ms(3000); 

    printf("\n========================================\n");
    printf("   ELECTROPHY NATIVE SENSOR TESTER\n");
    printf("========================================\n");

    i2c_init(I2C_PORT, 100 * 1000);
    gpio_set_function(SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(SDA_PIN);
    gpio_pull_up(SCL_PIN);

    while (true) {
        printf("\nSelect a test:\n");
        printf("1. Scan I2C Bus\n");
        printf("2. Test MPU-6050 (Read Accel Data)\n");
        printf("3. Test INA219 (Power Monitor)\n");
        printf("4. Test LDR (Light Sensor)\n");
        printf("5. GPIO Bounce Test (Cold Solder Check)\n");
        printf("6. Test SPH064 Microphone (PDM Stream)\n");
        printf("> ");

        int choice = getchar_timeout_us(0xFFFFFFFF);
        
        switch(choice) {
            case '1': run_i2c_scan(); break;
            case '2': test_mpu6050(); break;
            case '3': test_ina219(); break;
            case '4': test_ldr(); break;
            case '5': test_gpio_bounce(); break;
            case '6': test_microphone(); break;
            case '\r':
            case '\n': break;
            default: printf("\nInvalid selection.\n");
        }
    }
    return 0;
}

// ... [Keep run_i2c_scan(), test_mpu6050(), test_ina219(), test_ldr(), test_gpio_bounce() exactly as they were] ...
void run_i2c_scan() {
    printf("\n--- SCANNING I2C BUS ---\n");
    int devices_found = 0;
    uint8_t rxdata;

    for (int addr = 0; addr < (1 << 7); ++addr) {
        if ((addr & 0x78) == 0 || (addr & 0x78) == 0x78) continue;

        int ret = i2c_read_timeout_us(I2C_PORT, addr, &rxdata, 1, false, 25000);
        if (ret >= 0) {
            devices_found++;
            printf("Found device at 0x%02X ", addr);
            if (addr == ADDR_INA219) printf("(Likely INA219)\n");
            else if (addr == ADDR_MPU6050_1 || addr == ADDR_MPU6050_2) printf("(Likely MPU-6050)\n");
            else printf("(Other/Unknown)\n");
        }
    }
    if (devices_found == 0) printf("No devices found. Check 3.3V power and traces.\n");
    else printf("Scan complete.\n");
}

void test_mpu6050() {
    printf("\n--- TESTING MPU-6050 ---\n");
    uint8_t chip_id;
    uint8_t reg = MPU6050_WHO_AM_I;
    uint8_t active_addr = 0;

    if (i2c_write_timeout_us(I2C_PORT, ADDR_MPU6050_1, &reg, 1, true, 25000) >= 0) {
        active_addr = ADDR_MPU6050_1;
    } else if (i2c_write_timeout_us(I2C_PORT, ADDR_MPU6050_2, &reg, 1, true, 25000) >= 0) {
        active_addr = ADDR_MPU6050_2;
    }

    if (active_addr != 0) {
        i2c_read_timeout_us(I2C_PORT, active_addr, &chip_id, 1, false, 25000);
        printf("MPU-6050 found at 0x%02X.\n", active_addr);
        
        if (chip_id == MPU6050_EXPECTED_ID) {
            printf("[SUCCESS] WHO_AM_I verified (0x%02X).\n", chip_id);
            uint8_t wake_cmd[] = {MPU6050_PWR_MGMT_1, 0x00};
            i2c_write_timeout_us(I2C_PORT, active_addr, wake_cmd, 2, false, 25000);
            printf("IMU Wake command sent successfully.\n");
            sleep_ms(100); 

            printf("Reading Accelerometer data (Move the board!)...\n");
            
            for (int i = 0; i < 15; i++) {
                uint8_t accel_reg = MPU6050_ACCEL_XOUT_H;
                uint8_t buffer[6];

                i2c_write_timeout_us(I2C_PORT, active_addr, &accel_reg, 1, true, 25000);
                i2c_read_timeout_us(I2C_PORT, active_addr, buffer, 6, false, 25000);

                int16_t raw_ax = (buffer[0] << 8) | buffer[1];
                int16_t raw_ay = (buffer[2] << 8) | buffer[3];
                int16_t raw_az = (buffer[4] << 8) | buffer[5];

                float ax = raw_ax / 16384.0f;
                float ay = raw_ay / 16384.0f;
                float az = raw_az / 16384.0f;

                printf("\rAx: %6.2f g  |  Ay: %6.2f g  |  Az: %6.2f g    ", ax, ay, az);
                fflush(stdout); 
                sleep_ms(200); 
            }
            printf("\n\nIMU Data Read Complete.\n");

        } else {
            printf("[WARNING] Wrong WHO_AM_I. Expected 0x68, Got 0x%02X.\n", chip_id);
        }
    } else {
        printf("[ERROR] MPU-6050 not responding at 0x68 or 0x69.\n");
    }
}

void test_ina219() {
    printf("\n--- TESTING INA219 ---\n");
    uint8_t reg = INA219_REG_CONFIG;
    uint8_t data[2];

    int ret = i2c_write_timeout_us(I2C_PORT, ADDR_INA219, &reg, 1, true, 25000);
    if (ret >= 0) {
        i2c_read_timeout_us(I2C_PORT, ADDR_INA219, data, 2, false, 25000);
        uint16_t config_val = (data[0] << 8) | data[1];
        
        printf("[SUCCESS] INA219 responded at 0x%02X.\n", ADDR_INA219);
        printf("Configuration Register: 0x%04X\n", config_val);
        printf("Reading live voltage data...\n\n");

        for (int i = 0; i < 50; i++) {
            uint8_t reg_bus = 0x02; 
            uint8_t reg_shunt = 0x01; 
            uint8_t buf[2];

            i2c_write_timeout_us(I2C_PORT, ADDR_INA219, &reg_bus, 1, true, 25000);
            i2c_read_timeout_us(I2C_PORT, ADDR_INA219, buf, 2, false, 25000);
            
            int16_t bus_raw = (buf[0] << 8) | buf[1];
            bus_raw >>= 3; 
            float bus_voltage = bus_raw * 0.004f; 

            i2c_write_timeout_us(I2C_PORT, ADDR_INA219, &reg_shunt, 1, true, 25000);
            i2c_read_timeout_us(I2C_PORT, ADDR_INA219, buf, 2, false, 25000);
            
            int16_t shunt_raw = (buf[0] << 8) | buf[1];
            float shunt_voltage = shunt_raw * 0.01f; 

            printf("\rBus Voltage: %5.2f V  |  Shunt Voltage: %6.2f mV    ", bus_voltage, shunt_voltage);
            fflush(stdout);
            
            sleep_ms(200);
        }
        printf("\n\nINA219 Data Read Complete.\n");

    } else {
        printf("[ERROR] INA219 not responding at 0x%02X.\n", ADDR_INA219);
    }
}

void test_ldr() {
    printf("\n--- TESTING LDR (LIGHT SENSOR) ---\n");

    adc_init();
    adc_gpio_init(26);
    adc_select_input(0);

    printf("Reading live light data. Shine a flashlight or cover the sensor!\n\n");

    for (int i = 0; i < 50; i++) {
        uint16_t raw_adc = adc_read();
        float voltage = raw_adc * 3.3f / 4096.0f;
        printf("\rADC Raw: %04d  |  Voltage: %4.2f V    ", raw_adc, voltage);
        fflush(stdout);
        sleep_ms(200);
    }
    printf("\n\nLDR Data Read Complete.\n");
}

void test_gpio_bounce() {
    printf("\n--- GPIO BOUNCE TEST (PINS %d & %d) ---\n", SDA_PIN, SCL_PIN);
    printf("Proving RP2350 physical connection to I2C lines.\n");
    printf("Measure the voltage at the sensor pins. Press any key to stop.\n\n");

    i2c_deinit(I2C_PORT);
    
    gpio_init(SDA_PIN);
    gpio_set_dir(SDA_PIN, GPIO_OUT);
    gpio_init(SCL_PIN);
    gpio_set_dir(SCL_PIN, GPIO_OUT);

    bool state = false;
    while (getchar_timeout_us(0) == PICO_ERROR_TIMEOUT) {
        state = !state;
        
        gpio_put(SDA_PIN, state);
        gpio_put(SCL_PIN, state);
        
        if(state) {
            printf("\rPins HIGH (Should measure ~3.3V) ");
        } else {
            printf("\rPins LOW  (Should measure ~0.0V) ");
        }
        fflush(stdout);
        
        sleep_ms(3000); 
    }
    
    gpio_set_function(SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(SCL_PIN, GPIO_FUNC_I2C);
    i2c_init(I2C_PORT, 100 * 1000);
    
    printf("\n\nBounce test stopped. I2C restored.\n");
}

void test_microphone() {
    printf("\n--- TESTING SPH064 MICROPHONE (PDM) ---\n");
    
    // De-init PWM and take manual control of the pins
    gpio_init(MIC_CLK_PIN);
    gpio_set_dir(MIC_CLK_PIN, GPIO_OUT);
    gpio_init(MIC_DAT_PIN);
    gpio_set_dir(MIC_DAT_PIN, GPIO_IN);

    printf("Using Synchronous Bit-Banged Clock on GPIO22.\n");
    printf("Reading perfect lockstep PDM data. Clap directly over the mic!\n\n");

    int absolute_max_peak = 0;

    // Loop until user presses a key
    while (getchar_timeout_us(0) == PICO_ERROR_TIMEOUT) {
        int local_max_deviation = 0;

        // Take 20 rapid snapshots
        for (int snapshot = 0; snapshot < 20; snapshot++) {
            int high_count = 0;
            int total_samples = 1000;

            // Manually generate the clock and sample the data in lockstep
            for(int j=0; j < total_samples; j++) {
                gpio_put(MIC_CLK_PIN, 0); // Clock Falling Edge
                
                // 4 NOPs (No Operation) to give the mic silicon time to push the data
                __asm volatile ("nop\nnop\nnop\nnop\n"); 

                gpio_put(MIC_CLK_PIN, 1); // Clock Rising Edge
                
                // Sample exactly on the rising edge
                if (gpio_get(MIC_DAT_PIN)) high_count++; 
                
                __asm volatile ("nop\nnop\nnop\nnop\n"); 
            }

            // Calculate deviation from perfect 50/50 silence
            int center = total_samples / 2;
            int deviation = high_count - center;
            if (deviation < 0) deviation = -deviation; // Absolute value

            // Scale the score to be highly sensitive for testing
            int score = (deviation * 100) / (center / 2); 

            if (score > local_max_deviation) {
                local_max_deviation = score;
            }
        }

        if (local_max_deviation > absolute_max_peak) absolute_max_peak = local_max_deviation;

        printf("\rVolume: [");
        for (int v = 0; v < 25; v++) {
            if (v < local_max_deviation) printf("#");
            else printf(" ");
        }
        printf("]  Current: %2d  |  MAX PEAK: %2d    ", local_max_deviation, absolute_max_peak);
        fflush(stdout);
        
        sleep_ms(50);
    }
    printf("\n\nMicrophone Test Complete.\n");
}