# Handset and endstop wiring

Disconnect USB-C and any bench supply before soldering or changing motor wiring.
Use connector pin numbers from the PCB/pin-1 marking; cable-side and board-side
views mirror each other. Do not infer pin 1 from wire color or a left/right description.

## P300 — handset (JST GH, GHR-05V-S)

| Pin | Signal | Connect to |
| --- | --- | --- |
| 1 | UP | Rocker throw that closes when pressed UP |
| 2 | DOWN | Other rocker throw |
| 3 | MEM1 | Memory button 1 normally open contact |
| 4 | MEM2 | Memory button 2 normally open contact |
| 5 | GND | Rocker common and both memory-button commons |

For the selected C&K 7105J51ZBE22, terminal **2 is common**; terminals 1 and 3
are the two throws. This is momentary–OFF–momentary. Connect 2 to P300-5.
Use a continuity meter to determine which throw closes in your mounted UP
orientation, then connect that throw to P300-1 and the other to P300-2.
In the centre position neither throw should connect to common.

For each NKK EB2011G, connect terminal **2 (COM) to P300-5**, terminal
**1 (closes when pressed) to P300-3 or P300-4**. Leave terminal 3 unconnected
and insulated: it is closed to common when released. Numbers may not be marked
on the actual switch; identify COM and the press-only contact with continuity
before soldering. Do not assume the middle physical lug is COM.

All contacts switch to ground. No external 3.3 V or 20 V wire belongs on these
switches; the board supplies input pull-ups. Insulate solder lugs individually
and strain-relieve the cable.

## J400 / J401 — travel limits (JST GH, GHR-02V-S)

| Connector | Pin 1 | Pin 2 |
| --- | --- | --- |
| J400 | Lower limit signal | GND |
| J401 | Upper limit signal | GND |

Connect each switch's **COM and NC** across its two pins; leave NO unused.
Away from the limit there must be continuity. At the travel limit the circuit
must open. Firmware treats a high input or GPIO read error as active.
J400 inhibits DOWN; J401 inhibits UP; retreat remains possible. A disconnected
switch also inhibits its direction. Both must be wired for normal two-way motion.
Set the mechanical trip points before the hard stops and check stopping clearance
at the chosen speed. These inputs are polled in the main loop, not independent
hardware emergency stops.

## Controls

- Hold UP/DOWN to move; release to stop. Both pressed stops.
- Hold MEM1 or MEM2 for two seconds to store the current floor distance.
- Tap and release a memory button to recall. Press either memory button while
  moving automatically to cancel. UP/DOWN overrides automatic motion.
- Hold both memory buttons for five seconds to open Bluetooth pairing.
- Valid floor-distance readings and the 20 V / 3 A contract are required.
- Stored positions survive power loss when flash storage succeeds. Test this on
  the board; the simulated controller tests do not validate physical flash.

## P400 — motor reference

| Pin | Winding terminal | Selected lead |
| --- | --- | --- |
| 1 | B2 | Blue |
| 2 | B1 | Red |
| 3 | A2 | Green |
| 4 | A1 | Black |

Black/green and red/blue were measured as winding pairs on this motor.
Check actual table direction with a brief unloaded UP command. If reversed,
power off and exchange the two wires of one winding only; then recheck limits.

## Drawing sources

Board connector assignments: Table-Stepper schematic export dated 22 November
2026, MCU and TMC sheets; firmware board DTS.
Switch terminal functions:
[C&K 7000 series](https://www.littelfuse.com/assetdocs/littelfuse-ck-rocker-7000-series-datasheet?assetguid=58e0b968-377d-4b5d-b458-2488d5eb9b1f),
[NKK EB circuit table](https://www.nkkswitches.com/pdf/pushbuttons.pdf).
