#include "table_lifter.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <errno.h>

LOG_MODULE_REGISTER(lifter_led, LOG_LEVEL_INF);

static const struct gpio_dt_spec red = GPIO_DT_SPEC_GET(DT_ALIAS(led_red), gpios);
static const struct gpio_dt_spec green = GPIO_DT_SPEC_GET(DT_ALIAS(led_green), gpios);
static const struct gpio_dt_spec blue = GPIO_DT_SPEC_GET(DT_ALIAS(led_blue), gpios);

int lifter_led_init(void)
{
    const struct gpio_dt_spec *leds[] = {&red, &green, &blue};
    for (size_t i = 0; i < ARRAY_SIZE(leds); ++i) {
        if (!gpio_is_ready_dt(leds[i])) {
            LOG_ERR("LED %zu GPIO is not ready", i);
            return -ENODEV;
        }
        int err = gpio_pin_configure_dt(leds[i], GPIO_OUTPUT_INACTIVE);
        if (err) {
            LOG_ERR("LED %zu GPIO configuration failed: %d", i, err);
            return err;
        }
    }
    LOG_INF("RGB status LED ready");
    return 0;
}

void lifter_led_update(const struct lifter_status *status)
{
    bool fault = status->fault != LIFTER_OK;
    uint32_t phase = k_uptime_get_32() % (status->fault * 400u + 800u);
    bool red_on = fault && phase < status->fault * 400u && phase % 400u < 200u;
    (void)gpio_pin_set_dt(&red, red_on);
    (void)gpio_pin_set_dt(&green, !fault && status->pd_ok && status->motion == LIFTER_IDLE);
    (void)gpio_pin_set_dt(&blue, !fault &&
                          (status->motion != LIFTER_IDLE || status->pairing_window ||
                           (status->maintenance && (k_uptime_get_32() / 500u) % 2u == 0u)));
}

void lifter_led_button_feedback(bool pressed)
{
    /* Call after the regular status update so release restores that display.
     * Leave red fault indication intact while confirming button detection. */
    if (pressed) (void)gpio_pin_set_dt(&blue, 1);
}
