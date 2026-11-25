package com.jasper.tablelifter;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.BluetoothStatusCodes;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanResult;
import android.content.Context;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayDeque;
import java.util.UUID;

/** BLE transport for the version 1 Table Lifter GATT protocol. */
final class TableBleClient {
    private static final UUID SERVICE = uuid(1);
    private static final UUID STATUS = uuid(2);
    private static final UUID COMMAND = uuid(3);
    private static final UUID CONFIG = uuid(4);
    private static final UUID DIAGNOSTICS = uuid(5);
    private static final UUID HISTORY = uuid(6);
    private static final UUID CCC = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");

    interface Listener {
        /** Report connection or command feedback. */
        void onMessage(String message);
        /** Report a decoded live status sample. */
        void onStatus(int heightMm, int distanceMm, int motion, int fault, int flags);
        /** Report stored preset heights, or null for an unset preset. */
        void onPresets(Integer firstMm, Integer secondMm);
        /** Report a readable hardware diagnostics summary. */
        void onDiagnostics(String summary);
    }

    private interface Operation { boolean start(); }

    private final Context context;
    private final Listener listener;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final ArrayDeque<Operation> pending = new ArrayDeque<>();
    private BluetoothLeScanner scanner;
    private BluetoothGatt gatt;
    private BluetoothGattCharacteristic statusChar;
    private BluetoothGattCharacteristic commandChar;
    private BluetoothGattCharacteristic configChar;
    private BluetoothGattCharacteristic diagnosticsChar;
    private BluetoothGattCharacteristic historyChar;
    private boolean operationActive;
    private boolean connected;
    private boolean scanning;
    private Runnable updateDisconnect;

    /** Construct a client whose callbacks run on the Android main thread. */
    TableBleClient(Context context, Listener listener) {
        this.context = context;
        this.listener = listener;
    }

    /** Build one custom service or characteristic UUID. */
    private static UUID uuid(int suffix) {
        return UUID.fromString(String.format("b545afe1-55f0-4e9c-8f96-5d458b50%04x", suffix));
    }

    /** Scan by advertised name because the board does not advertise its service UUID. */
    void scanAndConnect() {
        BluetoothAdapter adapter = context.getSystemService(BluetoothManager.class).getAdapter();
        if (adapter == null || !adapter.isEnabled()) {
            listener.onMessage("Turn on Bluetooth, then scan again.");
            return;
        }
        close();
        scanner = adapter.getBluetoothLeScanner();
        if (scanner == null) {
            listener.onMessage("Bluetooth scanning is unavailable.");
            return;
        }
        scanning = true;
        listener.onMessage("Scanning. Hold both memory buttons for 5 s before first pairing.");
        scanner.startScan(scanCallback);
        main.postDelayed(() -> {
            if (scanning) {
                stopScan();
                listener.onMessage("Table Lifter not found. Tap Scan to retry.");
            }
        }, 15000);
    }

    /** Stop any active scan without losing an established connection. */
    private void stopScan() {
        if (scanning && scanner != null) scanner.stopScan(scanCallback);
        scanning = false;
    }

    private final ScanCallback scanCallback = new ScanCallback() {
        /** Connect to the first device advertising the expected product name. */
        @Override public void onScanResult(int callbackType, ScanResult result) {
            main.post(() -> {
                if (!scanning) return;
                String name = result.getScanRecord() == null ? null :
                        result.getScanRecord().getDeviceName();
                if (!"Table Lifter".equals(name)) return;
                stopScan();
                listener.onMessage("Connecting to Table Lifter…");
                gatt = result.getDevice().connectGatt(context, false, gattCallback,
                                                       BluetoothDevice.TRANSPORT_LE);
            });
        }

        /** Surface a scan failure so the user can retry. */
        @Override public void onScanFailed(int errorCode) {
            main.post(() -> {
                scanning = false;
                listener.onMessage("BLE scan failed: " + errorCode);
            });
        }
    };

    /** Queue one GATT operation; Android permits only one outstanding operation. */
    private void enqueue(Operation operation) {
        pending.add(operation);
        startNext();
    }

    /** Start the next queued operation and discard operations that cannot start. */
    private void startNext() {
        while (!operationActive && !pending.isEmpty()) {
            operationActive = true;
            if (!pending.remove().start()) {
                operationActive = false;
                listener.onMessage("Bluetooth operation could not start.");
            }
        }
    }

    /** Complete an asynchronous GATT operation on the UI thread. */
    private void completed() {
        main.post(() -> {
            operationActive = false;
            startNext();
        });
    }

    /** Queue a read of one characteristic. */
    private void read(BluetoothGattCharacteristic characteristic) {
        enqueue(() -> gatt != null && characteristic != null && gatt.readCharacteristic(characteristic));
    }

    /** Queue a CCC write to receive live values. */
    private void subscribe(BluetoothGattCharacteristic characteristic) {
        if (gatt == null || characteristic == null) return;
        BluetoothGattDescriptor descriptor = characteristic.getDescriptor(CCC);
        if (descriptor == null || !gatt.setCharacteristicNotification(characteristic, true)) return;
        enqueue(() -> {
            if (gatt == null) return false;
            byte[] value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE;
            if (Build.VERSION.SDK_INT >= 33) {
                return gatt.writeDescriptor(descriptor, value) == BluetoothStatusCodes.SUCCESS;
            }
            descriptor.setValue(value);
            return gatt.writeDescriptor(descriptor);
        });
    }

    /** Queue a bonded command write, then refresh the live and preset values. */
    void command(byte[] payload) {
        if (!connected || gatt == null || commandChar == null ||
            gatt.getDevice().getBondState() != BluetoothDevice.BOND_BONDED) {
            listener.onMessage("Connect and bond first. Open pairing on the table with both memory buttons.");
            return;
        }
        enqueue(() -> {
            if (gatt == null) return false;
            if (Build.VERSION.SDK_INT >= 33) {
                return gatt.writeCharacteristic(commandChar, payload,
                        BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) == BluetoothStatusCodes.SUCCESS;
            }
            commandChar.setValue(payload);
            return gatt.writeCharacteristic(commandChar);
        });
    }

    /** Return the bonded device for a dedicated MCUmgr update connection. */
    BluetoothDevice bondedDevice() {
        if (!connected || gatt == null) return null;
        BluetoothDevice device = gatt.getDevice();
        return device.getBondState() == BluetoothDevice.BOND_BONDED ? device : null;
    }

    /** Disconnect normal controls before the single available BLE link is used for DFU. */
    void disconnectForUpdate(Runnable onDisconnected) {
        if (gatt == null) {
            main.post(onDisconnected);
            return;
        }
        updateDisconnect = onDisconnected;
        pending.clear();
        operationActive = false;
        connected = false;
        gatt.disconnect();
        main.postDelayed(() -> {
            if (updateDisconnect != onDisconnected) return;
            updateDisconnect = null;
            if (gatt != null) {
                gatt.close();
                gatt = null;
            }
            onDisconnected.run();
        }, 3000);
    }

    /** Release the scan and connection when the activity closes or reconnects. */
    void close() {
        stopScan();
        updateDisconnect = null;
        pending.clear();
        operationActive = false;
        connected = false;
        if (gatt != null) {
            gatt.disconnect();
            gatt.close();
            gatt = null;
        }
    }

    private final BluetoothGattCallback gattCallback = new BluetoothGattCallback() {
        /** Discover characteristics after connection, or reset UI after disconnect. */
        @Override public void onConnectionStateChange(BluetoothGatt source, int status, int newState) {
            main.post(() -> {
                if (source != gatt) return;
                if (updateDisconnect != null && newState == BluetoothProfile.STATE_DISCONNECTED) {
                    Runnable ready = updateDisconnect;
                    updateDisconnect = null;
                    source.close();
                    gatt = null;
                    main.postDelayed(ready, 200);
                    return;
                }
                if (updateDisconnect != null) return;
                if (status == BluetoothGatt.GATT_SUCCESS && newState == BluetoothProfile.STATE_CONNECTED) {
                    connected = true;
                    listener.onMessage("Connected. Discovering controls…");
                    if (source.getDevice().getBondState() != BluetoothDevice.BOND_BONDED)
                        source.getDevice().createBond();
                    source.discoverServices();
                } else {
                    connected = false;
                    pending.clear();
                    operationActive = false;
                    listener.onMessage("Disconnected. An accepted target move continues on the table.");
                    source.close();
                    gatt = null;
                }
            });
        }

        /** Resolve the versioned service and subscribe before initial reads. */
        @Override public void onServicesDiscovered(BluetoothGatt source, int status) {
            main.post(() -> {
                if (source != gatt || status != BluetoothGatt.GATT_SUCCESS) return;
                BluetoothGattService service = source.getService(SERVICE);
                if (service == null) {
                    listener.onMessage("Connected device has no Table Lifter service.");
                    return;
                }
                statusChar = service.getCharacteristic(STATUS);
                commandChar = service.getCharacteristic(COMMAND);
                configChar = service.getCharacteristic(CONFIG);
                diagnosticsChar = service.getCharacteristic(DIAGNOSTICS);
                historyChar = service.getCharacteristic(HISTORY);
                if (statusChar == null || commandChar == null || configChar == null) {
                    listener.onMessage("Firmware Bluetooth protocol is incomplete.");
                    return;
                }
                subscribe(statusChar);
                subscribe(diagnosticsChar);
                read(statusChar);
                read(configChar);
                read(diagnosticsChar);
                read(historyChar);
                listener.onMessage("Connected. Waiting for encrypted status and presets.");
            });
        }

        /** Continue the serialized queue after a notification subscription. */
        @Override public void onDescriptorWrite(BluetoothGatt source, BluetoothGattDescriptor descriptor,
                                                 int status) {
            if (status != BluetoothGatt.GATT_SUCCESS)
                main.post(() -> listener.onMessage("Notification setup failed; reconnect after pairing."));
            completed();
        }

        /** Decode a characteristic read using the current Android API. */
        @Override public void onCharacteristicRead(BluetoothGatt source,
                BluetoothGattCharacteristic characteristic, byte[] value, int status) {
            handleRead(characteristic, value, status);
        }

        /** Decode a characteristic read on Android 12 and earlier. */
        @Override public void onCharacteristicRead(BluetoothGatt source,
                BluetoothGattCharacteristic characteristic, int status) {
            if (Build.VERSION.SDK_INT < 33) handleRead(characteristic, characteristic.getValue(), status);
        }

        /** Decode a live notification on Android 13 and later. */
        @Override public void onCharacteristicChanged(BluetoothGatt source,
                BluetoothGattCharacteristic characteristic, byte[] value) {
            main.post(() -> decode(characteristic.getUuid(), value));
        }

        /** Decode a live notification on Android 12 and earlier. */
        @Override public void onCharacteristicChanged(BluetoothGatt source,
                BluetoothGattCharacteristic characteristic) {
            if (Build.VERSION.SDK_INT < 33)
                main.post(() -> decode(characteristic.getUuid(), characteristic.getValue()));
        }

        /** Report acknowledged commands and refresh values after a write. */
        @Override public void onCharacteristicWrite(BluetoothGatt source,
                BluetoothGattCharacteristic characteristic, int status) {
            main.post(() -> {
                listener.onMessage(status == BluetoothGatt.GATT_SUCCESS ?
                        "Command accepted by Bluetooth stack." : "Command rejected: GATT " + status);
                if (status == BluetoothGatt.GATT_SUCCESS) {
                    read(statusChar);
                    read(configChar);
                }
                operationActive = false;
                startNext();
            });
        }
    };

    /** Decode one read and release the GATT queue. */
    private void handleRead(BluetoothGattCharacteristic characteristic, byte[] value, int status) {
        main.post(() -> {
            if (status == BluetoothGatt.GATT_SUCCESS) decode(characteristic.getUuid(), value);
            else listener.onMessage("Read failed: GATT " + status + ". Check pairing window.");
            operationActive = false;
            startNext();
        });
    }

    /** Parse status, configuration and hardware records from the board. */
    private void decode(UUID id, byte[] value) {
        if (value == null || value.length < 2 || value[0] != 1) return;
        ByteBuffer data = ByteBuffer.wrap(value).order(ByteOrder.LITTLE_ENDIAN);
        if (STATUS.equals(id) && value.length == 16) {
            listener.onStatus(data.getInt(8), data.getInt(4), value[1] & 0xff,
                    value[2] & 0xff, value[3] & 0xff);
        } else if (CONFIG.equals(id) && value.length == 25) {
            int bits = data.getInt(1);
            int offset = data.getInt(5);
            listener.onPresets((bits & 1) != 0 ? data.getInt(9) + offset : null,
                    (bits & 2) != 0 ? data.getInt(13) + offset : null);
        } else if (DIAGNOSTICS.equals(id) && value.length == 20) {
            int flags = value[1] & 0xff;
            int rangeStatus = value[2];
            int ageMs = (value[3] & 0xff) * 10;
            int rawMm = data.getShort(4) & 0xffff;
            int busMv = data.getShort(6) & 0xffff;
            long rdo = data.getInt(8) & 0xffffffffL;
            long drv = data.getInt(16) & 0xffffffffL;
            int pdo = (int)((rdo >> 28) & 7);
            int currentMa = (int)((rdo & 0x3ff) * 10);
            String warnings = (drv & (1L << 26)) != 0 ? "TMC temperature warning" : "TMC no temperature warning";
            listener.onDiagnostics(String.format("Bus %.2f V · PD PDO %d / %d mA (%s) · alert %d\nLiDAR raw %d mm · status %d · age %d ms\nTMC GSTAT 0x%08X · DRV 0x%08X · DIAG0 %d · DIAG1 %d · %s",
                    busMv / 1000.0, pdo, currentMa, (flags & 1) != 0 ? "valid" : "invalid",
                    (flags & 4) != 0 ? 1 : 0, rawMm, rangeStatus, ageMs,
                    data.getInt(12), data.getInt(16), (flags & 8) != 0 ? 1 : 0,
                    (flags & 16) != 0 ? 1 : 0, warnings));
        } else if (HISTORY.equals(id) && value.length == 22) {
            int count = Math.min(value[1] & 0xff, 4);
            if (count > 0) listener.onMessage("Last fault: code " + (value[6] & 0xff) +
                    " at uptime " + data.getInt(2) + " s (" + count + " stored)");
        }
    }
}
