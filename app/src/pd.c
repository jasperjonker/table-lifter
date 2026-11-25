#include "table_lifter.h"

#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(lifter_pd, LOG_LEVEL_INF);

#define PD_NODE DT_NODELABEL(stusb4500)
#define REG_PDO3 0x8d
#define REG_RDO 0x91
#define FTP_DATA 0x53
#define FTP_KEY 0x95
#define FTP_CTRL0 0x96
#define FTP_CTRL1 0x97
#define FTP_POWER 0xc0
#define FTP_REQUEST 0x10

static const struct i2c_dt_spec pd = I2C_DT_SPEC_GET(PD_NODE);
static const struct adc_dt_spec bus_adc = ADC_DT_SPEC_GET_BY_IDX(DT_NODELABEL(table_io), 0);
static const struct gpio_dt_spec pd_alert = GPIO_DT_SPEC_GET(DT_NODELABEL(table_io), pd_alert_gpios);
static struct k_spinlock diagnostics_lock;
static struct lifter_pd_diagnostics diagnostics;

/** @brief Write one STUSB4500 register over I2C.
 *  @param reg Register address.
 *  @param value Byte to write.
 *  @return 0 on success, or a negative errno value.
 */
static int write8(uint8_t reg, uint8_t value)
{
    return i2c_reg_write_byte_dt(&pd, reg, value);
}

/** @brief Wait for an STUSB4500 NVM FTP command to complete.
 *  @return 0 on completion, or a negative errno value.
 */
static int ftp_wait(void)
{
    for (int i = 0; i < 20; ++i) {
        uint8_t value;
        if (i2c_reg_read_byte_dt(&pd, FTP_CTRL0, &value)) return -EIO;
        if (!(value & FTP_REQUEST)) return 0;
        k_msleep(1);
    }
    return -ETIMEDOUT;
}

/** @brief Issue one STUSB4500 NVM FTP operation.
 *  @param opcode FTP operation code.
 *  @param sector NVM sector selector.
 *  @return 0 on completion, or a negative errno value.
 */
static int ftp_command(uint8_t opcode, uint8_t sector)
{
    int err = write8(FTP_CTRL1, opcode);
    if (err) return err;
    err = write8(FTP_CTRL0, FTP_POWER | FTP_REQUEST | sector);
    if (err) return err;
    err = ftp_wait();
    if (err) LOG_ERR("NVM opcode=0x%02x sector=%u failed: %d", opcode, sector, err);
    return err;
}

/** @brief Unlock and power the STUSB4500 NVM FTP interface.
 *  @return 0 on success, or a negative errno value.
 */
static int ftp_enter(void)
{
    int err = write8(FTP_KEY, 0x47);
    if (err) return err;
    err = write8(FTP_CTRL0, 0);
    if (err) return err;
    k_busy_wait(5);
    return write8(FTP_CTRL0, FTP_POWER);
}

/** @brief Power down and lock the STUSB4500 NVM FTP interface. */
static void ftp_exit(void)
{
    uint8_t clear[] = {0x40, 0};
    (void)i2c_burst_write_dt(&pd, FTP_CTRL0, clear, sizeof(clear));
    (void)write8(FTP_KEY, 0);
}

/** @brief Read one eight-byte STUSB4500 NVM bank.
 *  @param bank NVM bank index.
 *  @param data Destination buffer for eight bytes.
 *  @return 0 on success, or a negative errno value.
 */
static int ftp_read_bank(uint8_t bank, uint8_t data[8])
{
    int err = ftp_enter();
    if (!err) err = ftp_command(0, bank);
    if (!err) err = i2c_burst_read_dt(&pd, FTP_DATA, data, 8);
    ftp_exit();
    return err;
}

/** @brief Program the eight-byte PDO3 NVM bank while preserving other banks.
 *  @param data New bank contents.
 *  @return 0 on success, or a negative errno value.
 */
static int ftp_write_bank3(const uint8_t data[8])
{
    int err = ftp_enter();
    if (err) goto out;
    /* STSW-STUSB004 partial erase sequence. Only sector 3 is selected. */
    err = write8(FTP_DATA, 0);
    if (err) goto out;
    err = ftp_command((0x08u << 3) | 0x02u, 0);
    if (err) goto out;
    err = ftp_command(0x07, 0);
    if (err) goto out;
    err = ftp_command(0x05, 0);
    if (err) goto out;
    err = i2c_burst_write_dt(&pd, FTP_DATA, data, 8);
    if (err) goto out;
    err = ftp_command(0x01, 0);
    if (err) goto out;
    err = ftp_command(0x06, 3);
out:
    ftp_exit();
    return err;
}

/** @brief Measure motor-bus voltage through the 34:1 ADC divider.
 *  @return Motor-bus millivolts, or a negative errno value.
 */
static int bus_millivolts(void)
{
    int16_t raw;
    struct adc_sequence sequence = {
        .buffer = &raw,
        .buffer_size = sizeof(raw),
    };
    int err = adc_sequence_init_dt(&bus_adc, &sequence);
    if (err) return err;
    err = adc_read_dt(&bus_adc, &sequence);
    if (err) return err;
    int32_t mv = raw;
    err = adc_raw_to_millivolts_dt(&bus_adc, &mv);
    if (err) return err;
    /* R217=330 kOhm, R218=10 kOhm; ADC sees V_MOTOR/34. */
    return mv * 34;
}

/** @brief Handle the one-time PD profile provisioning shell command.
 *  @param shell Active Zephyr shell session.
 *  @param argc Number of command arguments.
 *  @param argv Command argument strings.
 *  @return 0 on success, or a negative errno value.
 */
static int pd_shell(const struct shell *shell, size_t argc, char **argv)
{
    if (argc != 3 || strcmp(argv[1], "provision") || strcmp(argv[2], "20v3a")) {
        shell_print(shell, "usage: pd provision 20v3a");
        return -EINVAL;
    }
    int err = lifter_pd_provision_20v3a();
    if (err) {
        LOG_ERR("STUSB4500 NVM provisioning failed: %d", err);
        shell_error(shell, "NVM provisioning failed: %d", err);
    } else {
        LOG_INF("STUSB4500 PDO3 NVM verified as 20 V / 3 A; power cycle required");
        shell_print(shell, "PDO3 NVM readback is 20 V / 3 A. Power cycle and verify the live contract.");
    }
    return err;
}

SHELL_CMD_REGISTER(pd, NULL, "STUSB4500 provisioning", pd_shell);

int lifter_pd_init(void)
{
    if (!i2c_is_ready_dt(&pd) || !adc_is_ready_dt(&bus_adc) ||
        !gpio_is_ready_dt(&pd_alert)) return -ENODEV;
    int err = gpio_pin_configure_dt(&pd_alert, GPIO_INPUT);
    return err ? err : adc_channel_setup_dt(&bus_adc);
}

bool lifter_pd_contract_ok(void)
{
    uint8_t bytes[4];
    int rdo_err = i2c_burst_read_dt(&pd, REG_RDO, bytes, sizeof(bytes));
    uint32_t rdo = rdo_err ? 0 : sys_get_le32(bytes);
    int mv = bus_millivolts();
    bool ok = !rdo_err && ((rdo >> 28) & 7u) != 0u && !(rdo & BIT(26)) &&
              ((rdo >> 10) & 0x3ffu) >= 300u && (rdo & 0x3ffu) >= 300u &&
              mv >= 18000 && mv <= 22000;
    bool alert = gpio_pin_get_dt(&pd_alert) > 0;
    k_spinlock_key_t key = k_spin_lock(&diagnostics_lock);
    diagnostics = (struct lifter_pd_diagnostics){
        .rdo = rdo, .bus_mv = mv, .contract_ok = ok,
        .alert = alert,
    };
    k_spin_unlock(&diagnostics_lock, key);
    return ok;
}

struct lifter_pd_diagnostics lifter_pd_diagnostics(void)
{
    k_spinlock_key_t key = k_spin_lock(&diagnostics_lock);
    struct lifter_pd_diagnostics copy = diagnostics;
    k_spin_unlock(&diagnostics_lock, key);
    return copy;
}

int lifter_pd_provision_20v3a(void)
{
    struct lifter_status state = lifter_control_status();
    if (state.motion != LIFTER_IDLE) return -EBUSY;
    uint8_t pdo_bytes[4], bank[8], verify[8];
    int err = i2c_burst_read_dt(&pd, REG_PDO3, pdo_bytes, sizeof(pdo_bytes));
    if (err) {
        LOG_ERR("PDO3 I2C read failed: %d", err);
        return err;
    }
    uint32_t pdo = sys_get_le32(pdo_bytes);
    LOG_INF("PDO3 before provisioning: 0x%08x", pdo);
    if (((pdo >> 10) & 0x3ffu) != 400u) return -EINVAL; /* PDO3 must be 20 V. */
    err = ftp_read_bank(3, bank);
    if (err) return err;
    if (bank[0] != 0x00 || bank[1] != 0x19 || (bank[2] & 0x06) != 0x06)
        return -EINVAL; /* Unknown image: preserve it for manual review. */
    uint8_t current_code = bank[5] >> 4;
    if (current_code == 0x0b) return 0;
    if (current_code != 0x03) return -EINVAL;
    bank[5] = (bank[5] & 0x0fu) | 0xb0u; /* ST LUT 0xB = 3.00 A. */
    err = ftp_write_bank3(bank);
    if (err) return err;
    err = ftp_read_bank(3, verify);
    if (err) return err;
    return memcmp(bank, verify, sizeof(bank)) == 0 ? 0 : -EIO;
}

