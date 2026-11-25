#include "rotation_control.h"
#include <zephyr/ztest.h>
#include <errno.h>

static int32_t raw;
static enum lifter_motion direction;
static bool fault;
static int read_error, stop_error;
static unsigned int starts;

int lifter_motor_position(int32_t *value) { *value = raw; return read_error; }
bool lifter_motor_fault(void) { return fault; }
int lifter_motor_counted_stop(void) { direction = LIFTER_IDLE; return stop_error; }
int lifter_motor_run(enum lifter_motion dir, bool slow, bool quiet)
{
    zassert_true(slow);
    zassert_false(quiet);
    direction = dir;
    ++starts;
    return 0;
}

static void reset(void)
{
    raw = 10000;
    direction = LIFTER_IDLE;
    fault = false;
    read_error = stop_error = starts = 0;
    rotation_init();
    rotation_tick(100, 0, 0, true);
}

static void home(void)
{
    rotation_tick(200, BIT(LIFTER_BTN_DOWN), LIFTER_LIMIT_LOW, true);
    zassert_true(rotation_homed());
    zassert_equal(rotation_position(), 0);
    rotation_tick(300, 0, 0, true);
}

static void store_first(void)
{
    raw += 3200;
    rotation_tick(1000, BIT(LIFTER_BTN_MEM1), 0, true);
    rotation_tick(3001, BIT(LIFTER_BTN_MEM1), 0, true);
    rotation_tick(3100, 0, 0, true);
}

ZTEST(rotation, test_manual_and_no_limits)
{
    reset();
    rotation_tick(200, BIT(LIFTER_BTN_UP), 0, true);
    zassert_equal(direction, LIFTER_UP);
    rotation_tick(300, BIT(LIFTER_BTN_UP), LIFTER_LIMIT_HIGH, true);
    zassert_equal(direction, LIFTER_IDLE);
    rotation_tick(400, BIT(LIFTER_BTN_DOWN), LIFTER_LIMIT_HIGH, true);
    zassert_equal(direction, LIFTER_DOWN);
    rotation_tick(500, BIT(LIFTER_BTN_DOWN), LIFTER_LIMIT_LOW, true);
    zassert_equal(direction, LIFTER_IDLE);
    zassert_true(rotation_homed());
    rotation_tick(600, BIT(LIFTER_BTN_UP) | BIT(LIFTER_BTN_DOWN), 0, true);
    zassert_equal(direction, LIFTER_IDLE);
}

ZTEST(rotation, test_memories_require_reference_and_do_not_move_at_startup)
{
    reset();
    store_first();
    raw -= 3200;
    rotation_tick(3200, BIT(LIFTER_BTN_MEM1), 0, true);
    rotation_tick(3300, 0, 0, true);
    zassert_equal(direction, LIFTER_IDLE);
    zassert_false(rotation_homed());
    zassert_equal(starts, 0);
}

ZTEST(rotation, test_store_recall_and_arrival_in_steps)
{
    reset(); home(); store_first();
    raw -= 3200;
    rotation_tick(3200, BIT(LIFTER_BTN_MEM1), 0, true);
    rotation_tick(3300, 0, 0, true);
    zassert_equal(direction, LIFTER_UP);
    raw += 3190;
    rotation_tick(3400, 0, 0, true);
    zassert_equal(direction, LIFTER_IDLE);
    zassert_equal(rotation_position(), 3190);
}

ZTEST(rotation, test_memory_press_cancels_without_restart)
{
    reset(); home(); store_first();
    raw -= 3200;
    rotation_tick(3200, BIT(LIFTER_BTN_MEM1), 0, true);
    rotation_tick(3300, 0, 0, true);
    rotation_tick(3400, BIT(LIFTER_BTN_MEM2), 0, true);
    zassert_equal(direction, LIFTER_IDLE);
    rotation_tick(3500, 0, 0, true);
    zassert_equal(direction, LIFTER_IDLE);
}

ZTEST(rotation, test_preset_endstop_cancels)
{
    reset(); home(); store_first();
    raw -= 3200;
    rotation_tick(3200, BIT(LIFTER_BTN_MEM1), 0, true);
    rotation_tick(3300, 0, LIFTER_LIMIT_HIGH, true);
    zassert_equal(direction, LIFTER_IDLE);
    rotation_tick(3400, 0, 0, true);
    zassert_equal(direction, LIFTER_IDLE);
}

ZTEST(rotation, test_pd_loss_invalidates_reference_and_requires_release)
{
    reset(); home();
    rotation_tick(400, BIT(LIFTER_BTN_UP), 0, true);
    rotation_tick(500, BIT(LIFTER_BTN_UP), 0, false);
    zassert_equal(direction, LIFTER_IDLE);
    zassert_false(rotation_homed());
    rotation_tick(600, BIT(LIFTER_BTN_UP), 0, true);
    zassert_equal(direction, LIFTER_IDLE);
    rotation_tick(700, 0, 0, true);
    rotation_tick(800, BIT(LIFTER_BTN_UP), 0, true);
    zassert_equal(direction, LIFTER_UP);
}

ZTEST(rotation, test_spi_and_stop_errors_discard_reference)
{
    reset(); home();
    rotation_tick(400, BIT(LIFTER_BTN_UP), 0, true);
    read_error = -EIO;
    rotation_tick(500, 0, 0, true);
    zassert_false(rotation_homed());
    zassert_equal(direction, LIFTER_IDLE);
    reset(); home();
    rotation_tick(400, BIT(LIFTER_BTN_UP), 0, true);
    stop_error = -EIO;
    rotation_tick(500, 0, 0, true);
    zassert_false(rotation_homed());
    zassert_equal(lifter_control_status().fault, LIFTER_FAULT_DRIVER);
}

ZTEST(rotation, test_timeout_and_restart_clear_memories)
{
    reset(); home();
    rotation_tick(400, BIT(LIFTER_BTN_UP), 0, true);
    rotation_tick(120401, BIT(LIFTER_BTN_UP), 0, true);
    zassert_equal(direction, LIFTER_IDLE);
    zassert_false(rotation_homed());
    reset(); home(); store_first();
    rotation_init();
    rotation_tick(4000, 0, 0, true);
    home();
    raw -= 3200;
    rotation_tick(4100, BIT(LIFTER_BTN_MEM1), 0, true);
    rotation_tick(4200, 0, 0, true);
    zassert_equal(direction, LIFTER_IDLE);
}

ZTEST(rotation, test_second_memory_and_boot_held_button)
{
    reset();
    rotation_init();
    rotation_tick(100, BIT(LIFTER_BTN_UP), 0, true);
    zassert_equal(direction, LIFTER_IDLE);
    rotation_tick(200, 0, 0, true);
    home();
    raw += 6400;
    rotation_tick(1000, BIT(LIFTER_BTN_MEM2), 0, true);
    rotation_tick(3001, BIT(LIFTER_BTN_MEM2), 0, true);
    rotation_tick(3100, 0, 0, true);
    raw += 3200;
    rotation_tick(3200, BIT(LIFTER_BTN_MEM2), 0, true);
    rotation_tick(3300, 0, 0, true);
    zassert_equal(direction, LIFTER_DOWN);
    rotation_tick(3400, BIT(LIFTER_BTN_UP), 0, true);
    zassert_equal(direction, LIFTER_UP);
    rotation_tick(3500, 0, 0, true);
    zassert_equal(direction, LIFTER_IDLE);
}

ZTEST_SUITE(rotation, NULL, NULL, NULL, NULL, NULL);
