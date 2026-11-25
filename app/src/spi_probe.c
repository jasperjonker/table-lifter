#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(spi_probe, LOG_LEVEL_INF);

static const struct gpio_dt_spec enable =
    GPIO_DT_SPEC_GET(DT_NODELABEL(table_io), driver_enable_gpios);
static const struct spi_dt_spec bus = SPI_DT_SPEC_GET(DT_NODELABEL(tmc),
    SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_MODE_CPOL | SPI_MODE_CPHA);

/** @brief Repeatedly read IOIN with recognizable dummy data and bridge disabled. */
int main(void)
{
    if (!gpio_is_ready_dt(&enable) ||
        gpio_pin_configure_dt(&enable, GPIO_OUTPUT_INACTIVE)) return 0;
    if (!spi_is_ready_dt(&bus)) return 0;
    /* Bit 7 of address is zero: read only; the four data bytes are ignored. */
    uint8_t tx[] = {0x04, 0xaa, 0x55, 0xaa, 0x55};
    uint8_t rx[5] = {0};
    struct spi_buf txb = {.buf = tx, .len = sizeof(tx)};
    struct spi_buf rxb = {.buf = rx, .len = sizeof(rx)};
    struct spi_buf_set txs = {.buffers = &txb, .count = 1};
    struct spi_buf_set rxs = {.buffers = &rxb, .count = 1};
    unsigned count = 0;
    LOG_WRN("SPI PROBE ONLY: bridge disabled; mode 3, 2 MHz, 40 clocks every 100 ms");
    while (true) {
        int err = spi_transceive_dt(&bus, &txs, &rxs);
        if ((count++ % 10) == 0) {
            LOG_INF("SPI err=%d status=0x%02x IOIN=0x%08x",
                    err, rx[0], sys_get_be32(&rx[1]));
        }
        k_msleep(100);
    }
}
