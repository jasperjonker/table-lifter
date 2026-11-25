package com.jasper.tablelifter;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.bluetooth.BluetoothDevice;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.text.InputType;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.ArrayAdapter;
import android.widget.Switch;
import android.widget.TextView;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.Locale;

import no.nordicsemi.android.mcumgr.image.McuMgrImage;
import no.nordicsemi.android.mcumgr.image.tlv.McuMgrImageTlv;
import no.nordicsemi.android.mcumgr.image.tlv.McuMgrImageTlvTrailerEntry;

/** Simple local control screen for one bonded Table Lifter. */
public final class MainActivity extends Activity implements TableBleClient.Listener,
        FirmwareUpdateClient.Listener {
    private static final int PERMISSIONS_REQUEST = 12;
    private static final int PICK_IMAGE_REQUEST = 20;
    private static final int MAX_IMAGE_BYTES = 729_088;
    private TableBleClient client;
    private FirmwareUpdateClient updateClient;
    private TextView connectionView;
    private TextView heightView;
    private TextView stateView;
    private TextView diagnosticsView;
    private Button presetOne;
    private Button presetTwo;
    private Button moveButton;
    private Switch quietSwitch;
    private Switch remindersSwitch;
    private Spinner intervalSpinner;
    private EditText targetInput;
    private Button updateButton;
    private Button cancelUpdateButton;
    private ProgressBar updateProgress;
    private TextView updateState;
    private int lastMotion = -1;
    private boolean lastMaintenance;
    private boolean updateBusy;
    private boolean cancelLockAfterReconnect;

    /** One locally selected, structurally valid signed MCUboot image. */
    private static final class SelectedImage {
        final byte[] data;
        final String name;
        final String version;
        final String hashPrefix;

        /** Keep image metadata together with the bytes shown in confirmation. */
        SelectedImage(byte[] data, String name, String version, String hashPrefix) {
            this.data = data;
            this.name = name;
            this.version = version;
            this.hashPrefix = hashPrefix;
        }
    }

    /** Build the screen and restore local reminder preferences. */
    @Override protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        client = new TableBleClient(this, this);
        updateClient = new FirmwareUpdateClient(this, this);
        ScrollView scroll = new ScrollView(this);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        int padding = (int)(20 * getResources().getDisplayMetrics().density);
        page.setPadding(padding, padding, padding, padding);
        scroll.addView(page);
        setContentView(scroll);

        title(page, "Table Lifter");
        connectionView = label(page, "Disconnected");
        button(page, "Scan and connect", () -> scanWithPermissions());
        label(page, "For first pairing, hold both memory buttons on the table for 5 seconds, then scan.");

        title(page, "Height");
        heightView = label(page, "—");
        stateView = label(page, "Waiting for sensor data");
        targetInput = new EditText(this);
        targetInput.setHint("Target tabletop height (mm)");
        targetInput.setInputType(InputType.TYPE_CLASS_NUMBER);
        page.addView(targetInput);
        moveButton = button(page, "Move to height", this::moveToHeight);
        button(page, "Stop", () -> client.command(command(4)));

        title(page, "Memories");
        label(page, "Tap to recall. Long press to store the current height.");
        presetOne = button(page, "Memory 1 — unset", () -> client.command(command(2, (byte)0)));
        presetTwo = button(page, "Memory 2 — unset", () -> client.command(command(2, (byte)1)));
        presetOne.setOnLongClickListener(v -> { client.command(command(3, (byte)0)); return true; });
        presetTwo.setOnLongClickListener(v -> { client.command(command(3, (byte)1)); return true; });

        title(page, "Motion profile");
        quietSwitch = new Switch(this);
        quietSwitch.setText("Quiet speed (15 rpm maximum)");
        quietSwitch.setOnClickListener(v -> client.command(command(7,
                (byte)(quietSwitch.isChecked() ? 1 : 0))));
        page.addView(quietSwitch);

        title(page, "Firmware update");
        label(page, "Select the signed zephyr.signed.bin from your board build. Keep the phone close and this screen open.");
        updateButton = button(page, "Choose signed firmware", this::chooseFirmware);
        updateButton.setEnabled(false);
        updateProgress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        updateProgress.setMax(100);
        page.addView(updateProgress);
        updateState = label(page, "Connect to an idle table before updating.");
        cancelUpdateButton = button(page, "Cancel upload", updateClient::cancel);
        cancelUpdateButton.setEnabled(false);

        title(page, "Sit / stand reminder");
        label(page, "Phone notification only. A reminder never moves the table.");
        intervalSpinner = new Spinner(this);
        String[] intervals = {"Every 30 minutes", "Every 60 minutes", "Every 90 minutes"};
        intervalSpinner.setAdapter(new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_dropdown_item, intervals));
        intervalSpinner.setSelection(getPreferences(MODE_PRIVATE).getInt("interval", 1));
        page.addView(intervalSpinner);
        remindersSwitch = new Switch(this);
        remindersSwitch.setText("Enable reminders");
        remindersSwitch.setChecked(getPreferences(MODE_PRIVATE).getBoolean("reminders", false));
        remindersSwitch.setOnClickListener(v -> updateReminder());
        page.addView(remindersSwitch);
        button(page, "Apply reminder interval", this::updateReminder);

        title(page, "Diagnostics");
        diagnosticsView = label(page, "Connect to read the live hardware snapshot.");
        button(page, "Cancel incomplete firmware upload", () -> client.command(command(8)));
    }

    /** Release Bluetooth resources while leaving scheduled reminders active. */
    @Override protected void onDestroy() {
        updateClient.close();
        client.close();
        super.onDestroy();
    }

    /** Open Android's document picker for a signed MCUboot image. */
    private void chooseFirmware() {
        if (updateBusy || lastMotion != 0 || lastMaintenance || client.bondedDevice() == null) {
            onMessage("Connect and bond to an idle table before updating.");
            return;
        }
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        pick.addCategory(Intent.CATEGORY_OPENABLE);
        pick.setType("*/*");
        startActivityForResult(pick, PICK_IMAGE_REQUEST);
    }

    /** Validate a picked file before showing the irreversible reboot step. */
    @Override protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_IMAGE_REQUEST || resultCode != RESULT_OK || data == null ||
            data.getData() == null) return;
        Uri uri = data.getData();
        updateState.setText("Reading and validating image…");
        updateButton.setEnabled(false);
        new Thread(() -> {
            try {
                SelectedImage image = readImage(uri);
                runOnUiThread(() -> {
                    if (!isFinishing()) showImageConfirmation(image);
                });
            } catch (Exception error) {
                runOnUiThread(() -> {
                    if (isFinishing()) return;
                    updateButton.setEnabled(true);
                    updateState.setText("Image rejected: " + error.getMessage());
                });
            }
        }, "firmware-file-reader").start();
    }

    /** Read a bounded document and require an MCUboot ECDSA-P256 signature TLV. */
    private SelectedImage readImage(Uri uri) throws Exception {
        String name = "selected image";
        try (Cursor cursor = getContentResolver().query(uri,
                new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) name = cursor.getString(0);
        }
        if (name == null || !name.endsWith(".bin"))
            throw new IOException("Choose zephyr.signed.bin, not a HEX or ZIP file.");
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        try (InputStream input = getContentResolver().openInputStream(uri)) {
            if (input == null) throw new IOException("The selected file cannot be opened.");
            byte[] buffer = new byte[8192];
            int count;
            while ((count = input.read(buffer)) != -1) {
                if (output.size() + count > MAX_IMAGE_BYTES)
                    throw new IOException("The image is larger than the board's OTA slot.");
                output.write(buffer, 0, count);
            }
        }
        byte[] bytes = output.toByteArray();
        McuMgrImage parsed = McuMgrImage.fromBytes(bytes);
        boolean signed = false;
        for (McuMgrImageTlvTrailerEntry entry : parsed.getTlv().getTrailerEntries()) {
            if ((entry.type & 0xff) == McuMgrImageTlv.IMG_TLV_ECDSA256 && entry.length > 0)
                signed = true;
        }
        if (!signed) throw new IOException("Image has no ECDSA-P256 signature. Build with MCUboot.");
        StringBuilder hash = new StringBuilder();
        for (int i = 0; i < 8; ++i) hash.append(String.format(Locale.ROOT,
                "%02x", parsed.getHash()[i] & 0xff));
        return new SelectedImage(bytes, name, parsed.getHeader().getVersion().toString(),
                hash.toString());
    }

    /** Ask for confirmation after showing the selected image's version and hash. */
    private void showImageConfirmation(SelectedImage image) {
        updateButton.setEnabled(true);
        updateState.setText("Ready: " + image.name + " (" + image.version + ")");
        new AlertDialog.Builder(this)
                .setTitle("Update table firmware?")
                .setMessage(image.name + "\nVersion: " + image.version +
                        "\nSize: " + image.data.length + " bytes\nImage hash: " +
                        image.hashPrefix + "…\n\nThe table must be idle. Keep it powered and keep this screen open until confirmation completes.")
                .setNegativeButton("Not now", null)
                .setPositiveButton("Update", (dialog, which) -> beginUpdate(image))
                .show();
    }

    /** Hand the bonded device to MCUmgr after closing the normal control link. */
    private void beginUpdate(SelectedImage image) {
        BluetoothDevice device = client.bondedDevice();
        if (device == null || lastMotion != 0 || lastMaintenance || updateBusy) {
            updateState.setText("Table is not connected, bonded, and idle. Retry after it stops.");
            return;
        }
        updateBusy = true;
        updateButton.setEnabled(false);
        moveButton.setEnabled(false);
        presetOne.setEnabled(false);
        presetTwo.setEnabled(false);
        quietSwitch.setEnabled(false);
        updateProgress.setProgress(0);
        updateState.setText("Closing control link for firmware transfer…");
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        client.disconnectForUpdate(() -> updateClient.start(device, image.data));
    }

    /** Return to controls after the update transport has released the radio. */
    private void finishUpdate(boolean reconnect) {
        updateBusy = false;
        cancelUpdateButton.setEnabled(false);
        getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (reconnect) new android.os.Handler(getMainLooper()).postDelayed(
                this::scanWithPermissions, 1500);
    }

    /** Add a section title to the screen. */
    private void title(LinearLayout page, String text) {
        TextView view = label(page, text);
        view.setTextSize(22);
        view.setPadding(0, 20, 0, 8);
    }

    /** Add a text label to the screen. */
    private TextView label(LinearLayout page, String text) {
        TextView view = new TextView(this);
        view.setText(text);
        view.setTextSize(16);
        view.setPadding(0, 4, 0, 8);
        page.addView(view);
        return view;
    }

    /** Add a full-width action button to the screen. */
    private Button button(LinearLayout page, String text, Runnable action) {
        Button view = new Button(this);
        view.setText(text);
        view.setOnClickListener(v -> action.run());
        page.addView(view);
        return view;
    }

    /** Request Android's Nearby Devices permission before scanning. */
    private void scanWithPermissions() {
        if (Build.VERSION.SDK_INT >= 31) {
            if (checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) != PackageManager.PERMISSION_GRANTED ||
                checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
                requestPermissions(new String[]{Manifest.permission.BLUETOOTH_SCAN,
                        Manifest.permission.BLUETOOTH_CONNECT}, PERMISSIONS_REQUEST);
                return;
            }
        } else if (checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) !=
                   PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.ACCESS_FINE_LOCATION}, PERMISSIONS_REQUEST);
            return;
        }
        client.scanAndConnect();
    }

    /** Continue scanning after the runtime permission dialog. */
    @Override public void onRequestPermissionsResult(int requestCode, String[] permissions,
                                                       int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == PERMISSIONS_REQUEST) {
            if (grantResults.length == 0) {
                onMessage("Bluetooth permission request cancelled.");
                return;
            }
            for (int result : grantResults) {
                if (result != PackageManager.PERMISSION_GRANTED) {
                    onMessage("Bluetooth permission denied. Grant Nearby Devices to connect.");
                    return;
                }
            }
            scanWithPermissions();
        }
    }

    /** Encode a version 1 command with optional one-byte argument. */
    private byte[] command(int opcode, byte... payload) {
        ByteBuffer data = ByteBuffer.allocate(2 + payload.length).order(ByteOrder.LITTLE_ENDIAN);
        data.put((byte)1).put((byte)opcode).put(payload);
        return data.array();
    }

    /** Validate and send a calibrated target tabletop height. */
    private void moveToHeight() {
        try {
            int height = Integer.parseInt(targetInput.getText().toString());
            if (height < 40 || height > 5000) throw new NumberFormatException();
            ByteBuffer data = ByteBuffer.allocate(6).order(ByteOrder.LITTLE_ENDIAN);
            client.command(data.put((byte)1).put((byte)1).putInt(height).array());
        } catch (NumberFormatException error) {
            onMessage("Enter a target height from 40 to 5000 mm.");
        }
    }

    /** Save and reschedule an inexact phone notification interval. */
    private void updateReminder() {
        boolean enabled = remindersSwitch.isChecked();
        int choice = intervalSpinner.getSelectedItemPosition();
        getPreferences(MODE_PRIVATE).edit().putBoolean("reminders", enabled)
                .putInt("interval", choice).apply();
        ReminderReceiver.schedule(this);
        if (enabled && Build.VERSION.SDK_INT >= 33 &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, 13);
        }
        onMessage(enabled ? "Reminders scheduled on this phone." : "Reminders disabled.");
    }

    /** Show connection and command feedback. */
    @Override public void onMessage(String message) { connectionView.setText(message); }

    /** Show the latest height and lock/motion state. */
    @Override public void onStatus(int heightMm, int distanceMm, int motion, int fault, int flags) {
        boolean valid = (flags & 1) != 0;
        boolean maintenance = (flags & 32) != 0;
        lastMotion = motion;
        lastMaintenance = maintenance;
        heightView.setText(valid ? String.format(Locale.getDefault(), "%.1f cm", heightMm / 10.0) :
                "No valid distance");
        String[] motions = {"idle", "up", "down", "automatic"};
        String[] faults = {"none", "USB-PD", "LiDAR", "motor driver", "configuration"};
        stateView.setText("Motion: " + motions[Math.min(motion, 3)] +
                " · Fault: " + faults[Math.min(fault, 4)] +
                " · Floor distance: " + distanceMm + " mm" +
                (maintenance ? " · UPDATE LOCK" : ""));
        quietSwitch.setChecked((flags & 16) != 0);
        quietSwitch.setEnabled(!maintenance && !updateBusy);
        moveButton.setEnabled(!maintenance && !updateBusy);
        presetOne.setEnabled(!maintenance && !updateBusy);
        presetTwo.setEnabled(!maintenance && !updateBusy);
        updateButton.setEnabled(!maintenance && !updateBusy && motion == 0 &&
                client.bondedDevice() != null);
        if (cancelLockAfterReconnect) {
            cancelLockAfterReconnect = false;
            if (maintenance) client.command(command(8));
        }
    }

    /** Show stored preset heights from the encrypted config record. */
    @Override public void onPresets(Integer firstMm, Integer secondMm) {
        presetOne.setText(firstMm == null ? "Memory 1 — unset" : "Memory 1 — " + firstMm + " mm");
        presetTwo.setText(secondMm == null ? "Memory 2 — unset" : "Memory 2 — " + secondMm + " mm");
    }

    /** Show the latest raw sensor and motor diagnostics. */
    @Override public void onDiagnostics(String summary) { diagnosticsView.setText(summary); }

    /** Show the library's current MCUboot update phase. */
    @Override public void onPhase(String phase) {
        updateState.setText(phase);
        cancelUpdateButton.setEnabled(phase.startsWith("Uploading"));
    }

    /** Update the progress bar after an acknowledged image chunk. */
    @Override public void onProgress(int percent) { updateProgress.setProgress(percent); }

    /** Reconnect to normal controls after test boot and confirmation succeed. */
    @Override public void onComplete() {
        updateProgress.setProgress(100);
        updateState.setText("Firmware updated and confirmed. Reconnecting to controls…");
        finishUpdate(true);
    }

    /** Reconnect and clear the incomplete-upload maintenance lock. */
    @Override public void onCancelled() {
        cancelLockAfterReconnect = true;
        updateState.setText("Upload cancelled. Reconnecting to clear maintenance lock…");
        finishUpdate(true);
    }

    /** Explain whether a failed update may need the maintenance lock cleared. */
    @Override public void onFailure(String phase, String error) {
        cancelLockAfterReconnect = phase.equals("UPLOAD");
        updateState.setText("Update failed during " + phase + ": " + error +
                ". Reconnecting; check the board before retrying.");
        finishUpdate(true);
    }
}
