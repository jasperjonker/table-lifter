#include "rotation_control.h"
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(rotation_test, LOG_LEVEL_INF);

int main(void)
{
    LOG_WRN("ROTATION TEST: normally-open endstops, no LiDAR, RAM-only memories, no BLE");
    rotation_init();
    int err = lifter_led_init();
    if (!err) err = lifter_buttons_init();
    if (!err) err = lifter_pd_init();
    if (!err) err = lifter_motor_init();
    if (!err) err = lifter_motor_counted_stop();
    if (err) {
        LOG_ERR("Initialization failed: %d; no movement", err);
        return 0;
    }
    (void)boot_write_img_confirmed();
    LOG_INF("Release controls, then hold DOWN to the lower switch to reference");
    int64_t next_log = 0;
    while (true) {
        bool pd_ok = lifter_pd_contract_ok();
        uint8_t buttons = lifter_buttons_read();
        uint8_t limits = lifter_limits_read();
        rotation_tick(k_uptime_get_32(), buttons, limits, pd_ok);
        struct lifter_status state = lifter_control_status();
        lifter_led_update(&state);
        lifter_led_button_feedback(buttons != 0);
        if (k_uptime_get() >= next_log) {
            struct lifter_pd_diagnostics pd = lifter_pd_diagnostics();
            LOG_INF("steps=%d home=%d buttons=0x%x limits=0x%x motion=%d fault=%d",
                    rotation_position(), rotation_homed(), buttons, limits,
                    state.motion, state.fault);
            LOG_INF("bus=%d mV RDO=0x%08x pd=%d", pd.bus_mv, pd.rdo, pd_ok);
            next_log = k_uptime_get() + 2000;
        }
        k_msleep(20);
    }
}
