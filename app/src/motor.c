#include "table_lifter.h"

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/stepper/stepper.h>
#include <zephyr/drivers/stepper/stepper_ctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <errno.h>

LOG_MODULE_REGISTER(lifter_motor, LOG_LEVEL_INF);

#define TMC_NODE DT_NODELABEL(tmc)
#define DRIVER_NODE DT_NODELABEL(driver)
#define RAMP_NODE DT_NODELABEL(ramp)

static const struct device *const driver = DEVICE_DT_GET(DRIVER_NODE);
static const struct device *const ramp = DEVICE_DT_GET(RAMP_NODE);
static const struct spi_dt_spec tmc_spi = SPI_DT_SPEC_GET(
    TMC_NODE, SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_MODE_CPOL | SPI_MODE_CPHA);
static const struct gpio_dt_spec diag0 = GPIO_DT_SPEC_GET(DT_NODELABEL(table_io), tmc_diag0_gpios);
static const struct gpio_dt_spec diag1 = GPIO_DT_SPEC_GET(DT_NODELABEL(table_io), tmc_diag1_gpios);
static bool enabled;
static bool initialized;
static enum lifter_motion running;
static bool running_slow;
static bool running_quiet;
static struct k_spinlock diagnostics_lock;
static struct lifter_motor_diagnostics diagnostics;
static uint32_t previous_warnings;

/* 200 full steps/revolution, 16 microsteps; 30 rpm and 10 rpm/s initially. */
static const struct stepper_ctrl_ramp normal_ramp = {
    .speed_max = 1600, .acceleration_max = 533, .deceleration_max = 533,
};
static const struct stepper_ctrl_ramp slow_ramp = {
    .speed_max = 400, .acceleration_max = 533, .deceleration_max = 533,
};
static const struct stepper_ctrl_ramp quiet_ramp = {
    .speed_max = 800, .acceleration_max = 267, .deceleration_max = 267,
};
static const struct stepper_ctrl_ramp quiet_slow_ramp = {
    .speed_max = 250, .acceleration_max = 267, .deceleration_max = 267,
};

/** @brief Read a TMC5160 register through its pipelined SPI protocol.
 *  @param reg Register address.
 *  @param value Destination for the register value.
 *  @return 0 on success, or a negative errno value.
 */
static int tmc_read(uint8_t reg, uint32_t *value)
{
    uint8_t tx[5] = {reg & 0x7f, 0, 0, 0, 0};
    uint8_t rx[5];
    struct spi_buf txb = {.buf = tx, .len = sizeof(tx)};
    struct spi_buf rxb = {.buf = rx, .len = sizeof(rx)};
    struct spi_buf_set txs = {.buffers = &txb, .count = 1};
    struct spi_buf_set rxs = {.buffers = &rxb, .count = 1};
    int err = spi_transceive_dt(&tmc_spi, &txs, &rxs);

    if (err) return err;
    /* TMC5160 reads are pipelined: response belongs to the preceding frame. */
    err = spi_transceive_dt(&tmc_spi, &txs, &rxs);
    if (err) return err;
    *value = ((uint32_t)rx[1] << 24) | ((uint32_t)rx[2] << 16) |
             ((uint32_t)rx[3] << 8) | rx[4];
    return 0;
}

/** @brief Write a TMC5160 register over SPI.
 *  @param reg Register address.
 *  @param value Register value to write.
 *  @return 0 on success, or a negative errno value.
 */
static int tmc_write(uint8_t reg, uint32_t value)
{
    uint8_t tx[5] = {reg | 0x80u, value >> 24, value >> 16, value >> 8, value};
    struct spi_buf txb = {.buf = tx, .len = sizeof(tx)};
    struct spi_buf_set txs = {.buffers = &txb, .count = 1};
    return spi_write_dt(&tmc_spi, &txs);
}

int lifter_motor_init(void)
{
    uint32_t ioin;
    if (!device_is_ready(driver) || !device_is_ready(ramp) || !spi_is_ready_dt(&tmc_spi) ||
        !gpio_is_ready_dt(&diag0) || !gpio_is_ready_dt(&diag1))
        return -ENODEV;
    int err = gpio_pin_configure_dt(&diag0, GPIO_INPUT);
    if (err) return err;
    err = gpio_pin_configure_dt(&diag1, GPIO_INPUT);
    if (err) return err;
    if (stepper_disable(driver)) return -EIO;
    /* IFCNT is UART-only. IOIN VERSION validates SPI, including stuck MISO. */
    if (tmc_read(0x04, &ioin)) return -EIO;
    LOG_INF("TMC5160 IOIN=0x%08x", ioin);
    if ((ioin >> 24) != 0x30u || (ioin & BIT(6))) return -EIO;
    /* Clear power-on reset and pre-existing fault bits before monitoring. */
    if (tmc_write(0x01, 0x07)) return -EIO;
    err = stepper_ctrl_configure_ramp(ramp, &normal_ramp);
    initialized = err == 0;
    if (!err) LOG_INF("TMC5160 SPI ready; version=%u, initial ramp 30 rpm/10 rpm per second",
                      ioin >> 24);
    return err;
}

int lifter_motor_run(enum lifter_motion direction, bool slow, bool quiet)
{
    int err;
    if (direction != LIFTER_UP && direction != LIFTER_DOWN) return -EINVAL;
    if (!initialized) return -ENODEV;
    if (running == direction && running_slow == slow && running_quiet == quiet) return 0;
    if (running != LIFTER_IDLE && running != direction) lifter_motor_stop();
    const struct stepper_ctrl_ramp *selected = quiet ?
        (slow ? &quiet_slow_ramp : &quiet_ramp) :
        (slow ? &slow_ramp : &normal_ramp);
    err = stepper_ctrl_configure_ramp(ramp, selected);
    if (err) return err;
    if (!enabled) {
        err = stepper_enable(driver);
        if (err) return err;
        enabled = true;
    }
    err = stepper_ctrl_run(ramp, direction == LIFTER_UP ?
                           STEPPER_CTRL_DIRECTION_POSITIVE :
                           STEPPER_CTRL_DIRECTION_NEGATIVE);
    if (err) {
        lifter_motor_stop();
        return err;
    }
    running = direction;
    running_slow = slow;
    running_quiet = quiet;
    LOG_INF("Motor run: direction=%d slow=%d quiet=%d", direction, slow, quiet);
    return 0;
}

void lifter_motor_stop(void)
{
    if (running != LIFTER_IDLE) LOG_INF("Motor stop");
    if (running != LIFTER_IDLE) (void)stepper_ctrl_stop(ramp);
    if (enabled) (void)stepper_disable(driver);
    enabled = false;
    running = LIFTER_IDLE;
    running_slow = false;
    running_quiet = false;
}

bool lifter_motor_fault(void)
{
    uint32_t gstat;
    uint32_t drv_status;
    bool spi_ok = initialized && tmc_read(0x01, &gstat) == 0 &&
                  tmc_read(0x6f, &drv_status) == 0;
    bool diag0_active = gpio_pin_get_dt(&diag0) > 0;
    bool diag1_active = gpio_pin_get_dt(&diag1) > 0;
    k_spinlock_key_t key = k_spin_lock(&diagnostics_lock);
    diagnostics.spi_ok = spi_ok;
    diagnostics.diag0 = diag0_active;
    diagnostics.diag1 = diag1_active;
    if (spi_ok) {
        diagnostics.gstat = gstat;
        diagnostics.drv_status = drv_status;
    }
    k_spin_unlock(&diagnostics_lock, key);
    if (!spi_ok) return true;
    /* DRV_STATUS bits 25–28: overtemperature, prewarning, and shorts. */
    uint32_t warnings = drv_status & (BIT(25) | BIT(26) | BIT(27) | BIT(28));
    if (warnings != previous_warnings) {
        LOG_WRN("TMC warnings changed: DRV_STATUS=0x%08x GSTAT=0x%08x", drv_status, gstat);
        previous_warnings = warnings;
    }
    return (gstat & 0x07u) != 0 || (drv_status & (BIT(25) | BIT(27) | BIT(28))) != 0;
}

struct lifter_motor_diagnostics lifter_motor_diagnostics(void)
{
    k_spinlock_key_t key = k_spin_lock(&diagnostics_lock);
    struct lifter_motor_diagnostics copy = diagnostics;
    k_spin_unlock(&diagnostics_lock, key);
    return copy;
}

int lifter_motor_position(int32_t *position)
{
    if (!initialized) return -ENODEV;
    return stepper_ctrl_get_actual_position(ramp, position);
}

int lifter_motor_counted_stop(void)
{
    if (!initialized) return -ENODEV;
    /* At 400 microsteps/s, maximum AMAX stops in well under one millisecond.
     * Keep the bridge enabled until VACTUAL is zero so no disabled ramp
     * steps get counted as movement. The next run restores its ramp. */
    int err = tmc_write(0x26, 65535);
    if (!err) err = stepper_ctrl_stop(ramp);
    bool stopped = false;
    for (int i = 0; !err && i < 20; ++i) {
        uint32_t velocity;
        err = tmc_read(0x22, &velocity);
        if (!err && (velocity & 0xffffffu) == 0) {
            stopped = true;
            break;
        }
        k_msleep(1);
    }
    int disable_err = stepper_disable(driver);
    enabled = false;
    running = LIFTER_IDLE;
    running_slow = false;
    running_quiet = false;
    return err ? err : (disable_err ? disable_err : (stopped ? 0 : -ETIMEDOUT));
}
