#include "rotation_control.h"
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(rotation_control, LOG_LEVEL_INF);

static struct lifter_status state;
static bool homed, automatic, released;
static int32_t origin, position, target, preset[2];
static uint8_t valid, previous, consumed;
static uint32_t pressed_at[2], move_started;

/** @brief Stop and discard the reference if ramp communication failed. */
static void stop(void)
{
    if (state.motion != LIFTER_IDLE && lifter_motor_counted_stop()) {
        homed = false;
        valid = 0;
        state.fault = LIFTER_FAULT_DRIVER;
        released = false;
    }
    state.motion = LIFTER_IDLE;
    automatic = false;
}

void rotation_init(void)
{
    memset(&state, 0, sizeof(state));
    homed = automatic = released = false;
    origin = position = target = 0;
    valid = previous = consumed = 0;
    memset(preset, 0, sizeof(preset));
    memset(pressed_at, 0, sizeof(pressed_at));
    move_started = 0;
}

struct lifter_status lifter_control_status(void) { return state; }
bool rotation_homed(void) { return homed; }
int32_t rotation_position(void) { return position; }

void rotation_tick(uint32_t now, uint8_t buttons, uint8_t limits, bool pd_ok)
{
    int32_t raw;
    state.pd_ok = pd_ok;
    if (!pd_ok || lifter_motor_fault() || lifter_motor_position(&raw)) {
        stop();
        homed = false;
        valid = 0;
        released = false;
        previous = buttons;
        consumed = BIT(LIFTER_BTN_MEM1) | BIT(LIFTER_BTN_MEM2);
        state.fault = pd_ok ? LIFTER_FAULT_DRIVER : LIFTER_FAULT_PD;
        return;
    }
    int64_t relative = (int64_t)raw - origin;
    if (relative < INT32_MIN || relative > INT32_MAX) {
        stop();
        homed = false;
        valid = 0;
        released = false;
        state.fault = LIFTER_FAULT_CONFIG;
        return;
    }
    position = (int32_t)relative;
    state.fault = homed ? LIFTER_OK : LIFTER_FAULT_CONFIG;
    if (!released) {
        previous = buttons;
        if (!buttons) { released = true; consumed = 0; }
        return;
    }

    /* A fresh press is needed after a timeout; never restart automatically. */
    if (state.motion != LIFTER_IDLE && now - move_started >= 120000u) {
        stop();
        homed = false;
        valid = 0;
        released = false;
        state.fault = LIFTER_FAULT_CONFIG;
        LOG_WRN("Motion timeout; release controls and home again");
        return;
    }

    const uint8_t memories = BIT(LIFTER_BTN_MEM1) | BIT(LIFTER_BTN_MEM2);
    bool both = (buttons & memories) == memories;
    if (both) { stop(); consumed |= memories; }
    for (int i = 0; i < 2; ++i) {
        uint8_t bit = BIT(LIFTER_BTN_MEM1 + i);
        if ((buttons & bit) && !(previous & bit)) {
            pressed_at[i] = now;
            if (!both) consumed &= ~bit;
            if (automatic) { stop(); consumed |= bit; }
        } else if ((buttons & bit) && !(consumed & bit) &&
                   now - pressed_at[i] >= 2000u) {
            if (homed && state.motion == LIFTER_IDLE) {
                preset[i] = position;
                valid |= BIT(i);
                LOG_INF("Memory %d stored: %d microsteps", i + 1, position);
            }
            consumed |= bit;
        } else if (!(buttons & bit) && (previous & bit) && !(consumed & bit) &&
                   homed && (valid & BIT(i))) {
            target = preset[i];
            automatic = true;
            LOG_INF("Memory %d target: %d microsteps", i + 1, target);
        }
    }
    previous = buttons;

    bool up = buttons & BIT(LIFTER_BTN_UP);
    bool down = buttons & BIT(LIFTER_BTN_DOWN);
    enum lifter_motion direction = LIFTER_IDLE;
    if (up || down) {
        automatic = false;
        if (up != down) direction = up ? LIFTER_UP : LIFTER_DOWN;
    } else if (automatic && homed) {
        int64_t error = (int64_t)target - position;
        if (error > 16) direction = LIFTER_UP;
        else if (error < -16) direction = LIFTER_DOWN;
    }

    if (direction == LIFTER_DOWN && (limits & LIFTER_LIMIT_LOW)) {
        bool was_homed = homed;
        stop();
        if (state.fault == LIFTER_FAULT_DRIVER) return;
        /* Only the held DOWN control establishes a reference, never an
         * automatic recall or an uncommanded switch activation. */
        if (down && !up && !lifter_motor_position(&raw)) {
            origin = raw;
            position = 0;
            homed = true;
            if (!was_homed) valid = 0;
            state.fault = LIFTER_OK;
            if (!was_homed) LOG_INF("Lower reference established; memories available");
        }
        return;
    }
    if (direction == LIFTER_IDLE ||
        (direction == LIFTER_UP && (limits & LIFTER_LIMIT_HIGH))) {
        stop();
        return;
    }
    if (state.motion != LIFTER_IDLE && state.motion != direction) {
        bool resume_auto = automatic;
        stop();
        automatic = resume_auto;
        if (state.fault == LIFTER_FAULT_DRIVER) return;
    }
    if (state.motion == LIFTER_IDLE) move_started = now;
    if (lifter_motor_run(direction, true, false)) {
        stop();
        homed = false;
        valid = 0;
        released = false;
        state.fault = LIFTER_FAULT_DRIVER;
        return;
    }
    state.motion = direction;
}
