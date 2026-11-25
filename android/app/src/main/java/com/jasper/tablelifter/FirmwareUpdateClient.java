package com.jasper.tablelifter;

import android.bluetooth.BluetoothDevice;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;

import no.nordicsemi.android.mcumgr.ble.McuMgrBleTransport;
import no.nordicsemi.android.mcumgr.dfu.FirmwareUpgradeCallback;
import no.nordicsemi.android.mcumgr.dfu.FirmwareUpgradeController;
import no.nordicsemi.android.mcumgr.dfu.mcuboot.FirmwareUpgradeManager;
import no.nordicsemi.android.mcumgr.dfu.mcuboot.model.ImageSet;
import no.nordicsemi.android.mcumgr.dfu.mcuboot.model.TargetImage;
import no.nordicsemi.android.mcumgr.exception.McuMgrException;

/** Owns a dedicated Nordic MCUmgr BLE connection during one firmware update. */
final class FirmwareUpdateClient implements FirmwareUpgradeCallback<FirmwareUpgradeManager.State> {
    interface Listener {
        /** Show the current upgrade phase. */
        void onPhase(String phase);
        /** Show acknowledged upload progress. */
        void onProgress(int percent);
        /** Report successful test, reboot, and confirmation. */
        void onComplete();
        /** Report an upload cancelled by the user. */
        void onCancelled();
        /** Report a transfer or boot confirmation failure. */
        void onFailure(String phase, String error);
    }

    private final Context context;
    private final Listener listener;
    private final Handler main = new Handler(Looper.getMainLooper());
    private McuMgrBleTransport transport;
    private FirmwareUpgradeManager manager;
    private boolean active;

    /** Bind one update session to the application and UI. */
    FirmwareUpdateClient(Context context, Listener listener) {
        this.context = context;
        this.listener = listener;
    }

    /** Begin a single-core test, reboot, and confirm upgrade on a bonded device. */
    void start(BluetoothDevice device, byte[] signedImage) {
        if (active) return;
        active = true;
        try {
            transport = new McuMgrBleTransport(context, device);
            manager = new FirmwareUpgradeManager(transport, this);
            manager.setMode(FirmwareUpgradeManager.Mode.TEST_AND_CONFIRM);
            FirmwareUpgradeManager.Settings settings = new FirmwareUpgradeManager.Settings.Builder()
                    .setWindowCapacity(1)
                    .setEstimatedSwapTime(8000)
                    .setEraseAppSettings(false)
                    .build();
            ImageSet images = new ImageSet().add(new TargetImage(0, signedImage));
            manager.start(images, settings);
        } catch (Exception error) {
            close();
            listener.onFailure("START", error.getMessage());
        }
    }

    /** Cancel only while bytes are being uploaded. */
    void cancel() {
        if (manager != null && manager.getState() == FirmwareUpgradeManager.State.UPLOAD)
            manager.cancel();
    }

    /** Release the MCUmgr transport and its callback. */
    void close() {
        if (manager != null) manager.setFirmwareUpgradeCallback(null);
        if (transport != null) transport.release();
        manager = null;
        transport = null;
        active = false;
    }

    /** Report the initial validation phase. */
    @Override public void onUpgradeStarted(FirmwareUpgradeController controller) {
        main.post(() -> listener.onPhase("Validating image and board state…"));
    }

    /** Report upload, test, reboot, and confirmation phases. */
    @Override public void onStateChanged(FirmwareUpgradeManager.State previous,
                                         FirmwareUpgradeManager.State current) {
        main.post(() -> listener.onPhase(switch (current) {
            case UPLOAD -> "Uploading signed image…";
            case TEST -> "Scheduling test boot…";
            case RESET -> "Rebooting table controller…";
            case CONFIRM -> "Confirming new firmware…";
            default -> current.name();
        }));
    }

    /** Report bytes acknowledged by the target. */
    @Override public void onUploadProgressChanged(int sent, int size, long timestamp) {
        int percent = size == 0 ? 0 : Math.min(100, sent * 100 / size);
        main.post(() -> listener.onProgress(percent));
    }

    /** Release the transport after the new image is confirmed. */
    @Override public void onUpgradeCompleted() {
        main.post(() -> {
            close();
            listener.onComplete();
        });
    }

    /** Release the transport after a user cancellation. */
    @Override public void onUpgradeCanceled(FirmwareUpgradeManager.State state) {
        main.post(() -> {
            close();
            listener.onCancelled();
        });
    }

    /** Release the transport and retain the failing phase for recovery guidance. */
    @Override public void onUpgradeFailed(FirmwareUpgradeManager.State state, McuMgrException error) {
        main.post(() -> {
            close();
            listener.onFailure(state.name(), error.getMessage());
        });
    }
}
