# Bluetooth protocol v1

Service UUID `b545afe1-55f0-4e9c-8f96-5d458b500001`.

| Characteristic | UUID suffix | Access | Payload |
| --- | --- | --- | --- |
| Status | `0002` | read/notify | 16 bytes |
| Command | `0003` | encrypted bonded write | variable |
| Configuration | `0004` | encrypted read | 25 bytes |
| Diagnostics | `0005` | encrypted read/notify | 20 bytes |
| Fault history | `0006` | encrypted read | 22 bytes |

All multi-byte integers are signed little-endian millimetres except the
unsigned bit field. The first byte is protocol version `1`.

Status: version, motion (0 idle, 1 up, 2 down, 3 automatic), fault (0 none,
1 PD, 2 range, 3 driver, 4 configuration), flags (bit 0 valid range, bit 1
20 V/3 A contract, bit 2 calibrated, bit 3 pairing window, bit 4 quiet
profile, bit 5 OTA maintenance lock), distance,
tabletop height, target height. Each integer occupies four bytes.

Configuration: version, valid bits (bit 0 preset 1, bit 1 preset 2, bit 2
bounds, bit 3 offset), offset, preset 1 floor distance, preset 2 floor
distance, minimum height, maximum height. Each field after version is four
bytes.

Commands start with version `1` and one opcode:

| Opcode | Data | Action |
| --- | --- | --- |
| 1 | `int32 height` | Move to tabletop height |
| 2 | `uint8 index` (0 or 1) | Recall preset |
| 3 | `uint8 index` | Store current position |
| 4 | none | Stop |
| 5 | `int32 offset` | Calibrate sensor-to-tabletop offset |
| 6 | `int32 minimum, int32 maximum` | Set measured bounds |
| 7 | `uint8 quiet` (0 or 1) | Select normal or quiet speed while idle |
| 8 | none | Cancel an incomplete OTA upload and unlock motion |

Diagnostics byte layout: version (1), flags (bit 0 valid PD contract, bit 1
TMC SPI readable, bit 2 active PD alert, bits 3/4 active TMC DIAG0/DIAG1),
signed LiDAR range status (`0` valid, negative on I²C
failure), age of last valid reading in 10 ms units (clamped at 255), raw LiDAR
millimetres (uint16), motor bus millivolts (uint16), STUSB4500 RDO (uint32),
TMC GSTAT (uint32), and TMC DRV_STATUS (uint32). The 20-byte record fits the
default BLE notification MTU. RDO object position is bits 28–30; operating and
maximum current are in 10 mA units in bits 0–9 and 10–19. TMC DRV_STATUS bit
26 is overtemperature prewarning; bits 25, 27 and 28 indicate overtemperature
or short faults and stop motion.

Fault history byte layout: version (1), count (0–4), then four five-byte
records, newest first. Each record is uptime seconds (uint32) and fault code
(uint8); unused records are zero. The history is **RAM-only** and clears at
reboot, avoiding flash writes during a fault.

Target height requires calibration. Bounds are inactive until explicitly
configured. A phone disconnect does not cancel an accepted target; the board
controller continues to monitor range and PD status.

Pairing is accepted only during the one-minute window opened by holding both
memory buttons for five seconds. The board has no passkey display; pairing is
encrypted but uses the Bluetooth Just Works association model. Physical button
access is the pairing authorization. Commands additionally verify that the
connection is bonded. MCUmgr image upload is available on an encrypted bonded
connection only while motion is idle. The first accepted upload chunk latches
OTA maintenance: button and Bluetooth motion are then blocked throughout the
upload and after transfer. The lock survives a phone disconnect and clears
only after reboot or an authenticated opcode 8 cancellation while the upload
is incomplete. After a complete transfer, opcode 8 is rejected and a reboot
is required. MCUboot verifies the image signature.

The [Android app](android.md#bluetooth-firmware-updates) uses Nordic's MCUmgr
client to upload a signed image, request a test boot, and confirm the update.
Only one BLE connection is available, so it closes the normal GATT control
connection during the transfer and reconnects afterward.
