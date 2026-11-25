# Android control app

Open `android/` in Android Studio. Use JDK 17 and install Android SDK 37.0.
The project uses Android Gradle Plugin 9.4.0 and the verified Gradle 9.6.0
wrapper. Build the `app` debug variant and install it on an Android 8+ phone
with Bluetooth LE. The app has no cloud account or internet permission.

For first pairing, hold both table memory buttons for five seconds to open the
one-minute pairing window, then tap **Scan and connect**. Grant Nearby Devices
permission on Android 12+, or Location on older Android. The table has no
passkey display, so pairing uses Bluetooth Just Works with the physical button
gesture as authorization. Accept the system pairing prompt. Existing bonds
reconnect without the gesture.

The screen shows live tabletop height, motion/fault state, the two memories,
and raw hardware diagnostics. Tap a memory to recall it; long-press to store
the current valid position. **Move to height** sends millimetres after the
floor-sensor offset has been calibrated. **Stop** interrupts motion. Quiet mode
limits the motor to 15 rpm and 5 rpm/s acceleration, with a slower approach
near the target; normal mode remains 30 rpm and 10 rpm/s. Hardware current is
unchanged. The quiet choice is stored on the board.

The optional 30/60/90-minute sit/stand reminders are local, inexact phone
notifications and never send a movement command. Android 13+ also asks for
notification permission. The reminder schedule survives a phone reboot but
may be delayed by Android battery management. The app does not need to stay
connected for reminders or for an accepted target move.

## Bluetooth firmware updates

1. Build the board in the devcontainer with **Zephyr: board build**. For each
   new release, increase the version in `app/VERSION` before building. The
   signed application image is `build/board/app/zephyr/zephyr.signed.bin`.
   Copy that file to your phone. Do not select `zephyr.bin`, a HEX file, or
   an image signed with a different key. Keep the signing key backed up and
   outside this repository.
2. Power the board from a stable supply and connect to it in the Android app.
   It must be bonded, idle, and outside OTA maintenance. Tap **Choose signed
   firmware**, select the `.bin`, and check the displayed image version, size,
   and hash prefix. Tap **Update**.
3. Leave the phone near the board and keep the app open. The app closes its
   control connection, uploads through Nordic's Bluetooth MCUmgr transport,
   requests a test boot, waits for reboot, and confirms the new image. It then
   reconnects to the table controls. The progress bar reflects acknowledged
   upload bytes; testing, reboot, and confirmation take additional time.

**Cancel upload** is available while bytes are transferring. The app then
reconnects and requests release of an incomplete-upload maintenance lock.
If the app is closed or the link fails, reconnect and use **Cancel incomplete
firmware upload** before retrying. After a complete upload, only reboot clears
the lock. If the new firmware fails to boot or confirm, MCUboot should revert
to the previous image; verify this on the real board before relying on OTA.

The app checks the MCUboot image format and presence of an ECDSA-P256
signature; it does not authenticate the signing key itself. MCUboot verifies
the signature at boot. This updates the application image, not MCUboot. The
app uses Nordic Android MCUmgr 3.4.1. Read [Bluetooth protocol](bluetooth.md)
and [commissioning](commissioning.md) before moving a coupled table.

Android APIs: [Bluetooth permissions](https://developer.android.com/develop/connectivity/bluetooth/bt-permissions),
[notification permission](https://developer.android.com/develop/ui/views/notifications/notification-permission),
[Nordic Android MCUmgr](https://github.com/NordicSemiconductor/Android-nRF-Connect-Device-Manager),
and [AGP 9.4 compatibility](https://developer.android.com/build/releases/agp-9-4-0-release-notes).
