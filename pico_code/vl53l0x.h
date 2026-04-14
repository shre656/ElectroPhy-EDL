#ifndef VL53L0X_H
#define VL53L0X_H

/*
 * VL53L0X Time-of-Flight sensor driver — Pure C
 * Target : RP2350A (Raspberry Pi Pico 2) using Pico SDK hardware/i2c
 *
 * Default wiring:
 *   SDA → GPIO 4   (VL53L0X_DEFAULT_SDA_PIN)
 *   SCL → GPIO 5   (VL53L0X_DEFAULT_SCL_PIN)
 *   VIN → 3.3 V
 *   GND → GND
 *
 * CMakeLists.txt — link these libraries to your target:
 *   pico_stdlib  hardware_i2c
 */

#include <stdint.h>
#include <stdbool.h>
#include "hardware/i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Default pin / bus — override before including if needed ────────────── */
#ifndef VL53L0X_DEFAULT_I2C
#  define VL53L0X_DEFAULT_I2C      i2c0
#endif
#ifndef VL53L0X_DEFAULT_SDA_PIN
#  define VL53L0X_DEFAULT_SDA_PIN  4
#endif
#ifndef VL53L0X_DEFAULT_SCL_PIN
#  define VL53L0X_DEFAULT_SCL_PIN  5
#endif
#ifndef VL53L0X_I2C_FREQ_HZ
#  define VL53L0X_I2C_FREQ_HZ      400000u   /* 400 kHz fast-mode */
#endif

#define VL53L0X_ADDRESS_DEFAULT  0x29u
#define VL53L0X_RANGE_TIMEOUT    65535u   /* returned on timeout / error */

/* ── Register map ───────────────────────────────────────────────────────── */
typedef enum {
    SYSRANGE_START                              = 0x00,
    SYSTEM_SEQUENCE_CONFIG                      = 0x01,
    SYSTEM_INTERMEASUREMENT_PERIOD              = 0x04,
    SYSTEM_INTERRUPT_CONFIG_GPIO                = 0x0A,
    SYSTEM_INTERRUPT_CLEAR                      = 0x0B,
    RESULT_INTERRUPT_STATUS                     = 0x13,
    RESULT_RANGE_STATUS                         = 0x14,
    MSRC_CONFIG_CONTROL                         = 0x60,
    FINAL_RANGE_CONFIG_MIN_COUNT_RATE_RTN_LIMIT = 0x44,
    FINAL_RANGE_CONFIG_VALID_PHASE_LOW          = 0x47,
    FINAL_RANGE_CONFIG_VALID_PHASE_HIGH         = 0x48,
    MSRC_CONFIG_TIMEOUT_MACROP                  = 0x46,
    PRE_RANGE_CONFIG_VALID_PHASE_LOW            = 0x56,
    PRE_RANGE_CONFIG_VALID_PHASE_HIGH           = 0x57,
    PRE_RANGE_CONFIG_VCSEL_PERIOD               = 0x50,
    PRE_RANGE_CONFIG_TIMEOUT_MACROP_HI          = 0x51,
    PRE_RANGE_CONFIG_TIMEOUT_MACROP_LO          = 0x52,
    FINAL_RANGE_CONFIG_VCSEL_PERIOD             = 0x70,
    FINAL_RANGE_CONFIG_TIMEOUT_MACROP_HI        = 0x71,
    FINAL_RANGE_CONFIG_TIMEOUT_MACROP_LO        = 0x72,
    GLOBAL_CONFIG_VCSEL_WIDTH                   = 0x32,
    GLOBAL_CONFIG_SPAD_ENABLES_REF_0            = 0xB0,
    GLOBAL_CONFIG_REF_EN_START_SELECT           = 0xB6,
    DYNAMIC_SPAD_NUM_REQUESTED_REF_SPAD         = 0x4E,
    DYNAMIC_SPAD_REF_EN_START_OFFSET            = 0x4F,
    GPIO_HV_MUX_ACTIVE_HIGH                     = 0x84,
    VHV_CONFIG_PAD_SCL_SDA__EXTSUP_HV          = 0x89,
    I2C_SLAVE_DEVICE_ADDRESS                    = 0x8A,
    IDENTIFICATION_MODEL_ID                     = 0xC0,
    OSC_CALIBRATE_VAL                           = 0xF8,
    ALGO_PHASECAL_LIM                           = 0x30,
    ALGO_PHASECAL_CONFIG_TIMEOUT                = 0x30,
} VL53L0X_Reg;

/* ── VCSEL period type ──────────────────────────────────────────────────── */
typedef enum {
    VcselPeriodPreRange,
    VcselPeriodFinalRange
} vcselPeriodType;

/* ── Internal structs ───────────────────────────────────────────────────── */
typedef struct {
    bool tcc;
    bool msrc;
    bool dss;
    bool pre_range;
    bool final_range;
} SequenceStepEnables;

typedef struct {
    uint8_t  pre_range_vcsel_period_pclks;
    uint8_t  final_range_vcsel_period_pclks;
    uint16_t msrc_dss_tcc_mclks;
    uint32_t msrc_dss_tcc_us;
    uint16_t pre_range_mclks;
    uint32_t pre_range_us;
    uint16_t final_range_mclks;
    uint32_t final_range_us;
} SequenceStepTimeouts;

/* ── Device handle ──────────────────────────────────────────────────────── */
typedef struct {
    i2c_inst_t *i2c;                        /* i2c0 or i2c1                */
    uint8_t     address;                    /* 7-bit slave address          */
    uint16_t    io_timeout;                 /* ms; 0 = wait forever         */
    bool        did_timeout;
    uint32_t    timeout_start_ms;
    uint32_t    measurement_timing_budget_us;
    uint8_t     stop_variable;
    int         last_status;                /* Pico SDK return code          */
} VL53L0X;

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

/**
 * Initialise the I2C peripheral and fill the device handle.
 * Call once at startup before vl53l0x_init().
 *
 * Example:
 *   VL53L0X tof;
 *   vl53l0x_init_device(&tof, i2c0, 4, 5,
 *                        VL53L0X_ADDRESS_DEFAULT, VL53L0X_I2C_FREQ_HZ);
 *   vl53l0x_init(&tof, true);
 */
void vl53l0x_init_device(VL53L0X   *dev,
                         i2c_inst_t *i2c,
                         uint        sda_pin,
                         uint        scl_pin,
                         uint8_t     address,
                         uint        freq_hz);

/**
 * Configure the sensor (DataInit + StaticInit + RefCalibration).
 * @param io_2v8  true for 2V8 I/O (typical breakout board); false for 1V8.
 * @return true on success
 */
bool vl53l0x_init(VL53L0X *dev, bool io_2v8);

/* ── Address ────────────────────────────────────────────────────────────── */
void vl53l0x_set_address(VL53L0X *dev, uint8_t new_addr);

/* ── Timeout ────────────────────────────────────────────────────────────── */
static inline void     vl53l0x_set_timeout(VL53L0X *dev, uint16_t ms) { dev->io_timeout = ms; }
static inline uint16_t vl53l0x_get_timeout(const VL53L0X *dev)        { return dev->io_timeout; }
bool                   vl53l0x_timeout_occurred(VL53L0X *dev);

/* ── Signal rate limit ──────────────────────────────────────────────────── */
bool  vl53l0x_set_signal_rate_limit(VL53L0X *dev, float limit_Mcps);
float vl53l0x_get_signal_rate_limit(VL53L0X *dev);

/* ── Measurement timing budget ──────────────────────────────────────────── */
bool     vl53l0x_set_measurement_timing_budget(VL53L0X *dev, uint32_t budget_us);
uint32_t vl53l0x_get_measurement_timing_budget(VL53L0X *dev);

/* ── VCSEL pulse period ─────────────────────────────────────────────────── */
bool    vl53l0x_set_vcsel_pulse_period(VL53L0X *dev, vcselPeriodType type, uint8_t period_pclks);
uint8_t vl53l0x_get_vcsel_pulse_period(VL53L0X *dev, vcselPeriodType type);

/* ── Ranging ────────────────────────────────────────────────────────────── */
void     vl53l0x_start_continuous(VL53L0X *dev, uint32_t period_ms);
void     vl53l0x_stop_continuous(VL53L0X *dev);
uint16_t vl53l0x_read_range_continuous_mm(VL53L0X *dev);  /* call in continuous mode  */
uint16_t vl53l0x_read_range_single_mm(VL53L0X *dev);      /* one-shot single reading  */

/* ── Low-level register I/O (exposed for advanced use) ──────────────────── */
void     vl53l0x_write_reg   (VL53L0X *dev, uint8_t reg, uint8_t  value);
void     vl53l0x_write_reg16 (VL53L0X *dev, uint8_t reg, uint16_t value);
void     vl53l0x_write_reg32 (VL53L0X *dev, uint8_t reg, uint32_t value);
uint8_t  vl53l0x_read_reg    (VL53L0X *dev, uint8_t reg);
uint16_t vl53l0x_read_reg16  (VL53L0X *dev, uint8_t reg);
uint32_t vl53l0x_read_reg32  (VL53L0X *dev, uint8_t reg);
void     vl53l0x_write_multi (VL53L0X *dev, uint8_t reg, const uint8_t *src, uint8_t count);
void     vl53l0x_read_multi  (VL53L0X *dev, uint8_t reg, uint8_t *dst,       uint8_t count);

#ifdef __cplusplus
}
#endif

#endif /* VL53L0X_H */
