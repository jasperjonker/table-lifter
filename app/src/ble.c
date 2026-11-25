#include "table_lifter.h"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(lifter_ble, LOG_LEVEL_INF);

#define TL_UUID(suffix) BT_UUID_128_ENCODE(0xb545afe1, 0x55f0, 0x4e9c, 0x8f96, (suffix))
static const struct bt_uuid_128 service_uuid = BT_UUID_INIT_128(TL_UUID(0x5d458b500001));
static const struct bt_uuid_128 status_uuid = BT_UUID_INIT_128(TL_UUID(0x5d458b500002));
static const struct bt_uuid_128 command_uuid = BT_UUID_INIT_128(TL_UUID(0x5d458b500003));
static const struct bt_uuid_128 config_uuid = BT_UUID_INIT_128(TL_UUID(0x5d458b500004));
static const struct bt_uuid_128 diagnostics_uuid = BT_UUID_INIT_128(TL_UUID(0x5d458b500005));
static const struct bt_uuid_128 history_uuid = BT_UUID_INIT_128(TL_UUID(0x5d458b500006));
static const struct bt_data advertising_data[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static struct bt_conn *active_conn;
static int64_t pairing_until;
static K_MUTEX_DEFINE(pairing_lock);
static atomic_t image_uploaded;

struct bond_query {
    const bt_addr_le_t *peer;
    bool found;
};

/** @brief Match an enumerated bond to the active Bluetooth peer.
 *  @param info Bond record being visited.
 *  @param user_data Bond query state.
 */
static void bond_check(const struct bt_bond_info *info, void *user_data)
{
    struct bond_query *query = user_data;
    if (!bt_addr_le_cmp(&info->addr, query->peer)) query->found = true;
}

/** @brief Check that a connection is both encrypted and bonded.
 *  @param conn Bluetooth connection to check.
 *  @return True when both requirements are met.
 */
static bool bonded_encrypted(struct bt_conn *conn)
{
    if (bt_conn_get_security(conn) < BT_SECURITY_L2) return false;
    struct bt_conn_info conn_info;
    if (bt_conn_get_info(conn, &conn_info)) return false;
    struct bond_query query = {
        .peer = bt_conn_get_dst(conn), .found = false,
    };
    bt_foreach_bond(conn_info.id, bond_check, &query);
    return query.found;
}

/** @brief Encode the versioned status characteristic payload.
 *  @param payload Destination for the 16-byte payload.
 *  @param status Control state to encode.
 */
static void encode_status(uint8_t payload[16], const struct lifter_status *status)
{
    payload[0] = 1;
    payload[1] = status->motion;
    payload[2] = status->fault;
    payload[3] = (status->range_valid ? BIT(0) : 0) |
                        (status->pd_ok ? BIT(1) : 0) |
                        (status->calibrated ? BIT(2) : 0) |
                        (status->pairing_window ? BIT(3) : 0) |
                        (status->quiet ? BIT(4) : 0) |
                        (status->maintenance ? BIT(5) : 0);
    sys_put_le32(status->distance_mm, &payload[4]);
    sys_put_le32(status->height_mm, &payload[8]);
    sys_put_le32(status->target_mm, &payload[12]);
}

/** @brief Serve a GATT status read.
 *  @param conn Reading Bluetooth connection.
 *  @param attr GATT characteristic attribute.
 *  @param buf Destination buffer.
 *  @param len Destination capacity in bytes.
 *  @param offset Read offset in bytes.
 *  @return Bytes read, or a negative ATT error.
 */
static ssize_t read_status(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           void *buf, uint16_t len, uint16_t offset)
{
    struct lifter_status status = lifter_control_status();
    uint8_t payload[16];
    encode_status(payload, &status);
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             payload, sizeof(payload));
}

/** @brief Serve an encrypted GATT configuration read.
 *  @param conn Reading Bluetooth connection.
 *  @param attr GATT characteristic attribute.
 *  @param buf Destination buffer.
 *  @param len Destination capacity in bytes.
 *  @param offset Read offset in bytes.
 *  @return Bytes read, or a negative ATT error.
 */
static ssize_t read_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           void *buf, uint16_t len, uint16_t offset)
{
    uint8_t payload[25];
    struct lifter_config config = lifter_control_config();
    payload[0] = 1;
    sys_put_le32(config.valid_bits, &payload[1]);
    sys_put_le32(config.offset_mm, &payload[5]);
    sys_put_le32(config.preset_mm[0], &payload[9]);
    sys_put_le32(config.preset_mm[1], &payload[13]);
    sys_put_le32(config.min_height_mm, &payload[17]);
    sys_put_le32(config.max_height_mm, &payload[21]);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, payload, sizeof(payload));
}

/** @brief Pack the latest hardware diagnostics into a 20-byte BLE payload.
 *  @param payload Destination for versioned diagnostics.
 */
static void encode_diagnostics(uint8_t payload[20])
{
    struct lifter_pd_diagnostics pd = lifter_pd_diagnostics();
    struct lifter_range_diagnostics range = lifter_range_diagnostics();
    struct lifter_motor_diagnostics motor = lifter_motor_diagnostics();
    payload[0] = 1;
    payload[1] = (pd.contract_ok ? BIT(0) : 0) | (motor.spi_ok ? BIT(1) : 0) |
                 (pd.alert ? BIT(2) : 0) | (motor.diag0 ? BIT(3) : 0) |
                 (motor.diag1 ? BIT(4) : 0);
    payload[2] = (uint8_t)range.range_status;
    payload[3] = (uint8_t)MIN(range.age_ms / 10u, 255u);
    sys_put_le16((uint16_t)CLAMP(range.raw_mm, 0, UINT16_MAX), &payload[4]);
    sys_put_le16((uint16_t)CLAMP(pd.bus_mv, 0, UINT16_MAX), &payload[6]);
    sys_put_le32(pd.rdo, &payload[8]);
    sys_put_le32(motor.gstat, &payload[12]);
    sys_put_le32(motor.drv_status, &payload[16]);
}

/** @brief Serve a bonded encrypted read of current hardware diagnostics.
 *  @param conn Reading connection.
 *  @param attr GATT attribute.
 *  @param buf Destination buffer.
 *  @param len Buffer capacity.
 *  @param offset Read offset.
 *  @return Bytes read or an ATT error.
 */
static ssize_t read_diagnostics(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                void *buf, uint16_t len, uint16_t offset)
{
    uint8_t payload[20];
    encode_diagnostics(payload);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, payload, sizeof(payload));
}

/** @brief Serve the four newest fault transitions from volatile memory.
 *  @param conn Reading connection.
 *  @param attr GATT attribute.
 *  @param buf Destination buffer.
 *  @param len Buffer capacity.
 *  @param offset Read offset.
 *  @return Bytes read or an ATT error.
 */
static ssize_t read_history(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                            void *buf, uint16_t len, uint16_t offset)
{
    struct lifter_fault_event events[4] = {0};
    uint8_t payload[22] = {0};
    payload[0] = 1;
    payload[1] = lifter_control_fault_history(events);
    for (unsigned int i = 0; i < payload[1]; ++i) {
        sys_put_le32(events[i].uptime_s, &payload[2 + i * 5]);
        payload[6 + i * 5] = events[i].fault;
    }
    return bt_gatt_attr_read(conn, attr, buf, len, offset, payload, sizeof(payload));
}

/** @brief Decode an authenticated motion or settings GATT command.
 *  @param conn Writing Bluetooth connection.
 *  @param attr GATT characteristic attribute.
 *  @param buf Command payload.
 *  @param len Payload length in bytes.
 *  @param offset Write offset in bytes.
 *  @param flags Zephyr GATT write flags.
 *  @return Bytes accepted, or a negative ATT error.
 */
static ssize_t write_command(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    const uint8_t *data = buf;
    int err = -EINVAL;
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);
    if (!bonded_encrypted(conn)) return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
    if (offset || len < 2 || data[0] != 1) return BT_GATT_ERR(BT_ATT_ERR_INVALID_PDU);
    if (lifter_control_status().maintenance && data[1] != 4 && data[1] != 8)
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    switch (data[1]) {
    case 1: /* target height in millimetres */
        if (len == 6) err = lifter_control_target((int32_t)sys_get_le32(&data[2]));
        break;
    case 2: /* recall preset: 0 or 1 */
        if (len == 3) err = lifter_control_preset(data[2]);
        break;
    case 3: /* store preset: 0 or 1 */
        if (len == 3) err = lifter_control_store(data[2]);
        break;
    case 4: /* stop */
        if (len == 2) { lifter_control_stop(); err = 0; }
        break;
    case 5: /* tabletop offset in millimetres */
        if (len == 6) err = lifter_control_offset((int32_t)sys_get_le32(&data[2]));
        break;
    case 6: /* measured height bounds in millimetres */
        if (len == 10) err = lifter_control_bounds((int32_t)sys_get_le32(&data[2]),
                                                   (int32_t)sys_get_le32(&data[6]));
        break;
    case 7: /* quiet speed profile */
        if (len == 3 && data[2] <= 1) err = lifter_control_set_quiet(data[2] != 0);
        break;
    case 8: /* explicit cancellation of an incomplete OTA upload */
        if (len == 2 && !atomic_get(&image_uploaded)) {
            lifter_control_cancel_maintenance();
            err = 0;
        }
        break;
    default: break;
    }
    if (err) LOG_WRN("Bluetooth command %u rejected: %d", data[1], err);
    else LOG_INF("Bluetooth command %u accepted", data[1]);
    return err ? BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED) : len;
}

/** @brief Handle changes to the status notification subscription.
 *  @param attr CCC descriptor attribute.
 *  @param value New CCC value.
 */
static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    ARG_UNUSED(attr);
    ARG_UNUSED(value);
}

BT_GATT_SERVICE_DEFINE(table_service,
    BT_GATT_PRIMARY_SERVICE(&service_uuid.uuid),
    BT_GATT_CHARACTERISTIC(&status_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ, read_status, NULL, NULL),
    BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    BT_GATT_CHARACTERISTIC(&command_uuid.uuid, BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_WRITE_ENCRYPT, NULL, write_command, NULL),
    BT_GATT_CHARACTERISTIC(&config_uuid.uuid, BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ_ENCRYPT, read_config, NULL, NULL),
    BT_GATT_CHARACTERISTIC(&diagnostics_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ_ENCRYPT, read_diagnostics, NULL, NULL),
    BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    BT_GATT_CHARACTERISTIC(&history_uuid.uuid, BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ_ENCRYPT, read_history, NULL, NULL)
);

/** @brief Accept new Bluetooth pairing only during the local pairing window.
 *  @param conn Connection requesting pairing.
 *  @param feat Peer pairing capabilities.
 *  @return Bluetooth pairing policy result.
 */
static enum bt_security_err pairing_accept(struct bt_conn *conn,
                                           const struct bt_conn_pairing_feat *feat)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(feat);
    k_mutex_lock(&pairing_lock, K_FOREVER);
    bool open = k_uptime_get() < pairing_until;
    k_mutex_unlock(&pairing_lock);
    return open ? BT_SECURITY_ERR_SUCCESS : BT_SECURITY_ERR_PAIR_NOT_ALLOWED;
}

static const struct bt_conn_auth_cb auth_callbacks = {
    .pairing_accept = pairing_accept,
};

/** @brief Track a new Bluetooth connection and request encryption.
 *  @param conn Connected peer.
 *  @param err Connection result from Zephyr.
 */
static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err || active_conn) return;
    active_conn = bt_conn_ref(conn);
    LOG_INF("Bluetooth connected; requesting encryption");
    (void)bt_conn_set_security(conn, BT_SECURITY_L2);
}

/** @brief Release a disconnected peer and restart advertising.
 *  @param conn Disconnected peer.
 *  @param reason Bluetooth disconnect reason.
 */
static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    ARG_UNUSED(reason);
    if (active_conn == conn) {
        bt_conn_unref(active_conn);
        active_conn = NULL;
    }
    LOG_INF("Bluetooth disconnected; active target remains under local control");
    /* Motion intentionally continues after a phone disconnect. */
    (void)bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, advertising_data,
                          ARRAY_SIZE(advertising_data), NULL, 0);
}

BT_CONN_CB_DEFINE(table_conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

/** @brief Latch maintenance before accepting the first authenticated DFU chunk.
 *  @param event MCUmgr callback event.
 *  @param previous Result from any previous callback.
 *  @param err_rc Destination for an MCUmgr error code.
 *  @param err_group Destination for an MCUmgr error group.
 *  @param abort_more Destination for callback-chain control.
 *  @param data Event-specific payload.
 *  @param data_size Payload size in bytes.
 *  @return MCUmgr callback result.
 */
static enum mgmt_cb_return upload_check(uint32_t event, enum mgmt_cb_return previous,
                                         int32_t *err_rc, uint16_t *err_group,
                                         bool *abort_more, void *data, size_t data_size)
{
    ARG_UNUSED(event);
    ARG_UNUSED(err_group);
    ARG_UNUSED(abort_more);
    ARG_UNUSED(data);
    ARG_UNUSED(data_size);
    if (previous != MGMT_CB_OK) return previous;
    if (!active_conn || !bonded_encrypted(active_conn) ||
        lifter_control_enter_maintenance()) {
        *err_rc = MGMT_ERR_EACCESSDENIED;
        return MGMT_CB_ERROR_RC;
    }
    return MGMT_CB_OK;
}

/** @brief Keep motion locked after a complete image transfer.
 *  @param event MCUmgr event.
 *  @param previous Earlier callback result.
 *  @param err_rc Unused MCUmgr error destination.
 *  @param err_group Unused error group destination.
 *  @param abort_more Unused callback-chain control.
 *  @param data Unused event payload.
 *  @param data_size Unused payload size.
 *  @return Unchanged prior callback result.
 */
static enum mgmt_cb_return upload_finished(uint32_t event, enum mgmt_cb_return previous,
                                            int32_t *err_rc, uint16_t *err_group,
                                            bool *abort_more, void *data, size_t data_size)
{
    ARG_UNUSED(err_rc);
    ARG_UNUSED(err_group);
    ARG_UNUSED(abort_more);
    ARG_UNUSED(data);
    ARG_UNUSED(data_size);
    if (event == MGMT_EVT_OP_IMG_MGMT_DFU_PENDING && previous == MGMT_CB_OK) {
        atomic_set(&image_uploaded, 1);
        LOG_INF("OTA image transferred; motion remains locked until reboot");
    }
    return previous;
}

/** @brief Authorize MCUmgr reset only for an idle, bonded encrypted peer.
 *  @param event MCUmgr event.
 *  @param previous Earlier callback result.
 *  @param err_rc MCUmgr error destination.
 *  @param err_group Unused error group destination.
 *  @param abort_more Unused callback-chain control.
 *  @param data Unused event payload.
 *  @param data_size Unused payload size.
 *  @return Callback authorization result.
 */
static enum mgmt_cb_return reset_check(uint32_t event, enum mgmt_cb_return previous,
                                       int32_t *err_rc, uint16_t *err_group,
                                       bool *abort_more, void *data, size_t data_size)
{
    ARG_UNUSED(event);
    ARG_UNUSED(err_group);
    ARG_UNUSED(abort_more);
    ARG_UNUSED(data);
    ARG_UNUSED(data_size);
    if (previous != MGMT_CB_OK) return previous;
    if (!active_conn || !bonded_encrypted(active_conn) ||
        lifter_control_status().motion != LIFTER_IDLE) {
        *err_rc = MGMT_ERR_EACCESSDENIED;
        return MGMT_CB_ERROR_RC;
    }
    return MGMT_CB_OK;
}

static struct mgmt_callback upload_callback = {
    .callback = upload_check,
    .event_id = MGMT_EVT_OP_IMG_MGMT_DFU_CHUNK,
};

static struct mgmt_callback reset_callback = {
    .callback = reset_check,
    .event_id = MGMT_EVT_OP_OS_MGMT_RESET,
};
static struct mgmt_callback finished_callback = {
    .callback = upload_finished,
    .event_id = MGMT_EVT_OP_IMG_MGMT_DFU_PENDING,
};

int lifter_ble_init(void)
{
    int err = bt_enable(NULL);
    if (err) return err;
    err = bt_conn_auth_cb_register(&auth_callbacks);
    if (err) return err;
    err = settings_load_subtree("bt");
    if (err) return err;
    err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, advertising_data,
                          ARRAY_SIZE(advertising_data), NULL, 0);
    if (err) return err;
    mgmt_callback_register(&upload_callback);
    mgmt_callback_register(&reset_callback);
    mgmt_callback_register(&finished_callback);
    LOG_INF("Bluetooth advertising and MCUmgr ready");
    return 0;
}

void lifter_ble_update(const struct lifter_status *status)
{
    k_mutex_lock(&pairing_lock, K_FOREVER);
    if (lifter_control_pairing_request()) {
        pairing_until = k_uptime_get() + 60000;
        LOG_INF("Pairing window open for 60 seconds");
    }
    bool open = k_uptime_get() < pairing_until;
    k_mutex_unlock(&pairing_lock);
    lifter_control_pairing_window(open);
    static int64_t last_notify;
    if (k_uptime_get() - last_notify >= 1000) {
        struct lifter_status fresh = lifter_control_status();
        uint8_t payload[16];
        encode_status(payload, &fresh);
        (void)bt_gatt_notify(NULL, &table_service.attrs[2], payload,
                              sizeof(payload));
        uint8_t diagnostics[20];
        encode_diagnostics(diagnostics);
        (void)bt_gatt_notify(NULL, &table_service.attrs[9], diagnostics,
                              sizeof(diagnostics));
        last_notify = k_uptime_get();
    }
    ARG_UNUSED(status);
}
