# Tag-Connect and SEGGER RTT

## Live diagnostics

Every five seconds and on state transitions, RTT channel 1 logs measured
motor-bus voltage, STUSB RDO and alert pin, raw LiDAR distance/status/sample
age, TMC GSTAT, DRV_STATUS and DIAG pins, SPI health, quiet profile, and OTA
maintenance state.
The controller records up to four recent fault transitions in RAM; the same
records are readable over encrypted Bluetooth. The log backend drops output
if the debugger is disconnected or slow.

The red LED repeats a fault code with 200 ms pulses and an 800 ms pause:
1 pulse = USB-PD contract/bus voltage, 2 = LiDAR range, 3 = TMC driver, and
4 = configuration. Red takes priority over all other colors. Green means
ready/idle; blue means moving or pairing, and flashes at 1 Hz during OTA
maintenance while power and range remain healthy.

Connect a SEGGER J-Link probe to the board's Tag-Connect SWD header. The probe
needs SWDIO, SWDCLK, GND, and the board's 3.3 V target reference; power the
board separately. The board definition names the J-Link device
`nRF54L15_M33` and uses SWD. Start at 4 MHz; lower the SWD speed if the
connection is unreliable. RTT transfers through SWD access to target RAM; it
does not need a separate SWO or UART wire.

Firmware uses two RTT up channels:

| Channel | Purpose | Host tool |
| --- | --- | --- |
| 0 | Interactive Zephyr shell and console | J-Link RTT Viewer or RTT Client |
| 1 | Timestamped Zephyr logs | J-Link RTT Logger |

On the host, install the [SEGGER J-Link Software Pack](https://www.segger.com/downloads/jlink/).
Open RTT Viewer with device `nRF54L15_M33`, interface SWD, and speed 4000 kHz.
Channel 0 is the interactive shell; for example, `help` lists commands and
`pd provision 20v3a` performs the one-time STUSB4500 NVM operation.

To save channel 1 logs on the host, run:

```sh
JLinkRTTLogger -Device nRF54L15_M33 -if SWD -Speed 4000 -RTTChannel 1 table-lifter.log
```

The logger is configured to drop messages if its buffer fills, so an absent
debugger cannot block motion control. Firmware records service startup,
MCUboot confirmation failures, TMC SPI initialization, PD provisioning,
Bluetooth connections and pairing, motion/fault transitions, and a status
snapshot about every five seconds. The shell and logger use separate channels
to avoid competing for channel 0. On a single probe, close one direct J-Link
session before opening another if your installed tools cannot share it.

J-Link is connected to the **host**. The container mounts the host SEGGER tools
and reaches the host over `host.docker.internal`. To use the VS Code flash task,
start `JLinkRemoteServerCLExe -Port 19020` on the host first. The task uses
Zephyr's J-Link runner over this connection and flashes the sysbuild images.
Close the remote server before debugging with the same probe, then start
`JLinkGDBServerCLExe -device nRF54L15_M33 -endian little -if SWD -speed 4000 -port 2331` on
the host and select the VS Code `Table Lifter: SEGGER J-Link attach` launch
configuration. RTT Viewer and Logger also run on the host; close competing
direct probe sessions if they cannot share the probe.

Zephyr documents [separate RTT channels for shell and logging](https://docs.zephyrproject.org/latest/services/shell/index.html#rtt-backend-channel-selection),
and SEGGER documents the [RTT Logger options](https://kb.segger.com/J-Link_RTT_Logger).
