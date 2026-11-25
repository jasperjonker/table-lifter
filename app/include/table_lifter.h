#ifndef TABLE_LIFTER_H_
#define TABLE_LIFTER_H_

#include <stdbool.h>
#include <stdint.h>

enum lifter_motion { LIFTER_IDLE, LIFTER_UP, LIFTER_DOWN, LIFTER_AUTO };
enum lifter_fault { LIFTER_OK, LIFTER_FAULT_PD, LIFTER_FAULT_RANGE,
                    LIFTER_FAULT_DRIVER, LIFTER_FAULT_CONFIG };
enum lifter_button { LIFTER_BTN_UP, LIFTER_BTN_DOWN, LIFTER_BTN_MEM1,
                     LIFTER_BTN_MEM2, LIFTER_BUTTON_COUNT };

struct lifter_status {
    int32_t distance_mm;
    int32_t height_mm;
    int32_t target_mm;
    enum lifter_motion motion;
    enum lifter_fault fault;
    bool range_valid;
    bool pd_ok;
    bool pairing_window;
    bool calibrated;
    bool quiet;
    bool maintenance;
};

struct lifter_pd_diagnostics {
    uint32_t rdo;
    int32_t bus_mv;
    bool contract_ok;
    bool alert;
};

struct lifter_range_diagnostics {
    int32_t raw_mm;
    int32_t range_status;
    uint32_t age_ms;
};

struct lifter_motor_diagnostics {
    uint32_t gstat;
    uint32_t drv_status;
    bool spi_ok;
    bool diag0;
    bool diag1;
};

struct lifter_fault_event {
    uint32_t uptime_s;
    enum lifter_fault fault;
};

struct lifter_config {
    int32_t offset_mm;
    int32_t preset_mm[2];
    int32_t min_height_mm;
    int32_t max_height_mm;
    uint32_t valid_bits;
};

#define LIFTER_LIMIT_LOW 0x01u
#define LIFTER_LIMIT_HIGH 0x02u

/** @brief Read fail-closed endstops; bit 0 lower and bit 1 upper. */
uint8_t lifter_limits_read(void);

#define LIFTER_VALID_PRESET1 0x01u
#define LIFTER_VALID_PRESET2 0x02u
#define LIFTER_VALID_BOUNDS 0x04u
#define LIFTER_VALID_OFFSET 0x08u

/** @brief Load persisted control settings and reset transient state.
 *  @return 0 on success, or a negative errno value.
 */
int lifter_control_init(void);

/** @brief Advance the motion state machine using the latest inputs.
 *  @param now_ms Monotonic uptime in milliseconds.
 *  @param buttons Bitmask of pressed buttons.
 *  @param range_mm Latest measured floor distance in millimetres.
 *  @param range_valid Whether the measurement is valid and fresh.
 *  @param pd_ok Whether the required power contract is live.
 *  @param limits Active endstops: bit 0 lower, bit 1 upper.
 */
void lifter_control_tick(uint32_t now_ms, uint8_t buttons, int range_mm,
                         bool range_valid, bool pd_ok, uint8_t limits);

/** @brief Request an automatic move to a calibrated tabletop height.
 *  @param height_mm Target height in millimetres.
 *  @return 0 on acceptance, or a negative errno value.
 */
int lifter_control_target(int32_t height_mm);

/** @brief Stop the current move and clear the automatic target. */
void lifter_control_stop(void);

/** @brief Recall a stored floor-distance preset.
 *  @param index Zero-based preset index.
 *  @return 0 on acceptance, or a negative errno value.
 */
int lifter_control_preset(unsigned int index);

/** @brief Store the current valid floor distance as a preset.
 *  @param index Zero-based preset index.
 *  @return 0 on success, or a negative errno value.
 */
int lifter_control_store(unsigned int index);

/** @brief Set the sensor-to-tabletop calibration offset.
 *  @param offset_mm Signed offset in millimetres.
 *  @return 0 on success, or a negative errno value.
 */
int lifter_control_offset(int32_t offset_mm);

/** @brief Set measured tabletop-height travel bounds.
 *  @param min_mm Minimum allowed height in millimetres.
 *  @param max_mm Maximum allowed height in millimetres.
 *  @return 0 on success, or a negative errno value.
 */
int lifter_control_bounds(int32_t min_mm, int32_t max_mm);

/** @brief Select and persist the lower-speed quiet profile.
 *  @param quiet True for quiet motion, false for normal motion.
 *  @return 0 on success, or a negative errno value.
 */
int lifter_control_set_quiet(bool quiet);

/** @brief Latch OTA maintenance while motion is idle.
 *  @return 0 if maintenance is active, or -EBUSY during motion.
 */
int lifter_control_enter_maintenance(void);

/** @brief Clear maintenance after an explicit cancelled upload. */
void lifter_control_cancel_maintenance(void);

/** @brief Copy the most recent fault transitions, newest first.
 *  @param events Destination for four event records.
 *  @return Number of valid records.
 */
unsigned int lifter_control_fault_history(struct lifter_fault_event events[4]);

/** @brief Get a snapshot of the motion and sensor status.
 *  @return Current status snapshot.
 */
struct lifter_status lifter_control_status(void);

/** @brief Get a snapshot of persisted control settings.
 *  @return Current settings snapshot.
 */
struct lifter_config lifter_control_config(void);

/** @brief Consume a pending local pairing gesture.
 *  @return True once for each completed gesture.
 */
bool lifter_control_pairing_request(void);

/** @brief Report whether Bluetooth pairing is currently open.
 *  @param open True while the pairing window is open.
 */
void lifter_control_pairing_window(bool open);

/** @brief Initialize the TMC5160 and verify SPI communication.
 *  @return 0 on success, or a negative errno value.
 */
int lifter_motor_init(void);

/** @brief Run the motor in a direction using the selected speed ramp.
 *  @param direction Requested up or down direction.
 *  @param slow True to use the near-target speed.
 *  @param quiet True to use the quiet speed and acceleration profile.
 *  @return 0 on success, or a negative errno value.
 */
int lifter_motor_run(enum lifter_motion direction, bool slow, bool quiet);

/** @brief Stop the ramp controller and disable the motor outputs. */
void lifter_motor_stop(void);

/** @brief Check TMC5160 status and SPI communication.
 *  @return True if the motor is uninitialized or a driver fault is present.
 */
bool lifter_motor_fault(void);

/** @brief Read the latest raw TMC register diagnostics.
 *  @return Last motor diagnostics snapshot.
 */
struct lifter_motor_diagnostics lifter_motor_diagnostics(void);

/** @brief Start the LiDAR ranging thread.
 *  @return 0 on success, or a negative errno value.
 */
int lifter_range_init(void);

/** @brief Read the latest valid, fresh floor distance.
 *  @param distance_mm Destination for the distance in millimetres.
 *  @return 0 on success, or a negative errno value.
 */
int lifter_range_read(int *distance_mm);

/** @brief Read raw LiDAR status, distance, and sample age.
 *  @return Last LiDAR diagnostics snapshot.
 */
struct lifter_range_diagnostics lifter_range_diagnostics(void);

/** @brief Initialize the STUSB4500 I2C and motor-bus ADC paths.
 *  @return 0 on success, or a negative errno value.
 */
int lifter_pd_init(void);

/** @brief Check the negotiated 20 V / 3 A contract and motor bus.
 *  @return True only while the contract and measured voltage are valid.
 */
bool lifter_pd_contract_ok(void);

/** @brief Read the last measured PD and motor-bus data.
 *  @return Last PD diagnostics snapshot.
 */
struct lifter_pd_diagnostics lifter_pd_diagnostics(void);

/** @brief Program and verify the STUSB4500 PDO3 20 V / 3 A NVM profile.
 *  @return 0 on success, or a negative errno value.
 */
int lifter_pd_provision_20v3a(void);

/** @brief Configure the four active-low handset inputs.
 *  @return 0 on success, or a negative errno value.
 */
int lifter_buttons_init(void);

/** @brief Read debounced pressed-button state.
 *  @return Bitmask of pressed buttons.
 */
uint8_t lifter_buttons_read(void);

/** @brief Configure the red, green and blue status LEDs.
 *  @return 0 on success, or a negative errno value.
 */
int lifter_led_init(void);

/** @brief Show ready, motion/pairing or fault status on the LEDs.
 *  @param status Current control status.
 */
void lifter_led_update(const struct lifter_status *status);

/** @brief Start Bluetooth advertising, bonding and MCUmgr hooks.
 *  @return 0 on success, or a negative errno value.
 */
int lifter_ble_init(void);

/** @brief Update pairing-window state and notify subscribed clients.
 *  @param status Current control status.
 */
void lifter_ble_update(const struct lifter_status *status);

#endif
