#include "table_lifter.h"

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <errno.h>

LOG_MODULE_REGISTER(lifter_range, LOG_LEVEL_INF);

#define RANGE_STALE_MS 300u

static const struct device *const lidar = DEVICE_DT_GET(DT_NODELABEL(lidar));
static K_SEM_DEFINE(start, 0, 1);
static struct k_spinlock sample_lock;
static int last_distance;
static uint32_t last_ms;
static bool last_valid;
static int history[3];
static unsigned int count;
static int raw_distance;
static int range_status = -1;

/** @brief Apply a median-of-three filter to successive valid ranges.
 *  @param raw Latest raw distance in millimetres.
 *  @return Filtered distance in millimetres.
 */
static int median_distance(int raw)
{
    history[count % 3] = raw;
    ++count;
    if (count < 3) return raw;
    int a = history[0], b = history[1], c = history[2];
    if (a > b) { int t = a; a = b; b = t; }
    if (b > c) { int t = b; b = c; c = t; }
    if (a > b) { int t = a; a = b; b = t; }
    return b;
}

/** @brief Poll the LiDAR and publish fresh, range-status-qualified samples.
 *  @param arg1 Unused Zephyr thread argument.
 *  @param arg2 Unused Zephyr thread argument.
 *  @param arg3 Unused Zephyr thread argument.
 */
static void range_thread(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
    k_sem_take(&start, K_FOREVER);
    bool previous_valid = false;
    bool first_sample = true;
    while (true) {
        struct sensor_value value;
        int err = sensor_sample_fetch(lidar);
        if (!err) err = sensor_channel_get(lidar, SENSOR_CHAN_DISTANCE, &value);
        /* val2 is ST RangeStatus from the pinned Zephyr patch: 0 means valid. */
        bool valid = !err && value.val2 == 0 && value.val1 >= 40 && value.val1 <= 4000;
        if (first_sample || valid != previous_valid) {
            if (valid) LOG_INF("LiDAR range valid: %d mm", value.val1);
            else if (err) LOG_WRN("LiDAR read failed: %d", err);
            else LOG_WRN("LiDAR range invalid: status=%d distance=%d mm",
                         value.val2, value.val1);
            previous_valid = valid;
            first_sample = false;
        }
        if (!valid) count = 0;
        int distance = valid ? median_distance(value.val1) : 0;
        k_spinlock_key_t key = k_spin_lock(&sample_lock);
        last_valid = valid;
        raw_distance = err ? 0 : value.val1;
        range_status = err ? err : value.val2;
        if (valid) {
            last_distance = distance;
            last_ms = k_uptime_get_32();
        }
        k_spin_unlock(&sample_lock, key);
        k_msleep(80);
    }
}

K_THREAD_DEFINE(range_thread_id, 4096, range_thread, NULL, NULL, NULL, 8, 0, 0);

int lifter_range_init(void)
{
    if (!device_is_ready(lidar)) {
        LOG_ERR("LiDAR device is not ready");
        return -ENODEV;
    }
    k_sem_give(&start);
    LOG_INF("LiDAR ranging thread started");
    return 0;
}

int lifter_range_read(int *distance_mm)
{
    k_spinlock_key_t key = k_spin_lock(&sample_lock);
    bool valid = last_valid;
    uint32_t age = k_uptime_get_32() - last_ms;
    int distance = last_distance;
    k_spin_unlock(&sample_lock, key);
    if (!valid || age > RANGE_STALE_MS) return -ENODATA;
    *distance_mm = distance;
    return 0;
}

struct lifter_range_diagnostics lifter_range_diagnostics(void)
{
    k_spinlock_key_t key = k_spin_lock(&sample_lock);
    struct lifter_range_diagnostics copy = {
        .raw_mm = raw_distance,
        .range_status = range_status,
        .age_ms = last_ms ? k_uptime_get_32() - last_ms : UINT32_MAX,
    };
    k_spin_unlock(&sample_lock, key);
    return copy;
}
