#include "table_lifter.h"

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/ztest.h>

#include <errno.h>

static enum lifter_motion motor_direction;
static bool motor_slow;
static bool motor_quiet;
static unsigned int stops;

/** @brief Feed one synthetic sensor and button sample to the controller.
 *  @param ms Synthetic uptime in milliseconds.
 *  @param buttons Pressed-button bitmask.
 *  @param distance Floor distance in millimetres.
 *  @param valid Whether the range sample is valid.
 *  @param pd Whether the PD contract is valid.
 */
static void tick(uint32_t ms, uint8_t buttons, int distance, bool valid, bool pd)
{
    lifter_control_tick(ms, buttons, distance, valid, pd, 0);
}

/** @brief Reset controller and fake motor state before a test. */
static void reset_control(void)
{
    (void)lifter_control_init();
    motor_direction = LIFTER_IDLE;
    motor_slow = false;
    motor_quiet = false;
    stops = 0;
    tick(100, 0, 1000, true, true);
}

/** @brief Verify jog follows held buttons and stops on conflict or release. */
ZTEST(table_control, test_manual_jog_and_stop)
{
    reset_control();
    tick(200, BIT(LIFTER_BTN_UP), 1000, true, true);
    zassert_equal(motor_direction, LIFTER_UP);
    tick(300, BIT(LIFTER_BTN_UP) | BIT(LIFTER_BTN_DOWN), 1000, true, true);
    zassert_equal(motor_direction, LIFTER_IDLE);
    tick(400, BIT(LIFTER_BTN_DOWN), 1000, true, true);
    zassert_equal(motor_direction, LIFTER_DOWN);
    tick(500, 0, 1000, true, true);
    zassert_equal(motor_direction, LIFTER_IDLE);
}

/** @brief Verify memory hold, recall and tap-to-interrupt behavior. */
ZTEST(table_control, test_memory_hold_recall_and_interrupt)
{
    reset_control();
    tick(1000, BIT(LIFTER_BTN_MEM1), 1200, true, true);
    tick(3100, BIT(LIFTER_BTN_MEM1), 1200, true, true);
    zassert_true(lifter_control_config().valid_bits & LIFTER_VALID_PRESET1);
    tick(3200, 0, 1200, true, true);
    zassert_equal(lifter_control_status().motion, LIFTER_IDLE);
    tick(3300, BIT(LIFTER_BTN_MEM1), 1000, true, true);
    tick(3400, 0, 1000, true, true);
    tick(3500, 0, 1000, true, true);
    zassert_equal(motor_direction, LIFTER_UP);
    tick(3600, BIT(LIFTER_BTN_MEM1), 1050, true, true);
    zassert_equal(motor_direction, LIFTER_IDLE);
    tick(3700, 0, 1050, true, true);
    zassert_equal(lifter_control_status().motion, LIFTER_IDLE);
}

/** @brief Verify holding a stop button stores without recalling. */
ZTEST(table_control, test_memory_hold_after_interrupt_stores_position)
{
    reset_control();
    (void)lifter_control_store(0);
    tick(1000, BIT(LIFTER_BTN_MEM1), 1200, true, true);
    tick(1100, 0, 1200, true, true);
    tick(1200, 0, 1200, true, true);
    zassert_equal(motor_direction, LIFTER_DOWN);
    tick(1300, BIT(LIFTER_BTN_MEM1), 1150, true, true);
    zassert_equal(motor_direction, LIFTER_IDLE);
    tick(3400, BIT(LIFTER_BTN_MEM1), 1150, true, true);
    zassert_equal(lifter_control_config().preset_mm[0], 1150);
    tick(3500, 0, 1150, true, true);
    zassert_equal(lifter_control_status().motion, LIFTER_IDLE);
}

/** @brief Verify calibrated target moves slow and stop near the target. */
ZTEST(table_control, test_calibration_arrival_and_slowdown)
{
    reset_control();
    (void)lifter_control_offset(100);
    zassert_ok(lifter_control_target(1400));
    tick(1000, 0, 1000, true, true);
    zassert_equal(motor_direction, LIFTER_UP);
    zassert_false(motor_slow);
    tick(1100, 0, 1250, true, true);
    zassert_true(motor_slow);
    tick(1200, 0, 1285, true, true);
    zassert_equal(motor_direction, LIFTER_IDLE);
}

/** @brief Verify invalid range and lost PD contract stop motion. */
ZTEST(table_control, test_invalid_range_or_contract_stops)
{
    reset_control();
    tick(200, BIT(LIFTER_BTN_UP), 1000, true, true);
    tick(300, BIT(LIFTER_BTN_UP), 0, false, true);
    zassert_equal(motor_direction, LIFTER_IDLE);
    zassert_equal(lifter_control_status().fault, LIFTER_FAULT_RANGE);
    tick(400, BIT(LIFTER_BTN_UP), 1000, true, true);
    tick(500, BIT(LIFTER_BTN_UP), 1000, true, false);
    zassert_equal(motor_direction, LIFTER_IDLE);
    zassert_equal(lifter_control_status().fault, LIFTER_FAULT_PD);
}

/** @brief Verify both memory buttons open pairing after five seconds. */
ZTEST(table_control, test_pairing_gesture)
{
    reset_control();
    uint8_t both = BIT(LIFTER_BTN_MEM1) | BIT(LIFTER_BTN_MEM2);
    tick(1000, both, 1000, true, true);
    tick(5900, both, 1000, true, true);
    zassert_false(lifter_control_pairing_request());
    tick(6000, both, 1000, true, true);
    zassert_true(lifter_control_pairing_request());
    zassert_false(lifter_control_pairing_request());
}

/** @brief Verify persisted settings reject invalid versions and bounds. */
ZTEST(table_control, test_settings_record_validation)
{
    reset_control();
    struct {
        uint32_t version;
        struct lifter_config data;
    } record = {
        .version = 1,
        .data = {.offset_mm = 125, .valid_bits = LIFTER_VALID_OFFSET},
    };
    zassert_ok(settings_runtime_set("lifter/config", &record, sizeof(record)));
    zassert_equal(lifter_control_config().offset_mm, 125);
    record.version = 2;
    record.data.offset_mm = 999;
    zassert_equal(settings_runtime_set("lifter/config", &record, sizeof(record)), -EINVAL);
    zassert_equal(lifter_control_config().offset_mm, 125);
    record.version = 1;
    record.data.valid_bits |= LIFTER_VALID_BOUNDS;
    record.data.min_height_mm = 2000;
    record.data.max_height_mm = 1000;
    zassert_equal(settings_runtime_set("lifter/config", &record, sizeof(record)), -EINVAL);
    zassert_equal(lifter_control_config().offset_mm, 125);
    record.data.valid_bits = LIFTER_VALID_OFFSET | LIFTER_VALID_PRESET1;
    record.data.min_height_mm = 0;
    record.data.max_height_mm = 0;
    record.data.preset_mm[0] = 9000;
    zassert_equal(settings_runtime_set("lifter/config", &record, sizeof(record)), -EINVAL);
    zassert_equal(lifter_control_config().offset_mm, 125);
}

/** @brief Verify measured bounds constrain jog and target requests. */
ZTEST(table_control, test_measured_bounds_limit_jog_and_target)
{
    reset_control();
    (void)lifter_control_offset(100);
    (void)lifter_control_bounds(1100, 1500);
    tick(1000, BIT(LIFTER_BTN_UP), 1400, true, true);
    zassert_equal(motor_direction, LIFTER_IDLE);
    tick(1100, BIT(LIFTER_BTN_DOWN), 1400, true, true);
    zassert_equal(motor_direction, LIFTER_DOWN);
    tick(1200, BIT(LIFTER_BTN_DOWN), 1000, true, true);
    zassert_equal(motor_direction, LIFTER_IDLE);
    zassert_equal(lifter_control_target(1600), -EPERM);
}

/** @brief Verify OTA maintenance blocks buttons and remote moves until cancelled. */
ZTEST(table_control, test_ota_maintenance_latches_motion_lock)
{
    reset_control();
    (void)lifter_control_store(0);
    zassert_ok(lifter_control_enter_maintenance());
    tick(200, BIT(LIFTER_BTN_UP), 1000, true, true);
    zassert_equal(motor_direction, LIFTER_IDLE);
    zassert_equal(lifter_control_preset(0), -EPERM);
    zassert_true(lifter_control_status().maintenance);
    tick(300, BIT(LIFTER_BTN_MEM1), 900, true, true);
    tick(400, 0, 900, true, true);
    zassert_equal(motor_direction, LIFTER_IDLE);
    lifter_control_cancel_maintenance();
    tick(500, BIT(LIFTER_BTN_UP), 900, true, true);
    zassert_equal(motor_direction, LIFTER_UP);
    zassert_equal(lifter_control_enter_maintenance(), -EBUSY);
}

/** @brief Verify quiet settings reach the motor and recent faults are recorded. */
ZTEST(table_control, test_quiet_profile_and_fault_history)
{
    reset_control();
    uint8_t quiet = 1;
    zassert_ok(settings_runtime_set("lifter/quiet", &quiet, sizeof(quiet)));
    tick(200, BIT(LIFTER_BTN_UP), 1000, true, true);
    zassert_true(motor_quiet);
    zassert_true(lifter_control_status().quiet);
    zassert_equal(lifter_control_set_quiet(false), -EBUSY);
    tick(300, BIT(LIFTER_BTN_UP), 1000, false, true);
    struct lifter_fault_event events[4] = {0};
    zassert_true(lifter_control_fault_history(events) >= 1);
    zassert_equal(events[0].fault, LIFTER_FAULT_RANGE);
}

int lifter_motor_run(enum lifter_motion direction, bool slow, bool quiet)
{
    motor_direction = direction;
    motor_slow = slow;
    motor_quiet = quiet;
    return 0;
}

void lifter_motor_stop(void)
{
    motor_direction = LIFTER_IDLE;
    ++stops;
}

bool lifter_motor_fault(void) { return false; }

/** @brief Open NC limits stop their direction and permit retreat. */
ZTEST(table_control, test_limits_stop_manual_and_preset)
{
    reset_control();
    lifter_control_tick(200, BIT(LIFTER_BTN_UP), 1000, true, true, LIFTER_LIMIT_HIGH);
    zassert_equal(motor_direction, LIFTER_IDLE);
    lifter_control_tick(300, BIT(LIFTER_BTN_DOWN), 1000, true, true, LIFTER_LIMIT_HIGH);
    zassert_equal(motor_direction, LIFTER_DOWN);
    lifter_control_tick(400, BIT(LIFTER_BTN_DOWN), 1000, true, true, LIFTER_LIMIT_LOW);
    zassert_equal(motor_direction, LIFTER_IDLE);
    lifter_control_tick(500, BIT(LIFTER_BTN_UP), 1000, true, true, LIFTER_LIMIT_LOW);
    zassert_equal(motor_direction, LIFTER_UP);
    tick(600, 0, 1200, true, true);
    (void)lifter_control_store(0);
    tick(700, 0, 1000, true, true);
    zassert_ok(lifter_control_preset(0));
    lifter_control_tick(800, 0, 1000, true, true, LIFTER_LIMIT_HIGH);
    zassert_equal(motor_direction, LIFTER_IDLE);
    tick(900, 0, 1000, true, true);
    zassert_equal(motor_direction, LIFTER_IDLE);
}

ZTEST_SUITE(table_control, NULL, NULL, NULL, NULL, NULL);
