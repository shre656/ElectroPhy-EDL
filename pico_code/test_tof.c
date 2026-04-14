#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "vl53l0x.h"

#define I2C_PORT i2c1
#define SDA_PIN 2
#define SCL_PIN 3

int main() {
    stdio_init_all();
    sleep_ms(3000); 
    printf("\n\n--- VL53L0X SURGICAL BOOT (NO SCANNER) ---\n");

    VL53L0X my_tof;
    // Note: Kept at 400kHz to match your C++ demo
    vl53l0x_init_device(&my_tof, I2C_PORT, SDA_PIN, SCL_PIN, 0x29, 400 * 1000); 

    printf("Attempting to contact 0x29 directly...\n");

    // Manually check the model ID (Register 0xC0) before doing anything else
    uint8_t reg = IDENTIFICATION_MODEL_ID; 
    uint8_t model_id = 0;
    
    int w_ret = i2c_write_blocking(I2C_PORT, 0x29, &reg, 1, true);
    int r_ret = i2c_read_blocking(I2C_PORT, 0x29, &model_id, 1, false);

    if (w_ret < 0 || r_ret < 0) {
        printf("[FATAL ERROR] VL53L0X NACKed the request! Write ret: %d, Read ret: %d\n", w_ret, r_ret);
        printf("- Did you completely power cycle the board?\n");
        printf("- If running on a battery ('no USB power'), is the voltage dropping?\n");
        while(1) { sleep_ms(1000); }
    }

    printf(">>> READ MODEL ID: 0x%02X (Expected: 0xEE)\n", model_id);

    // Boot the sensor logic
    vl53l0x_set_timeout(&my_tof, 40);
    bool success = vl53l0x_init(&my_tof, true);

    if (!success) {
        printf("[FATAL ERROR] Sensor found, but failed the initialization sequence!\n");
        while (1) { sleep_ms(1000); } 
    }

    printf("Success! Sensor Booted. Starting continuous stream...\n");
    vl53l0x_start_continuous(&my_tof, 0);

    while (1) {
        uint16_t dist = vl53l0x_read_range_continuous_mm(&my_tof);
        
        if (vl53l0x_timeout_occurred(&my_tof) || dist > 8000) {
            printf("Distance: [OUT OF RANGE]\n");
        } else {
            printf("Distance: %d mm\n", dist);
        }
        sleep_ms(50); 
    }
    return 0;
}