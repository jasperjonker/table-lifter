#include "table_lifter.h"

#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

LOG_MODULE_REGISTER(table_lifter, LOG_LEVEL_INF);

struct service_state {
    bool pd_ready;
    bool buttons_ready;
    bool range_ready;
    bool ble_ready;
};

struct status_log_state {
    enum lifter_motion previous_motion;
    enum lifter_fault previous_fault;
    bool previous_pd;
    bool previous_range;
    bool first_snapshot;
    int64_t next_snapshot;
};

/** @brief Initialize services in dependency order and record optional readiness.
 *  @param services Destination for services checked during each control cycle.
 *  @return True if every service initialized successfully.
 */
static bool initialize_services(struct service_state *services)
{
    int err;
    bool healthy = true;

    err = lifter_led_init();
    healthy &= err == 0;
    if (err) LOG_ERR("LED init: %d", err);

    err = lifter_buttons_init();
    services->buttons_ready = err == 0;
    healthy &= err == 0;
    if (err) LOG_ERR("Button init: %d", err);

    err = settings_subsys_init();
    healthy &= err == 0;
    if (err) LOG_ERR("Settings init: %d", err);

    err = lifter_control_init();
    healthy &= err == 0;
    if (err) LOG_ERR("Settings load: %d", err);

    err = lifter_pd_init();
    services->pd_ready = err == 0;
    healthy &= services->pd_ready;
    if (err) LOG_ERR("STUSB4500 init: %d", err);

    err = lifter_motor_init();
    healthy &= err == 0;
    if (err) LOG_ERR("TMC5160 init: %d", err);

    err = lifter_range_init();
    services->range_ready = err == 0;
    healthy &= services->range_ready;
    if (err) LOG_ERR("VL53L1X init: %d", err);

    err = lifter_ble_init();
    services->ble_ready = err == 0;
    healthy &= services->ble_ready;
    if (err) LOG_ERR("Bluetooth init/bond load: %d", err);

    return healthy;
}

/** @brief Confirm the booted image only when all services initialized.
 *  @param healthy True if service initialization completed without errors.
 */
static void confirm_image(bool healthy)
{
    if (!healthy) {
        LOG_ERR("Service initialization incomplete; MCUboot image unconfirmed");
        return;
    }

    int err = boot_write_img_confirmed();
    if (err) LOG_ERR("MCUboot confirm failed: %d", err);
    else LOG_INF("Services initialized; MCUboot image confirmed");
}

/** @brief Log state transitions and a periodic status snapshot.
 *  @param status Latest control status.
 *  @param log_state Previous logged values and next snapshot time.
 */
static void log_status(const struct lifter_status *status,
                       struct status_log_state *log_state)
{
    int64_t now = k_uptime_get();
    if (!log_state->first_snapshot &&
        status->motion == log_state->previous_motion &&
        status->fault == log_state->previous_fault &&
        status->pd_ok == log_state->previous_pd &&
        status->range_valid == log_state->previous_range &&
        now < log_state->next_snapshot) return;

    if (status->fault == LIFTER_OK) {
        LOG_INF("motion=%d pd=%d range=%d distance=%d mm height=%d mm target=%d mm",
                status->motion, status->pd_ok, status->range_valid,
                status->distance_mm, status->height_mm, status->target_mm);
    } else {
        LOG_WRN("fault=%d motion=%d pd=%d range=%d distance=%d mm",
                status->fault, status->motion, status->pd_ok,
                status->range_valid, status->distance_mm);
    }
    struct lifter_pd_diagnostics pd = lifter_pd_diagnostics();
    struct lifter_range_diagnostics range = lifter_range_diagnostics();
    struct lifter_motor_diagnostics motor = lifter_motor_diagnostics();
    LOG_INF("diagnostics: bus=%d mV RDO=0x%08x PD_ALERT=%d lidar_status=%d raw=%d mm age=%u ms TMC GSTAT=0x%08x DRV_STATUS=0x%08x DIAG0=%d DIAG1=%d spi=%d ota=%d quiet=%d",
            pd.bus_mv, pd.rdo, pd.alert, range.range_status, range.raw_mm, range.age_ms,
            motor.gstat, motor.drv_status, motor.diag0, motor.diag1, motor.spi_ok,
            status->maintenance, status->quiet);
    log_state->previous_motion = status->motion;
    log_state->previous_fault = status->fault;
    log_state->previous_pd = status->pd_ok;
    log_state->previous_range = status->range_valid;
    log_state->first_snapshot = false;
    log_state->next_snapshot = now + 5000;
}

/** @brief Initialize firmware services and run the table control loop.
 *  @return Never returns during normal operation.
 */
int main(void)
{
    struct service_state services = {0};
    bool healthy = initialize_services(&services);
    confirm_image(healthy);

    struct status_log_state log_state = {.first_snapshot = true};

    while (true) {
        int distance = 0;
        bool range_valid = services.range_ready && lifter_range_read(&distance) == 0;
        bool pd_ok = services.pd_ready && lifter_pd_contract_ok();
        uint8_t buttons = services.buttons_ready ? lifter_buttons_read() : 0;
        uint8_t limits = services.buttons_ready ? lifter_limits_read() :
                         LIFTER_LIMIT_LOW | LIFTER_LIMIT_HIGH;

        lifter_control_tick(k_uptime_get_32(), buttons, distance, range_valid, pd_ok, limits);
        struct lifter_status state = lifter_control_status();
        if (services.ble_ready) lifter_ble_update(&state);
        /* Bluetooth may change the pairing-window flag in the control status. */
        state = lifter_control_status();
        lifter_led_update(&state);
        log_status(&state, &log_state);
        k_msleep(50);
    }
}
