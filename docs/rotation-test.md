# Temporary rotation-count mode

Build option TABLE_LIFTER_ROTATION_TEST defaults OFF. This is a temporary
supervised handset mode, not measured-height control. The normal firmware remains
LiDAR-based. This mode disables the LiDAR devicetree device, Bluetooth and OTA.
It reads the TMC5160 commanded microstep counter: 3200 counts per motor revolution
with the current 200-step motor and 16-microstep setting. There is no encoder:
missed steps, stalls and movement while powered off cannot be measured.

## Wiring and use

This mode uses the user's **normally open** endstops: J400 lower, J401 upper.
Connect COM/NO across pin 1 (signal) and pin 2 (GND). Unpressed = open/high,
pressed = closed/low. Unlike NC wiring, a broken wire looks like an unpressed
switch and cannot be detected. Check both endstops manually before coupling.
The ordinary build still expects NC endstops; change wiring before reverting.

1. Release all handset controls after boot. There is no startup movement.
2. Hold DOWN until the lower switch is pressed. This establishes zero.
   Before referencing, slow UP/DOWN jogging is allowed but memories are disabled.
3. Hold UP/DOWN to move at at most 7.5 rpm; release or press both to stop.
4. While stationary, hold MEM1 or MEM2 for two seconds to store the step count.
   Tap/release to recall. Press either memory during recall to cancel; UP/DOWN
   overrides recall. Holding both memories stops and does not enter pairing.
5. Memories are RAM-only and clear at reset/power loss. Reference again and
   re-store them after restart. PD/SPI/driver faults or a two-minute continuous
   move timeout also discard the reference and memories; release all controls
   before jogging again.

Arrival tolerance is 16 microsteps (1/200 revolution), plus polling/stopping
latency. The ramp is stopped before the bridge is disabled so disabled ramp
steps are not mistaken for commanded movement. This still does not verify
physical shaft position. The mechanical transmission must hold without motor
holding torque. Endstops are software-polled, not independent emergency stops.

The blue LED lights for any pressed handset button, including when movement
is blocked; the red fault indication remains visible. On release, the blue LED
returns to its normal motion indication.

The 20 V / 3 A power gate stays active. The latest board log measured about
8.9 V, so power must be checked/restored separately. A red configuration fault
before referencing is expected; RTT logs show counts, reference, buttons,
limits and power state separately. No height in millimetres is inferred.

## Build

Inside the devcontainer, first configure sysbuild without compiling:

    west build -b table_lifter/nrf54l15/cpuapp --sysbuild app -d build/rotation --cmake-only -- -DBOARD_ROOT=/workspaces/table-lifter/app
    cmake -S app -B build/rotation/app -DTABLE_LIFTER_ROTATION_TEST=ON
    cmake --build build/rotation

Flash only the application using the existing bootloader:

    west flash -d build/rotation --domain app --runner jlink --dev-id host.docker.internal --dev-id-type ip --no-rebuild

Do not mix with TABLE_LIFTER_BENCH_TEST or TABLE_LIFTER_SPI_PROBE.
