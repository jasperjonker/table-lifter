#include "table_lifter.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(lifter_control, LOG_LEVEL_INF);

#define CONFIG_VERSION 1u
#define MEMORY_HOLD_MS 2000u
#define PAIR_HOLD_MS 5000u
#define TARGET_BAND_MM 20
#define SLOW_BAND_MM 100

static K_MUTEX_DEFINE(lock);
static struct lifter_config config;
static struct lifter_status status;
static int32_t target_distance_mm;
static bool automatic;
static bool pairing_requested;
static uint32_t memory_down_at[2];
static bool memory_was_down[2];
static bool memory_consumed[2];
static bool memory_stopped_move[2];
static uint32_t pair_down_at;
static bool pair_handled;
static bool quiet_profile;
static struct lifter_fault_event fault_events[4];
static unsigned int fault_next;
static unsigned int fault_count;
static enum lifter_fault last_recorded_fault;

struct stored_config {
    uint32_t version;
    struct lifter_config data;
};

/** @brief Validate and load the persisted controller configuration.
 *  @param name Settings key below the lifter subtree.
 *  @param len Stored record size in bytes.
 *  @param read_cb Zephyr settings read callback.
 *  @param cb_arg Callback context.
 *  @return 0 on success, or a negative errno value.
 */
static int config_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
    struct stored_config value;
    if (strcmp(name, "quiet") == 0) {
        uint8_t stored;
        if (len != sizeof(stored) || read_cb(cb_arg, &stored, sizeof(stored)) != sizeof(stored) ||
            stored > 1) return -EINVAL;
        quiet_profile = stored != 0;
        return 0;
    }
    if (strcmp(name, "config") != 0) return -ENOENT;
    if (len != sizeof(value)) return -EINVAL;
    int got = read_cb(cb_arg, &value, sizeof(value));
    if (got != sizeof(value) || value.version != CONFIG_VERSION) return -EINVAL;
    if (value.data.valid_bits & ~(LIFTER_VALID_PRESET1 | LIFTER_VALID_PRESET2 |
                                  LIFTER_VALID_BOUNDS | LIFTER_VALID_OFFSET)) return -EINVAL;
    if ((value.data.valid_bits & LIFTER_VALID_OFFSET) &&
        (value.data.offset_mm < -1000 || value.data.offset_mm > 1000)) return -EINVAL;
    for (unsigned int i = 0; i < 2; ++i) {
        if ((value.data.valid_bits & BIT(i)) &&
            (value.data.preset_mm[i] < 40 || value.data.preset_mm[i] > 4000)) return -EINVAL;
    }
    if ((value.data.valid_bits & LIFTER_VALID_BOUNDS) &&
        (value.data.min_height_mm >= value.data.max_height_mm ||
         value.data.min_height_mm < 0 || value.data.max_height_mm > 5000)) return -EINVAL;
    config = value.data;
    LOG_INF("Controller settings loaded: valid_bits=0x%x", config.valid_bits);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(table_lifter, "lifter", NULL, config_set, NULL, NULL);

/** @brief Persist the current settings record.
 *  @return 0 on success, or a negative errno value.
 */
static int save_config(void)
{
    struct stored_config value = {.version = CONFIG_VERSION, .data = config};
    return settings_save_one("lifter/config", &value, sizeof(value));
}

/** @brief Record a newly observed fault without writing flash during motion.
 *  @param now_ms Monotonic uptime in milliseconds.
 */
static void record_fault(uint32_t now_ms)
{
    if (status.fault == last_recorded_fault) return;
    last_recorded_fault = status.fault;
    if (status.fault == LIFTER_OK) return;
    fault_events[fault_next] = (struct lifter_fault_event){
        .uptime_s = now_ms / 1000u, .fault = status.fault,
    };
    fault_next = (fault_next + 1u) % ARRAY_SIZE(fault_events);
    if (fault_count < ARRAY_SIZE(fault_events)) ++fault_count;
    LOG_WRN("Fault %d at %u s", status.fault, now_ms / 1000u);
}

/** @brief Check a height against configured bounds, if any.
 *  @param height Tabletop height in millimetres.
 *  @return True if the height is within the configured range.
 */
static bool inside_bounds(int32_t height)
{
    return !(config.valid_bits & LIFTER_VALID_BOUNDS) ||
           (height >= config.min_height_mm && height <= config.max_height_mm);
}

/** @brief Stop motion while the controller mutex is held. */
static void stop_locked(void)
{
    lifter_motor_stop();
    automatic = false;
    status.motion = LIFTER_IDLE;
}

/** @brief Handle preset taps, holds, interruptions and the pairing gesture.
 *  @param now_ms Monotonic uptime in milliseconds.
 *  @param buttons Bitmask of pressed buttons.
 */
static void process_memory(uint32_t now_ms, uint8_t buttons)
{
    bool both = (buttons & (BIT(LIFTER_BTN_MEM1) | BIT(LIFTER_BTN_MEM2))) ==
                (BIT(LIFTER_BTN_MEM1) | BIT(LIFTER_BTN_MEM2));
    if (both) {
        if (!pair_down_at) pair_down_at = now_ms;
        if (!pair_handled && now_ms - pair_down_at >= PAIR_HOLD_MS) {
            pairing_requested = true;
            pair_handled = true;
            LOG_INF("Pairing gesture accepted");
        }
        memory_consumed[0] = memory_consumed[1] = true;
    } else {
        pair_down_at = 0;
        pair_handled = false;
    }

    for (unsigned int i = 0; i < 2; ++i) {
        bool down = (buttons & BIT(LIFTER_BTN_MEM1 + i)) != 0;
        if (down && !memory_was_down[i]) {
            memory_down_at[i] = now_ms;
            memory_consumed[i] = both;
            memory_stopped_move[i] = automatic;
            if (automatic) {
                LOG_INF("Preset %u stopped automatic move", i + 1);
                stop_locked();
            }
        } else if (down && !both && !memory_consumed[i] &&
                   now_ms - memory_down_at[i] >= MEMORY_HOLD_MS) {
            if (!status.maintenance && status.range_valid) {
                config.preset_mm[i] = status.distance_mm;
                config.valid_bits |= BIT(i);
                int err = save_config();
                if (err) LOG_ERR("Preset %u save failed: %d", i + 1, err);
                else LOG_INF("Preset %u stored at %d mm", i + 1, status.distance_mm);
            }
            memory_consumed[i] = true;
        } else if (!down && memory_was_down[i]) {
            if (!status.maintenance && !memory_consumed[i] && !memory_stopped_move[i] &&
                (config.valid_bits & BIT(i)) && status.range_valid && status.pd_ok) {
                int32_t height = config.preset_mm[i] + config.offset_mm;
                if (inside_bounds(height)) {
                    target_distance_mm = config.preset_mm[i];
                    status.target_mm = height;
                    automatic = true;
                    status.motion = LIFTER_AUTO;
                    LOG_INF("Preset %u recalled: %d mm", i + 1, height);
                }
            }
            memory_stopped_move[i] = false;
        }
        memory_was_down[i] = down;
    }
}

int lifter_control_init(void)
{
    k_mutex_lock(&lock, K_FOREVER);
    memset(&config, 0, sizeof(config));
    memset(&status, 0, sizeof(status));
    memset(memory_down_at, 0, sizeof(memory_down_at));
    memset(memory_was_down, 0, sizeof(memory_was_down));
    memset(memory_consumed, 0, sizeof(memory_consumed));
    memset(memory_stopped_move, 0, sizeof(memory_stopped_move));
    automatic = false;
    pairing_requested = false;
    pair_down_at = 0;
    pair_handled = false;
    quiet_profile = false;
    memset(fault_events, 0, sizeof(fault_events));
    fault_next = fault_count = 0;
    last_recorded_fault = LIFTER_OK;
    status.motion = LIFTER_IDLE;
    status.fault = LIFTER_FAULT_PD;
    k_mutex_unlock(&lock);
    int err = settings_load_subtree("lifter");
    if (err) LOG_ERR("Controller settings load failed: %d", err);
    return err;
}

void lifter_control_stop(void)
{
    k_mutex_lock(&lock, K_FOREVER);
    stop_locked();
    k_mutex_unlock(&lock);
}

int lifter_control_target(int32_t height_mm)
{
    int err = 0;
    k_mutex_lock(&lock, K_FOREVER);
    if (status.maintenance || !(config.valid_bits & LIFTER_VALID_OFFSET) || !status.range_valid ||
        !status.pd_ok || !inside_bounds(height_mm)) err = -EPERM;
    else {
        target_distance_mm = height_mm - config.offset_mm;
        if (target_distance_mm < 40 || target_distance_mm > 4000) err = -ERANGE;
        else {
            automatic = true;
            status.target_mm = height_mm;
            status.motion = LIFTER_AUTO;
            LOG_INF("Automatic target accepted: %d mm", height_mm);
        }
    }
    k_mutex_unlock(&lock);
    return err;
}

int lifter_control_preset(unsigned int index)
{
    int err = 0;
    if (index >= 2) return -EINVAL;
    k_mutex_lock(&lock, K_FOREVER);
    if (status.maintenance || !(config.valid_bits & BIT(index)) || !status.range_valid || !status.pd_ok)
        err = -EPERM;
    else {
        int32_t height = config.preset_mm[index] + config.offset_mm;
        if (!inside_bounds(height)) err = -ERANGE;
        else {
            target_distance_mm = config.preset_mm[index];
            status.target_mm = height;
            automatic = true;
            status.motion = LIFTER_AUTO;
            LOG_INF("Preset %u requested: %d mm", index + 1, height);
        }
    }
    k_mutex_unlock(&lock);
    return err;
}

int lifter_control_store(unsigned int index)
{
    if (index >= 2) return -EINVAL;
    k_mutex_lock(&lock, K_FOREVER);
    if (!status.range_valid) {
        k_mutex_unlock(&lock);
        return -ENODATA;
    }
    config.preset_mm[index] = status.distance_mm;
    config.valid_bits |= BIT(index);
    int err = save_config();
    if (err) LOG_ERR("Preset %u save failed: %d", index + 1, err);
    else LOG_INF("Preset %u stored at %d mm", index + 1, status.distance_mm);
    k_mutex_unlock(&lock);
    return err;
}

int lifter_control_offset(int32_t offset_mm)
{
    if (offset_mm < -1000 || offset_mm > 1000) return -ERANGE;
    k_mutex_lock(&lock, K_FOREVER);
    if (status.motion != LIFTER_IDLE) {
        k_mutex_unlock(&lock);
        return -EBUSY;
    }
    config.offset_mm = offset_mm;
    config.valid_bits |= LIFTER_VALID_OFFSET;
    int err = save_config();
    if (err) LOG_ERR("Calibration offset save failed: %d", err);
    else LOG_INF("Calibration offset set: %d mm", offset_mm);
    k_mutex_unlock(&lock);
    return err;
}

int lifter_control_bounds(int32_t min_mm, int32_t max_mm)
{
    if (min_mm < 0 || max_mm > 5000 || min_mm >= max_mm) return -ERANGE;
    k_mutex_lock(&lock, K_FOREVER);
    if (status.motion != LIFTER_IDLE || !(config.valid_bits & LIFTER_VALID_OFFSET)) {
        k_mutex_unlock(&lock);
        return -EPERM;
    }
    config.min_height_mm = min_mm;
    config.max_height_mm = max_mm;
    config.valid_bits |= LIFTER_VALID_BOUNDS;
    int err = save_config();
    if (err) LOG_ERR("Height bounds save failed: %d", err);
    else LOG_INF("Height bounds set: %d..%d mm", min_mm, max_mm);
    k_mutex_unlock(&lock);
    return err;
}

int lifter_control_set_quiet(bool quiet)
{
    k_mutex_lock(&lock, K_FOREVER);
    if (status.motion != LIFTER_IDLE || status.maintenance) {
        k_mutex_unlock(&lock);
        return -EBUSY;
    }
    uint8_t stored = quiet ? 1u : 0u;
    int err = settings_save_one("lifter/quiet", &stored, sizeof(stored));
    if (!err) {
        quiet_profile = quiet;
        status.quiet = quiet;
        LOG_INF("Quiet profile %s", quiet ? "enabled" : "disabled");
    }
    k_mutex_unlock(&lock);
    return err;
}

int lifter_control_enter_maintenance(void)
{
    k_mutex_lock(&lock, K_FOREVER);
    int err = status.motion == LIFTER_IDLE ? 0 : -EBUSY;
    if (!err && !status.maintenance) {
        status.maintenance = true;
        stop_locked();
        LOG_INF("OTA maintenance entered");
    }
    k_mutex_unlock(&lock);
    return err;
}

void lifter_control_cancel_maintenance(void)
{
    k_mutex_lock(&lock, K_FOREVER);
    status.maintenance = false;
    LOG_INF("OTA maintenance cancelled");
    k_mutex_unlock(&lock);
}

unsigned int lifter_control_fault_history(struct lifter_fault_event events[4])
{
    k_mutex_lock(&lock, K_FOREVER);
    unsigned int count = fault_count;
    for (unsigned int i = 0; i < count; ++i) {
        events[i] = fault_events[(fault_next + ARRAY_SIZE(fault_events) - 1u - i) %
                                  ARRAY_SIZE(fault_events)];
    }
    k_mutex_unlock(&lock);
    return count;
}

struct lifter_status lifter_control_status(void)
{
    k_mutex_lock(&lock, K_FOREVER);
    struct lifter_status copy = status;
    k_mutex_unlock(&lock);
    return copy;
}

struct lifter_config lifter_control_config(void)
{
    k_mutex_lock(&lock, K_FOREVER);
    struct lifter_config copy = config;
    k_mutex_unlock(&lock);
    return copy;
}

bool lifter_control_pairing_request(void)
{
    k_mutex_lock(&lock, K_FOREVER);
    bool result = pairing_requested;
    pairing_requested = false;
    k_mutex_unlock(&lock);
    return result;
}

void lifter_control_pairing_window(bool open)
{
    k_mutex_lock(&lock, K_FOREVER);
    status.pairing_window = open;
    k_mutex_unlock(&lock);
}

void lifter_control_tick(uint32_t now_ms, uint8_t buttons, int range_mm,
                         bool range_valid, bool pd_ok, uint8_t limits)
{
    k_mutex_lock(&lock, K_FOREVER);
    status.pd_ok = pd_ok;
    status.range_valid = range_valid;
    status.calibrated = (config.valid_bits & LIFTER_VALID_OFFSET) != 0;
    status.quiet = quiet_profile;
    if (range_valid) {
        status.distance_mm = range_mm;
        status.height_mm = range_mm + config.offset_mm;
    }
    if (!pd_ok) status.fault = LIFTER_FAULT_PD;
    else if (!range_valid) status.fault = LIFTER_FAULT_RANGE;
    else if (lifter_motor_fault()) status.fault = LIFTER_FAULT_DRIVER;
    else status.fault = LIFTER_OK;

    record_fault(now_ms);

    process_memory(now_ms, buttons);
    if (status.fault != LIFTER_OK || status.maintenance) {
        stop_locked();
        k_mutex_unlock(&lock);
        return;
    }

    bool up = (buttons & BIT(LIFTER_BTN_UP)) != 0;
    bool down = (buttons & BIT(LIFTER_BTN_DOWN)) != 0;
    if (up || down) {
        automatic = false;
        if (up == down ||
            (up && (limits & LIFTER_LIMIT_HIGH)) ||
            (down && (limits & LIFTER_LIMIT_LOW)) ||
            (up && !inside_bounds(status.height_mm + 1)) ||
            (down && !inside_bounds(status.height_mm - 1))) {
            stop_locked();
        } else {
            enum lifter_motion direction = up ? LIFTER_UP : LIFTER_DOWN;
            if (lifter_motor_run(direction, false, quiet_profile)) {
                status.fault = LIFTER_FAULT_DRIVER;
                record_fault(now_ms);
                stop_locked();
            } else status.motion = direction;
        }
    } else if (automatic) {
        int32_t error = target_distance_mm - status.distance_mm;
        int32_t magnitude = error < 0 ? -error : error;
        if (magnitude <= TARGET_BAND_MM ||
            (error > 0 && (limits & LIFTER_LIMIT_HIGH)) ||
            (error < 0 && (limits & LIFTER_LIMIT_LOW)) ||
            !inside_bounds(status.height_mm + (error > 0 ? 1 : -1))) {
            stop_locked();
        } else {
            enum lifter_motion direction = error > 0 ? LIFTER_UP : LIFTER_DOWN;
            if (lifter_motor_run(direction, magnitude <= SLOW_BAND_MM, quiet_profile)) {
                status.fault = LIFTER_FAULT_DRIVER;
                record_fault(now_ms);
                stop_locked();
            } else status.motion = LIFTER_AUTO;
        }
    } else if (status.motion != LIFTER_IDLE) {
        stop_locked();
    }
    k_mutex_unlock(&lock);
}
