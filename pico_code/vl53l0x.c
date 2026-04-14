/*
 * VL53L0X Time-of-Flight sensor driver — Pure C
 * Target : RP2350A (Raspberry Pi Pico 2) — Pico SDK hardware/i2c
 *
 * Ported from Pololu's Arduino C++ library (MIT licence).
 */

#include "vl53l0x.h"

#include <string.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"

/* ── Compile-time helpers ───────────────────────────────────────────────── */
#define decodeVcselPeriod(r)      (((r) + 1) << 1)
#define encodeVcselPeriod(p)      (((p) >> 1) - 1)
#define calcMacroPeriod(p)        ((((uint32_t)2304 * (p) * 1655u) + 500u) / 1000u)

/* ── Timeout helpers using Pico time_us_32() ────────────────────────────── */
static inline void start_timeout(VL53L0X *dev)
{
    dev->timeout_start_ms = to_ms_since_boot(get_absolute_time());
}

static inline bool check_timeout_expired(VL53L0X *dev)
{
    if (dev->io_timeout == 0) return false;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    return (uint16_t)(now - dev->timeout_start_ms) > dev->io_timeout;
}

/* ── Private forward declarations ───────────────────────────────────────── */
static bool     get_spad_info(VL53L0X *dev, uint8_t *count, bool *type_is_aperture);
static void     get_sequence_step_enables(VL53L0X *dev, SequenceStepEnables *e);
static void     get_sequence_step_timeouts(VL53L0X *dev,
                    const SequenceStepEnables *e, SequenceStepTimeouts *t);
static uint16_t decode_timeout(uint16_t val);
static uint16_t encode_timeout(uint32_t mclks);
static uint32_t mclks_to_us(uint16_t mclks, uint8_t vcsel_pclks);
static uint32_t us_to_mclks(uint32_t us,    uint8_t vcsel_pclks);
static bool     single_ref_calibration(VL53L0X *dev, uint8_t vhv_init_byte);

/* ══════════════════════════════════════════════════════════════════════════
   LOW-LEVEL REGISTER I/O
   All Pico SDK i2c functions used here:
     i2c_write_blocking(inst, addr, src, len, nostop)
     i2c_read_blocking (inst, addr, dst, len, nostop)
   Return value is number of bytes transferred, or PICO_ERROR_GENERIC (-1).
   ══════════════════════════════════════════════════════════════════════════ */

void vl53l0x_write_reg(VL53L0X *dev, uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    dev->last_status = i2c_write_blocking(dev->i2c, dev->address, buf, 2, false);
}

void vl53l0x_write_reg16(VL53L0X *dev, uint8_t reg, uint16_t value)
{
    uint8_t buf[3] = { reg, (uint8_t)(value >> 8), (uint8_t)value };
    dev->last_status = i2c_write_blocking(dev->i2c, dev->address, buf, 3, false);
}

void vl53l0x_write_reg32(VL53L0X *dev, uint8_t reg, uint32_t value)
{
    uint8_t buf[5] = {
        reg,
        (uint8_t)(value >> 24),
        (uint8_t)(value >> 16),
        (uint8_t)(value >>  8),
        (uint8_t) value
    };
    dev->last_status = i2c_write_blocking(dev->i2c, dev->address, buf, 5, false);
}

uint8_t vl53l0x_read_reg(VL53L0X *dev, uint8_t reg)
{
    uint8_t value = 0;
    /* write register address with nostop=true, then read */
    i2c_write_blocking(dev->i2c, dev->address, &reg, 1, true);
    i2c_read_blocking (dev->i2c, dev->address, &value, 1, false);
    return value;
}

uint16_t vl53l0x_read_reg16(VL53L0X *dev, uint8_t reg)
{
    uint8_t buf[2] = {0};
    i2c_write_blocking(dev->i2c, dev->address, &reg, 1, true);
    i2c_read_blocking (dev->i2c, dev->address, buf,  2, false);
    return ((uint16_t)buf[0] << 8) | buf[1];
}

uint32_t vl53l0x_read_reg32(VL53L0X *dev, uint8_t reg)
{
    uint8_t buf[4] = {0};
    i2c_write_blocking(dev->i2c, dev->address, &reg, 1, true);
    i2c_read_blocking (dev->i2c, dev->address, buf,  4, false);
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] <<  8) |  (uint32_t)buf[3];
}

void vl53l0x_write_multi(VL53L0X *dev, uint8_t reg, const uint8_t *src, uint8_t count)
{
    /* max count in practice is 6 (SPAD map), safe stack buffer */
    uint8_t buf[64];
    buf[0] = reg;
    memcpy(buf + 1, src, count);
    dev->last_status = i2c_write_blocking(dev->i2c, dev->address, buf, (size_t)count + 1, false);
}

void vl53l0x_read_multi(VL53L0X *dev, uint8_t reg, uint8_t *dst, uint8_t count)
{
    i2c_write_blocking(dev->i2c, dev->address, &reg, 1,     true);
    i2c_read_blocking (dev->i2c, dev->address, dst,  count, false);
}

/* ══════════════════════════════════════════════════════════════════════════
   LIFECYCLE
   ══════════════════════════════════════════════════════════════════════════ */

void vl53l0x_init_device(VL53L0X   *dev,
                         i2c_inst_t *i2c,
                         uint        sda_pin,
                         uint        scl_pin,
                         uint8_t     address,
                         uint        freq_hz)
{
    memset(dev, 0, sizeof(*dev));
    dev->i2c     = i2c;
    dev->address = address ? address : VL53L0X_ADDRESS_DEFAULT;

    /* Initialise Pico I2C peripheral */
    i2c_init(i2c, freq_hz);
    gpio_set_function(sda_pin, GPIO_FUNC_I2C);
    gpio_set_function(scl_pin, GPIO_FUNC_I2C);
    gpio_pull_up(sda_pin);
    gpio_pull_up(scl_pin);
}

bool vl53l0x_init(VL53L0X *dev, bool io_2v8)
{
    /* ── Verify sensor presence ── */
    if (vl53l0x_read_reg(dev, IDENTIFICATION_MODEL_ID) != 0xEE) return false;

    /* ── DataInit ── */
    if (io_2v8) {
        vl53l0x_write_reg(dev, VHV_CONFIG_PAD_SCL_SDA__EXTSUP_HV,
            vl53l0x_read_reg(dev, VHV_CONFIG_PAD_SCL_SDA__EXTSUP_HV) | 0x01);
    }

    vl53l0x_write_reg(dev, 0x88, 0x00);
    vl53l0x_write_reg(dev, 0x80, 0x01);
    vl53l0x_write_reg(dev, 0xFF, 0x01);
    vl53l0x_write_reg(dev, 0x00, 0x00);
    dev->stop_variable = vl53l0x_read_reg(dev, 0x91);
    vl53l0x_write_reg(dev, 0x00, 0x01);
    vl53l0x_write_reg(dev, 0xFF, 0x00);
    vl53l0x_write_reg(dev, 0x80, 0x00);

    /* Disable SIGNAL_RATE_MSRC and SIGNAL_RATE_PRE_RANGE limit checks */
    vl53l0x_write_reg(dev, MSRC_CONFIG_CONTROL,
        vl53l0x_read_reg(dev, MSRC_CONFIG_CONTROL) | 0x12);

    vl53l0x_set_signal_rate_limit(dev, 0.25f);
    vl53l0x_write_reg(dev, SYSTEM_SEQUENCE_CONFIG, 0xFF);

    /* ── StaticInit — SPAD setup ── */
    uint8_t spad_count;
    bool    spad_aperture;
    if (!get_spad_info(dev, &spad_count, &spad_aperture)) return false;

    uint8_t ref_spad_map[6];
    vl53l0x_read_multi(dev, GLOBAL_CONFIG_SPAD_ENABLES_REF_0, ref_spad_map, 6);

    vl53l0x_write_reg(dev, 0xFF, 0x01);
    vl53l0x_write_reg(dev, DYNAMIC_SPAD_REF_EN_START_OFFSET, 0x00);
    vl53l0x_write_reg(dev, DYNAMIC_SPAD_NUM_REQUESTED_REF_SPAD, 0x2C);
    vl53l0x_write_reg(dev, 0xFF, 0x00);
    vl53l0x_write_reg(dev, GLOBAL_CONFIG_REF_EN_START_SELECT, 0xB4);

    uint8_t first = spad_aperture ? 12 : 0;
    uint8_t enabled = 0;
    for (uint8_t i = 0; i < 48; i++) {
        if (i < first || enabled == spad_count)
            ref_spad_map[i / 8] &= ~(1 << (i % 8));
        else if ((ref_spad_map[i / 8] >> (i % 8)) & 0x1)
            enabled++;
    }
    vl53l0x_write_multi(dev, GLOBAL_CONFIG_SPAD_ENABLES_REF_0, ref_spad_map, 6);

    /* ── Tuning settings (from vl53l0x_tuning.h) ── */
    vl53l0x_write_reg(dev, 0xFF, 0x01); vl53l0x_write_reg(dev, 0x00, 0x00);
    vl53l0x_write_reg(dev, 0xFF, 0x00); vl53l0x_write_reg(dev, 0x09, 0x00);
    vl53l0x_write_reg(dev, 0x10, 0x00); vl53l0x_write_reg(dev, 0x11, 0x00);
    vl53l0x_write_reg(dev, 0x24, 0x01); vl53l0x_write_reg(dev, 0x25, 0xFF);
    vl53l0x_write_reg(dev, 0x75, 0x00);
    vl53l0x_write_reg(dev, 0xFF, 0x01); vl53l0x_write_reg(dev, 0x4E, 0x2C);
    vl53l0x_write_reg(dev, 0x48, 0x00); vl53l0x_write_reg(dev, 0x30, 0x20);
    vl53l0x_write_reg(dev, 0xFF, 0x00); vl53l0x_write_reg(dev, 0x30, 0x09);
    vl53l0x_write_reg(dev, 0x54, 0x00); vl53l0x_write_reg(dev, 0x31, 0x04);
    vl53l0x_write_reg(dev, 0x32, 0x03); vl53l0x_write_reg(dev, 0x40, 0x83);
    vl53l0x_write_reg(dev, 0x46, 0x25); vl53l0x_write_reg(dev, 0x60, 0x00);
    vl53l0x_write_reg(dev, 0x27, 0x00); vl53l0x_write_reg(dev, 0x50, 0x06);
    vl53l0x_write_reg(dev, 0x51, 0x00); vl53l0x_write_reg(dev, 0x52, 0x96);
    vl53l0x_write_reg(dev, 0x56, 0x08); vl53l0x_write_reg(dev, 0x57, 0x30);
    vl53l0x_write_reg(dev, 0x61, 0x00); vl53l0x_write_reg(dev, 0x62, 0x00);
    vl53l0x_write_reg(dev, 0x64, 0x00); vl53l0x_write_reg(dev, 0x65, 0x00);
    vl53l0x_write_reg(dev, 0x66, 0xA0);
    vl53l0x_write_reg(dev, 0xFF, 0x01); vl53l0x_write_reg(dev, 0x22, 0x32);
    vl53l0x_write_reg(dev, 0x47, 0x14); vl53l0x_write_reg(dev, 0x49, 0xFF);
    vl53l0x_write_reg(dev, 0x4A, 0x00);
    vl53l0x_write_reg(dev, 0xFF, 0x00); vl53l0x_write_reg(dev, 0x7A, 0x0A);
    vl53l0x_write_reg(dev, 0x7B, 0x00); vl53l0x_write_reg(dev, 0x78, 0x21);
    vl53l0x_write_reg(dev, 0xFF, 0x01); vl53l0x_write_reg(dev, 0x23, 0x34);
    vl53l0x_write_reg(dev, 0x42, 0x00); vl53l0x_write_reg(dev, 0x44, 0xFF);
    vl53l0x_write_reg(dev, 0x45, 0x26); vl53l0x_write_reg(dev, 0x46, 0x05);
    vl53l0x_write_reg(dev, 0x40, 0x40); vl53l0x_write_reg(dev, 0x0E, 0x06);
    vl53l0x_write_reg(dev, 0x20, 0x1A); vl53l0x_write_reg(dev, 0x43, 0x40);
    vl53l0x_write_reg(dev, 0xFF, 0x00); vl53l0x_write_reg(dev, 0x34, 0x03);
    vl53l0x_write_reg(dev, 0x35, 0x44);
    vl53l0x_write_reg(dev, 0xFF, 0x01); vl53l0x_write_reg(dev, 0x31, 0x04);
    vl53l0x_write_reg(dev, 0x4B, 0x09); vl53l0x_write_reg(dev, 0x4C, 0x05);
    vl53l0x_write_reg(dev, 0x4D, 0x04);
    vl53l0x_write_reg(dev, 0xFF, 0x00); vl53l0x_write_reg(dev, 0x44, 0x00);
    vl53l0x_write_reg(dev, 0x45, 0x20); vl53l0x_write_reg(dev, 0x47, 0x08);
    vl53l0x_write_reg(dev, 0x48, 0x28); vl53l0x_write_reg(dev, 0x67, 0x00);
    vl53l0x_write_reg(dev, 0x70, 0x04); vl53l0x_write_reg(dev, 0x71, 0x01);
    vl53l0x_write_reg(dev, 0x72, 0xFE); vl53l0x_write_reg(dev, 0x76, 0x00);
    vl53l0x_write_reg(dev, 0x77, 0x00);
    vl53l0x_write_reg(dev, 0xFF, 0x01); vl53l0x_write_reg(dev, 0x0D, 0x01);
    vl53l0x_write_reg(dev, 0xFF, 0x00); vl53l0x_write_reg(dev, 0x80, 0x01);
    vl53l0x_write_reg(dev, 0x01, 0xF8);
    vl53l0x_write_reg(dev, 0xFF, 0x01); vl53l0x_write_reg(dev, 0x8E, 0x01);
    vl53l0x_write_reg(dev, 0x00, 0x01); vl53l0x_write_reg(dev, 0xFF, 0x00);
    vl53l0x_write_reg(dev, 0x80, 0x00);

    /* GPIO — interrupt on new sample, active low */
    vl53l0x_write_reg(dev, SYSTEM_INTERRUPT_CONFIG_GPIO, 0x04);
    vl53l0x_write_reg(dev, GPIO_HV_MUX_ACTIVE_HIGH,
        vl53l0x_read_reg(dev, GPIO_HV_MUX_ACTIVE_HIGH) & ~0x10u);
    vl53l0x_write_reg(dev, SYSTEM_INTERRUPT_CLEAR, 0x01);

    dev->measurement_timing_budget_us = vl53l0x_get_measurement_timing_budget(dev);

    /* Disable MSRC and TCC */
    vl53l0x_write_reg(dev, SYSTEM_SEQUENCE_CONFIG, 0xE8);
    vl53l0x_set_measurement_timing_budget(dev, dev->measurement_timing_budget_us);

    /* ── Reference calibration ── */
    vl53l0x_write_reg(dev, SYSTEM_SEQUENCE_CONFIG, 0x01);
    if (!single_ref_calibration(dev, 0x40)) return false;

    vl53l0x_write_reg(dev, SYSTEM_SEQUENCE_CONFIG, 0x02);
    if (!single_ref_calibration(dev, 0x00)) return false;

    vl53l0x_write_reg(dev, SYSTEM_SEQUENCE_CONFIG, 0xE8);

    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
   ADDRESS
   ══════════════════════════════════════════════════════════════════════════ */

void vl53l0x_set_address(VL53L0X *dev, uint8_t new_addr)
{
    vl53l0x_write_reg(dev, I2C_SLAVE_DEVICE_ADDRESS, new_addr & 0x7Fu);
    dev->address = new_addr;
}

/* ══════════════════════════════════════════════════════════════════════════
   TIMEOUT
   ══════════════════════════════════════════════════════════════════════════ */

bool vl53l0x_timeout_occurred(VL53L0X *dev)
{
    bool tmp = dev->did_timeout;
    dev->did_timeout = false;
    return tmp;
}

/* ══════════════════════════════════════════════════════════════════════════
   SIGNAL RATE LIMIT
   ══════════════════════════════════════════════════════════════════════════ */

bool vl53l0x_set_signal_rate_limit(VL53L0X *dev, float limit_Mcps)
{
    if (limit_Mcps < 0.0f || limit_Mcps > 511.99f) return false;
    vl53l0x_write_reg16(dev, FINAL_RANGE_CONFIG_MIN_COUNT_RATE_RTN_LIMIT,
                        (uint16_t)(limit_Mcps * (1 << 7)));
    return true;
}

float vl53l0x_get_signal_rate_limit(VL53L0X *dev)
{
    return (float)vl53l0x_read_reg16(dev, FINAL_RANGE_CONFIG_MIN_COUNT_RATE_RTN_LIMIT)
           / (float)(1 << 7);
}

/* ══════════════════════════════════════════════════════════════════════════
   MEASUREMENT TIMING BUDGET
   ══════════════════════════════════════════════════════════════════════════ */

bool vl53l0x_set_measurement_timing_budget(VL53L0X *dev, uint32_t budget_us)
{
    SequenceStepEnables  en;
    SequenceStepTimeouts to;

    const uint16_t kStart      = 1910, kEnd    = 960;
    const uint16_t kMsrc       =  660, kTcc    = 590;
    const uint16_t kDss        =  690, kPre    = 660;
    const uint16_t kFinal      =  550;

    uint32_t used = kStart + kEnd;

    get_sequence_step_enables(dev, &en);
    get_sequence_step_timeouts(dev, &en, &to);

    if (en.tcc)       used += to.msrc_dss_tcc_us + kTcc;
    if (en.dss)       used += 2 * (to.msrc_dss_tcc_us + kDss);
    else if (en.msrc) used += to.msrc_dss_tcc_us + kMsrc;
    if (en.pre_range) used += to.pre_range_us + kPre;

    if (en.final_range) {
        used += kFinal;
        if (used > budget_us) return false;

        uint32_t final_us    = budget_us - used;
        uint32_t final_mclks = us_to_mclks(final_us, to.final_range_vcsel_period_pclks);
        if (en.pre_range) final_mclks += to.pre_range_mclks;

        vl53l0x_write_reg16(dev, FINAL_RANGE_CONFIG_TIMEOUT_MACROP_HI,
                            encode_timeout(final_mclks));
        dev->measurement_timing_budget_us = budget_us;
    }
    return true;
}

uint32_t vl53l0x_get_measurement_timing_budget(VL53L0X *dev)
{
    SequenceStepEnables  en;
    SequenceStepTimeouts to;

    const uint16_t kStart = 1910, kEnd = 960;
    const uint16_t kMsrc  =  660, kTcc = 590;
    const uint16_t kDss   =  690, kPre = 660, kFinal = 550;

    uint32_t budget = kStart + kEnd;

    get_sequence_step_enables(dev, &en);
    get_sequence_step_timeouts(dev, &en, &to);

    if (en.tcc)       budget += to.msrc_dss_tcc_us + kTcc;
    if (en.dss)       budget += 2 * (to.msrc_dss_tcc_us + kDss);
    else if (en.msrc) budget += to.msrc_dss_tcc_us + kMsrc;
    if (en.pre_range)  budget += to.pre_range_us + kPre;
    if (en.final_range) budget += to.final_range_us + kFinal;

    dev->measurement_timing_budget_us = budget;
    return budget;
}

/* ══════════════════════════════════════════════════════════════════════════
   VCSEL PULSE PERIOD
   ══════════════════════════════════════════════════════════════════════════ */

bool vl53l0x_set_vcsel_pulse_period(VL53L0X *dev, vcselPeriodType type, uint8_t period_pclks)
{
    uint8_t vcsel_reg = (uint8_t)encodeVcselPeriod(period_pclks);
    SequenceStepEnables  en;
    SequenceStepTimeouts to;
    get_sequence_step_enables(dev, &en);
    get_sequence_step_timeouts(dev, &en, &to);

    if (type == VcselPeriodPreRange) {
        switch (period_pclks) {
            case 12: vl53l0x_write_reg(dev, PRE_RANGE_CONFIG_VALID_PHASE_HIGH, 0x18); break;
            case 14: vl53l0x_write_reg(dev, PRE_RANGE_CONFIG_VALID_PHASE_HIGH, 0x30); break;
            case 16: vl53l0x_write_reg(dev, PRE_RANGE_CONFIG_VALID_PHASE_HIGH, 0x40); break;
            case 18: vl53l0x_write_reg(dev, PRE_RANGE_CONFIG_VALID_PHASE_HIGH, 0x50); break;
            default: return false;
        }
        vl53l0x_write_reg(dev, PRE_RANGE_CONFIG_VALID_PHASE_LOW, 0x08);
        vl53l0x_write_reg(dev, PRE_RANGE_CONFIG_VCSEL_PERIOD, vcsel_reg);

        uint16_t new_pre   = (uint16_t)us_to_mclks(to.pre_range_us,      period_pclks);
        uint16_t new_msrc  = (uint16_t)us_to_mclks(to.msrc_dss_tcc_us,   period_pclks);
        vl53l0x_write_reg16(dev, PRE_RANGE_CONFIG_TIMEOUT_MACROP_HI, encode_timeout(new_pre));
        vl53l0x_write_reg(dev, MSRC_CONFIG_TIMEOUT_MACROP,
            (new_msrc > 256) ? 255u : (uint8_t)(new_msrc - 1));

    } else if (type == VcselPeriodFinalRange) {
        switch (period_pclks) {
            case 8:
                vl53l0x_write_reg(dev, FINAL_RANGE_CONFIG_VALID_PHASE_HIGH, 0x10);
                vl53l0x_write_reg(dev, FINAL_RANGE_CONFIG_VALID_PHASE_LOW,  0x08);
                vl53l0x_write_reg(dev, GLOBAL_CONFIG_VCSEL_WIDTH, 0x02);
                vl53l0x_write_reg(dev, ALGO_PHASECAL_CONFIG_TIMEOUT, 0x0C);
                vl53l0x_write_reg(dev, 0xFF, 0x01);
                vl53l0x_write_reg(dev, ALGO_PHASECAL_LIM, 0x30);
                vl53l0x_write_reg(dev, 0xFF, 0x00);
                break;
            case 10:
                vl53l0x_write_reg(dev, FINAL_RANGE_CONFIG_VALID_PHASE_HIGH, 0x28);
                vl53l0x_write_reg(dev, FINAL_RANGE_CONFIG_VALID_PHASE_LOW,  0x08);
                vl53l0x_write_reg(dev, GLOBAL_CONFIG_VCSEL_WIDTH, 0x03);
                vl53l0x_write_reg(dev, ALGO_PHASECAL_CONFIG_TIMEOUT, 0x09);
                vl53l0x_write_reg(dev, 0xFF, 0x01);
                vl53l0x_write_reg(dev, ALGO_PHASECAL_LIM, 0x20);
                vl53l0x_write_reg(dev, 0xFF, 0x00);
                break;
            case 12:
                vl53l0x_write_reg(dev, FINAL_RANGE_CONFIG_VALID_PHASE_HIGH, 0x38);
                vl53l0x_write_reg(dev, FINAL_RANGE_CONFIG_VALID_PHASE_LOW,  0x08);
                vl53l0x_write_reg(dev, GLOBAL_CONFIG_VCSEL_WIDTH, 0x03);
                vl53l0x_write_reg(dev, ALGO_PHASECAL_CONFIG_TIMEOUT, 0x08);
                vl53l0x_write_reg(dev, 0xFF, 0x01);
                vl53l0x_write_reg(dev, ALGO_PHASECAL_LIM, 0x20);
                vl53l0x_write_reg(dev, 0xFF, 0x00);
                break;
            case 14:
                vl53l0x_write_reg(dev, FINAL_RANGE_CONFIG_VALID_PHASE_HIGH, 0x48);
                vl53l0x_write_reg(dev, FINAL_RANGE_CONFIG_VALID_PHASE_LOW,  0x08);
                vl53l0x_write_reg(dev, GLOBAL_CONFIG_VCSEL_WIDTH, 0x03);
                vl53l0x_write_reg(dev, ALGO_PHASECAL_CONFIG_TIMEOUT, 0x07);
                vl53l0x_write_reg(dev, 0xFF, 0x01);
                vl53l0x_write_reg(dev, ALGO_PHASECAL_LIM, 0x20);
                vl53l0x_write_reg(dev, 0xFF, 0x00);
                break;
            default: return false;
        }
        vl53l0x_write_reg(dev, FINAL_RANGE_CONFIG_VCSEL_PERIOD, vcsel_reg);

        uint16_t new_final = (uint16_t)us_to_mclks(to.final_range_us, period_pclks);
        if (en.pre_range) new_final += to.pre_range_mclks;
        vl53l0x_write_reg16(dev, FINAL_RANGE_CONFIG_TIMEOUT_MACROP_HI,
                            encode_timeout(new_final));
    } else {
        return false;
    }

    vl53l0x_set_measurement_timing_budget(dev, dev->measurement_timing_budget_us);

    /* Phase calibration after VCSEL period change */
    uint8_t seq = vl53l0x_read_reg(dev, SYSTEM_SEQUENCE_CONFIG);
    vl53l0x_write_reg(dev, SYSTEM_SEQUENCE_CONFIG, 0x02);
    single_ref_calibration(dev, 0x00);
    vl53l0x_write_reg(dev, SYSTEM_SEQUENCE_CONFIG, seq);

    return true;
}

uint8_t vl53l0x_get_vcsel_pulse_period(VL53L0X *dev, vcselPeriodType type)
{
    if (type == VcselPeriodPreRange)
        return decodeVcselPeriod(vl53l0x_read_reg(dev, PRE_RANGE_CONFIG_VCSEL_PERIOD));
    if (type == VcselPeriodFinalRange)
        return decodeVcselPeriod(vl53l0x_read_reg(dev, FINAL_RANGE_CONFIG_VCSEL_PERIOD));
    return 255;
}

/* ══════════════════════════════════════════════════════════════════════════
   RANGING
   ══════════════════════════════════════════════════════════════════════════ */

void vl53l0x_start_continuous(VL53L0X *dev, uint32_t period_ms)
{
    vl53l0x_write_reg(dev, 0x80, 0x01);
    vl53l0x_write_reg(dev, 0xFF, 0x01);
    vl53l0x_write_reg(dev, 0x00, 0x00);
    vl53l0x_write_reg(dev, 0x91, dev->stop_variable);
    vl53l0x_write_reg(dev, 0x00, 0x01);
    vl53l0x_write_reg(dev, 0xFF, 0x00);
    vl53l0x_write_reg(dev, 0x80, 0x00);

    if (period_ms != 0) {
        uint16_t osc_cal = vl53l0x_read_reg16(dev, OSC_CALIBRATE_VAL);
        if (osc_cal != 0) period_ms *= osc_cal;
        vl53l0x_write_reg32(dev, SYSTEM_INTERMEASUREMENT_PERIOD, period_ms);
        vl53l0x_write_reg(dev, SYSRANGE_START, 0x04); /* timed mode */
    } else {
        vl53l0x_write_reg(dev, SYSRANGE_START, 0x02); /* back-to-back mode */
    }
}

void vl53l0x_stop_continuous(VL53L0X *dev)
{
    vl53l0x_write_reg(dev, SYSRANGE_START, 0x01);
    vl53l0x_write_reg(dev, 0xFF, 0x01);
    vl53l0x_write_reg(dev, 0x00, 0x00);
    vl53l0x_write_reg(dev, 0x91, 0x00);
    vl53l0x_write_reg(dev, 0x00, 0x01);
    vl53l0x_write_reg(dev, 0xFF, 0x00);
}

uint16_t vl53l0x_read_range_continuous_mm(VL53L0X *dev)
{
    start_timeout(dev);
    while ((vl53l0x_read_reg(dev, RESULT_INTERRUPT_STATUS) & 0x07) == 0) {
        if (check_timeout_expired(dev)) {
            dev->did_timeout = true;
            return VL53L0X_RANGE_TIMEOUT;
        }
    }

    uint16_t range = vl53l0x_read_reg16(dev, RESULT_RANGE_STATUS + 10);
    vl53l0x_write_reg(dev, SYSTEM_INTERRUPT_CLEAR, 0x01);
    return range;
}

uint16_t vl53l0x_read_range_single_mm(VL53L0X *dev)
{
    vl53l0x_write_reg(dev, 0x80, 0x01);
    vl53l0x_write_reg(dev, 0xFF, 0x01);
    vl53l0x_write_reg(dev, 0x00, 0x00);
    vl53l0x_write_reg(dev, 0x91, dev->stop_variable);
    vl53l0x_write_reg(dev, 0x00, 0x01);
    vl53l0x_write_reg(dev, 0xFF, 0x00);
    vl53l0x_write_reg(dev, 0x80, 0x00);
    vl53l0x_write_reg(dev, SYSRANGE_START, 0x01);

    start_timeout(dev);
    while (vl53l0x_read_reg(dev, SYSRANGE_START) & 0x01) {
        if (check_timeout_expired(dev)) {
            dev->did_timeout = true;
            return VL53L0X_RANGE_TIMEOUT;
        }
    }
    return vl53l0x_read_range_continuous_mm(dev);
}

/* ══════════════════════════════════════════════════════════════════════════
   PRIVATE HELPERS
   ══════════════════════════════════════════════════════════════════════════ */

static bool get_spad_info(VL53L0X *dev, uint8_t *count, bool *type_is_aperture)
{
    vl53l0x_write_reg(dev, 0x80, 0x01);
    vl53l0x_write_reg(dev, 0xFF, 0x01);
    vl53l0x_write_reg(dev, 0x00, 0x00);
    vl53l0x_write_reg(dev, 0xFF, 0x06);
    vl53l0x_write_reg(dev, 0x83, vl53l0x_read_reg(dev, 0x83) | 0x04);
    vl53l0x_write_reg(dev, 0xFF, 0x07);
    vl53l0x_write_reg(dev, 0x81, 0x01);
    vl53l0x_write_reg(dev, 0x80, 0x01);
    vl53l0x_write_reg(dev, 0x94, 0x6B);
    vl53l0x_write_reg(dev, 0x83, 0x00);

    start_timeout(dev);
    while (vl53l0x_read_reg(dev, 0x83) == 0x00) {
        if (check_timeout_expired(dev)) return false;
    }

    vl53l0x_write_reg(dev, 0x83, 0x01);
    uint8_t tmp = vl53l0x_read_reg(dev, 0x92);
    *count           = tmp & 0x7Fu;
    *type_is_aperture = (tmp >> 7) & 0x01u;

    vl53l0x_write_reg(dev, 0x81, 0x00);
    vl53l0x_write_reg(dev, 0xFF, 0x06);
    vl53l0x_write_reg(dev, 0x83, vl53l0x_read_reg(dev, 0x83) & ~0x04u);
    vl53l0x_write_reg(dev, 0xFF, 0x01);
    vl53l0x_write_reg(dev, 0x00, 0x01);
    vl53l0x_write_reg(dev, 0xFF, 0x00);
    vl53l0x_write_reg(dev, 0x80, 0x00);
    return true;
}

static void get_sequence_step_enables(VL53L0X *dev, SequenceStepEnables *e)
{
    uint8_t cfg = vl53l0x_read_reg(dev, SYSTEM_SEQUENCE_CONFIG);
    e->tcc         = (cfg >> 4) & 0x1;
    e->dss         = (cfg >> 3) & 0x1;
    e->msrc        = (cfg >> 2) & 0x1;
    e->pre_range   = (cfg >> 6) & 0x1;
    e->final_range = (cfg >> 7) & 0x1;
}

static void get_sequence_step_timeouts(VL53L0X *dev,
                                       const SequenceStepEnables *e,
                                       SequenceStepTimeouts *t)
{
    t->pre_range_vcsel_period_pclks =
        vl53l0x_get_vcsel_pulse_period(dev, VcselPeriodPreRange);

    t->msrc_dss_tcc_mclks = vl53l0x_read_reg(dev, MSRC_CONFIG_TIMEOUT_MACROP) + 1u;
    t->msrc_dss_tcc_us    = mclks_to_us(t->msrc_dss_tcc_mclks,
                                         t->pre_range_vcsel_period_pclks);

    t->pre_range_mclks = decode_timeout(
        vl53l0x_read_reg16(dev, PRE_RANGE_CONFIG_TIMEOUT_MACROP_HI));
    t->pre_range_us    = mclks_to_us(t->pre_range_mclks,
                                      t->pre_range_vcsel_period_pclks);

    t->final_range_vcsel_period_pclks =
        vl53l0x_get_vcsel_pulse_period(dev, VcselPeriodFinalRange);

    t->final_range_mclks = decode_timeout(
        vl53l0x_read_reg16(dev, FINAL_RANGE_CONFIG_TIMEOUT_MACROP_HI));
    if (e->pre_range)
        t->final_range_mclks -= t->pre_range_mclks;

    t->final_range_us = mclks_to_us(t->final_range_mclks,
                                     t->final_range_vcsel_period_pclks);
}

static uint16_t decode_timeout(uint16_t val)
{
    /* format: "(LSByte * 2^MSByte) + 1" */
    return (uint16_t)((val & 0x00FFu) << ((val & 0xFF00u) >> 8)) + 1u;
}

static uint16_t encode_timeout(uint32_t mclks)
{
    if (mclks == 0) return 0;
    uint32_t ls = mclks - 1;
    uint16_t ms = 0;
    while ((ls & 0xFFFFFF00u) > 0) { ls >>= 1; ms++; }
    return (uint16_t)((ms << 8) | (ls & 0xFFu));
}

static uint32_t mclks_to_us(uint16_t mclks, uint8_t vcsel_pclks)
{
    uint32_t macro_ns = calcMacroPeriod(vcsel_pclks);
    return ((mclks * macro_ns) + 500u) / 1000u;
}

static uint32_t us_to_mclks(uint32_t us, uint8_t vcsel_pclks)
{
    uint32_t macro_ns = calcMacroPeriod(vcsel_pclks);
    return (((us * 1000u) + (macro_ns / 2u)) / macro_ns);
}

static bool single_ref_calibration(VL53L0X *dev, uint8_t vhv_init_byte)
{
    vl53l0x_write_reg(dev, SYSRANGE_START, 0x01u | vhv_init_byte);
    start_timeout(dev);
    while ((vl53l0x_read_reg(dev, RESULT_INTERRUPT_STATUS) & 0x07u) == 0) {
        if (check_timeout_expired(dev)) return false;
    }
    vl53l0x_write_reg(dev, SYSTEM_INTERRUPT_CLEAR, 0x01);
    vl53l0x_write_reg(dev, SYSRANGE_START, 0x00);
    return true;
}
