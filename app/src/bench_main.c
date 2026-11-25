#include "table_lifter.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(bench_test, LOG_LEVEL_INF);

static const struct gpio_dt_spec enable =
    GPIO_DT_SPEC_GET(DT_NODELABEL(table_io), driver_enable_gpios);
static const struct i2c_dt_spec pd = I2C_DT_SPEC_GET(DT_NODELABEL(stusb4500));

/** @brief Deassert the bridge enable independently of the main test loop. */
static void disable_bridge(struct k_timer *timer)
{
    ARG_UNUSED(timer);
    (void)gpio_pin_set_dt(&enable, 0);
}

K_TIMER_DEFINE(stop_timer, disable_bridge, NULL);

/** @brief Run one unloaded, timed test per boot; never enter normal table control. */
int main(void)
{
    if (!gpio_is_ready_dt(&enable)) return 0;
    if (gpio_pin_configure_dt(&enable, GPIO_OUTPUT_INACTIVE)) return 0;
    LOG_WRN("BENCH TEST: unloaded motor only; automatic test after 5 seconds");
    k_sleep(K_SECONDS(5));

    uint8_t bytes[4];
    if (!i2c_is_ready_dt(&pd) || i2c_burst_read_dt(&pd, 0x91, bytes, 4)) {
        LOG_ERR("No PD communication; motor stays disabled");
        return 0;
    }
    uint32_t rdo = sys_get_le32(bytes);
    LOG_INF("PD RDO=0x%08x", rdo);
    if (((rdo >> 28) & 7u) == 0 || ((rdo >> 10) & 0x3ffu) < 300u ||
        (rdo & 0x3ffu) < 300u || (rdo & BIT(26))) {
        LOG_ERR("3 A operating contract not present; power cycle after provisioning");
        return 0;
    }
    int err = lifter_motor_init();
    if (err || lifter_motor_fault()) {
        (void)gpio_pin_set_dt(&enable, 0);
        LOG_ERR("Motor init/fault check failed: %d; no movement", err);
        return 0;
    }

    LOG_WRN("Starting 3-second test at maximum 7.5 rpm");
    k_timer_start(&stop_timer, K_SECONDS(3), K_NO_WAIT);
    err = lifter_motor_run(LIFTER_UP, true, false);
    int64_t end = k_uptime_get() + 3000;
    while (!err && k_uptime_get() < end && k_timer_remaining_get(&stop_timer) > 0) {
        if (lifter_motor_fault()) {
            LOG_ERR("Motor fault; stopping");
            break;
        }
        k_msleep(20);
    }
    (void)gpio_pin_set_dt(&enable, 0);
    lifter_motor_stop();
    k_timer_stop(&stop_timer);
    LOG_INF("Test finished; bridge disabled; run result=%d", err);
    return 0;
}
