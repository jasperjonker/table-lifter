# Board commissioning

Unloaded rotation, SWD programming and PD provisioning worked on 24–25 November
2026. The normal control image adds handset operation and normally closed endstops.
Keep the motor uncoupled for the initial direction and switch checks.

Capture the [RTT logs](rtt.md) during these checks and record the first fault
code and PD/range state if motion is inhibited.

1. Verify P2.00 enable polarity, TMC SPI register reads, current, motor winding
   pairs, and UP direction. Confirm 1 A initial current with a current probe.
2. With a PD source rated 20 V / 3 A, run `pd provision 20v3a` once over RTT.
   Power cycle. Verify a nonzero RDO source position (5 on the tested charger), requested 3 A, no capability mismatch, and V_MOTOR about
   20 V at the P1.06 ADC divider. Unplug or downgrade PD while jogging and
   confirm motion stops.
3. Check LiDAR range status and distance over the actual floor surface across
   the full travel. Confirm the sensor points down and increasing distance is
   UP. Determine the sensor-to-tabletop offset and set it using Bluetooth.
4. Measure the actual minimum and maximum tabletop heights and configure them.
   The suggested 1–2 m figures are placeholders and are **not** compiled as
   limits. Validate both manual and automatic boundary stops.
5. Test memory hold, recall, interruption, ±20 mm arrival band, pairing window,
   encrypted writes, signed OTA, failed-image rollback, and settings recovery.
6. Under load, tune current and speed while measuring driver temperature,
   motor temperature, and supply sag. Confirm the mechanical drive is
   self-locking with driver outputs disabled.

The board uses nRF54L15 QGAA; P1.02/P1.03 are LED GPIOs and NFC is disabled.
Wire the normally closed lower/upper endstops as described in [wiring](wiring.md).
Check each by hand before coupling: opening J400 must stop DOWN and allow UP;
opening J401 must stop UP and allow DOWN. Both open must block all motion.
An interrupted preset must not restart when the switch closes.
These are software-polled stops, not an independent safety circuit.
If the LiDAR becomes invalid, motion stops. Verify release-to-stop, both directions,
both presets and power-cycle retention before loaded travel. The drive disables
motor outputs when stopping, so confirm the SKARSTA transmission holds the load
without motor holding torque. Do not use this control on a backdrivable load.
