#include "table_lifter.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <errno.h>

LOG_MODULE_REGISTER(lifter_buttons, LOG_LEVEL_INF);

static const struct gpio_dt_spec buttons[LIFTER_BUTTON_COUNT] = {
    GPIO_DT_SPEC_GET(DT_ALIAS(up_button), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(down_button), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(memory1_button), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(memory2_button), gpios),
};
static const struct gpio_dt_spec limits[] = {
    GPIO_DT_SPEC_GET(DT_NODELABEL(table_io), lower_limit_gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(table_io), upper_limit_gpios),
};
static uint8_t stable;
static uint8_t candidate;
static int64_t candidate_since;

int lifter_buttons_init(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(buttons); ++i) {
        if (!gpio_is_ready_dt(&buttons[i])) {
            LOG_ERR("Button %zu GPIO is not ready", i);
            return -ENODEV;
        }
        int err = gpio_pin_configure_dt(&buttons[i], GPIO_INPUT);
        if (err) {
            LOG_ERR("Button %zu GPIO configuration failed: %d", i, err);
            return err;
        }
    }
    for (size_t i = 0; i < ARRAY_SIZE(limits); ++i) {
        if (!gpio_is_ready_dt(&limits[i])) return -ENODEV;
        int err = gpio_pin_configure_dt(&limits[i], GPIO_INPUT);
        if (err) return err;
    }
    LOG_INF("Handset and normally closed limit switches ready");
    return 0;
}

uint8_t lifter_buttons_read(void)
{
    uint8_t raw = 0;
    int64_t now = k_uptime_get();
    for (size_t i = 0; i < ARRAY_SIZE(buttons); ++i) {
        int value = gpio_pin_get_dt(&buttons[i]);
        if (value > 0) raw |= BIT(i);
        if (value < 0) return stable;
    }
    if (raw != candidate) {
        candidate = raw;
        candidate_since = now;
    } else if (now - candidate_since >= 30) {
        stable = candidate;
    }
    return stable;
}

uint8_t lifter_limits_read(void)
{
    uint8_t active = 0;
    /* NC to ground: opened switch, cable break or read error blocks travel. */
    for (size_t i = 0; i < ARRAY_SIZE(limits); ++i) {
        if (gpio_pin_get_dt(&limits[i]) != 0) active |= BIT(i);
    }
    return active;
}
