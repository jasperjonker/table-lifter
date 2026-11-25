# Table Lifter firmware

Zephyr application for the nRF54L15-QGAA-R7 table controller. Hardware mapping is
from the 14 November 2026 schematic prints, SHA-256
`a5c4105c0f1d09d376fe98c5227dd36e23a2191fb41ffd0bd0423c2143f0d465`.

| Function | nRF54L15 pins |
| --- | --- |
| TMC5160 SPI | P2.01 SCK, P2.02 MOSI, P2.04 MISO, P2.05 CS |
| Driver enable / LiDAR XSHUT | P2.00 / P2.03 |
| STUSB4500 I²C / VL53L1X I²C | P1.04–P1.05 / P1.08–P1.09 |
| Up, down, memory 1, memory 2 | P0.00–P0.03, active low |
| RGB LED, red, green, blue | P1.02, P1.03, P1.16, active low |
| Motor-bus ADC | P1.06, SAADC AIN2, 330 kΩ / 10 kΩ divider |

SWD uses the dedicated SWDIO/SWDCLK interface. The Tag-Connect header exposes
the RTT console and logs through a J-Link probe; see [RTT debugging](docs/rtt.md).

## Development

Clone this repository, open its folder in VS Code and
**Reopen in Container**. The Ubuntu 26.04
devcontainer installs the Ubuntu ARM GNU toolchain, west, the pinned Zephyr tree and its
modules. Run the `Zephyr: board build` task. The signing key is generated once
in the persistent `table-lifter-signing-v3` Docker volume, outside this repository.
Back it up before using OTA on a deployed board: losing it prevents future
updates accepted by that bootloader. For production, replace it with a
protected, backed-up key.

`app/west.yml` pins Zephyr main commit
`7cd4b562b30a9bcd76a7288ad90d5e71dd6f87d2` (observed 14 November
2026). To update, run `git ls-remote` for main, replace that revision, then
`west update && bash scripts/apply-zephyr-patches.sh`; rebuild and record the
validated commit. The small Zephyr patch exposes the ST ranging status as
`sensor_value.val2` for `SENSOR_CHAN_DISTANCE` in this application. It is
necessary because upstream hides the status that distinguishes valid floor
ranges from invalid measurements.

## Operation

- UP/DOWN jog only while held. Both together stop. No bounds are active until
  measured and configured. Motion still requires a valid LiDAR sample and a
  live 20 V / 3 A PD contract.
- MEM1/MEM2: tap to recall, tap either while travelling automatically to stop,
  hold two seconds to store the current floor distance. Hold both five
  seconds to allow pairing for one minute.
- Presets persist as floor distances. A later tabletop calibration therefore
  does not change the stored physical positions. Bluetooth height equals
  floor distance plus the calibrated sensor-to-tabletop offset.
- RGB LED: green ready, blue moving or pairing, red fault. Red flashes one
  to four times for PD, LiDAR, TMC or configuration faults. Blue flashes
  during OTA maintenance. See [RTT debugging](docs/rtt.md) for the codes.
- The quiet profile is 15 rpm maximum with 5 rpm/s acceleration; it is
  selected over Bluetooth and stored on the board. Normal mode remains
  30 rpm and 10 rpm/s.

First provisioning uses the RTT shell command `pd provision 20v3a`. The code
reads the current NVM, changes only PDO3's current lookup nibble from 1 A to
3 A, writes sector 3, and reads it back. It refuses unknown NVM layouts.
Power cycle the board and check the negotiated RDO and bus voltage. A 5 V
source can power startup logic, but cannot enable motion.

The initial TMC setting is approximately 1 A phase current with 50 mΩ shunts,
30 rpm maximum, and 10 rpm/s acceleration. The motor's 3 A marking is a
validation ceiling, not the initial drive current. Normally closed endstops on J400/J401 are checked every control cycle.
An open lower/upper circuit blocks DOWN/UP respectively. See [commissioning](docs/commissioning.md) before
coupling to the table.

The firmware exposes a [versioned GATT protocol](docs/bluetooth.md) and
Bluetooth MCUmgr with signed MCUboot images. The [Android app](docs/android.md)
shows live height, memories, quiet mode and phone-only sit/stand reminders,
and uploads signed application updates over Bluetooth. Increase `app/VERSION`
for each release and select `build/board/app/zephyr/zephyr.signed.bin` in the
app. The current application version is 0.1.1.
The first OTA chunk enters maintenance mode and blocks motion until reboot or
an explicit cancellation of an incomplete upload.

## Build, flash and debug

Use the VS Code tasks `Zephyr: board build`, `Zephyr: flash board (J-Link)` and
`Zephyr: control tests`. The flash task builds first, then flashes MCUboot and
the signed application through the J-Link Remote Server on the host. Start it
with the probe connected:

```sh
JLinkRemoteServerCLExe -Port 19020
```

Then run the flash task inside VS Code. For source debugging, close the remote
server and start the host GDB server instead:

```sh
JLinkGDBServerCLExe -device nRF54L15_M33 -endian little -if SWD -speed 4000 -port 2331
```

Select `Table Lifter: SEGGER J-Link attach` in VS Code Run and Debug. The
launch configuration attaches to the running firmware and uses the app ELF.
See [RTT debugging](docs/rtt.md) for the Tag-Connect setup and RTT channels.

## Verification status (25 November 2026)

The Ubuntu 26.04 container fetched Zephyr main commit
`7cd4b562b30a9bcd76a7288ad90d5e71dd6f87d2` and its manifest modules.
The custom-board MCUboot sysbuild completed and produced
`build/board/app/zephyr/zephyr.elf` and signed application images. All ten
controller tests pass on this pinned revision. The Android debug APK also
builds with JDK 17, SDK 37.0, Gradle 9.6.0 and AGP 9.4.0; its location is
`android/app/build/outputs/apk/debug/app-debug.apk`. The executable
post-create script completes, and the rebuilt container has J-Link Commander
and GDB.
SWD flashing, PD provisioning and unloaded motor rotation have now worked on
the physical board. Jasper confirmed the bench rotation works and reports the
distance sensor and endstops installed. Normal controls still need a live
commissioning check; Bluetooth, OTA and the Android screen are not hardware-validated.
The normal board build and 11 simulated controller tests pass. Simulated tests
do not exercise flash persistence; verify memory recall after a power cycle.

See [handset and limit-switch wiring](docs/wiring.md). The default build uses
normal controls and does not rotate automatically at boot. The separate
TABLE_LIFTER_BENCH_TEST and TABLE_LIFTER_SPI_PROBE CMake options are OFF by
default; do not enable the automatic bench image on a coupled table.
