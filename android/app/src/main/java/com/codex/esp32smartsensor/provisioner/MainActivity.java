package com.codex.esp32smartsensor.provisioner;

import android.Manifest;
import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanFilter;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelUuid;
import android.text.InputType;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Queue;
import java.util.UUID;

public class MainActivity extends Activity {
    private static final UUID SERVICE_UUID = UUID.fromString("9e9a0001-6f3d-4f57-9e9f-8c2b9a5f1000");
    private static final UUID DEVICE_NAME_UUID = UUID.fromString("9e9a0002-6f3d-4f57-9e9f-8c2b9a5f1000");
    private static final UUID WIFI_SSID_UUID = UUID.fromString("9e9a0003-6f3d-4f57-9e9f-8c2b9a5f1000");
    private static final UUID WIFI_PASSWORD_UUID = UUID.fromString("9e9a0004-6f3d-4f57-9e9f-8c2b9a5f1000");
    private static final UUID MQTT_HOST_UUID = UUID.fromString("9e9a0005-6f3d-4f57-9e9f-8c2b9a5f1000");
    private static final UUID MQTT_PORT_UUID = UUID.fromString("9e9a0006-6f3d-4f57-9e9f-8c2b9a5f1000");
    private static final UUID APPLY_UUID = UUID.fromString("9e9a0007-6f3d-4f57-9e9f-8c2b9a5f1000");
    private static final int REQUEST_BLE_PERMISSIONS = 42;

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final Map<String, BluetoothDevice> devices = new LinkedHashMap<>();
    private final Queue<PendingWrite> writeQueue = new ArrayDeque<>();

    private BluetoothAdapter bluetoothAdapter;
    private BluetoothLeScanner scanner;
    private BluetoothGatt gatt;
    private boolean scanning = false;
    private boolean writeInFlight = false;
    private BluetoothDevice selectedDevice;

    private LinearLayout deviceList;
    private TextView statusText;
    private EditText deviceNameInput;
    private EditText ssidInput;
    private EditText passwordInput;
    private EditText mqttHostInput;
    private EditText mqttPortInput;

    private final ScanCallback scanCallback = new ScanCallback() {
        @Override
        public void onScanResult(int callbackType, ScanResult result) {
            BluetoothDevice device = result.getDevice();
            if (device == null) {
                return;
            }

            String address = device.getAddress();
            if (!devices.containsKey(address)) {
                devices.put(address, device);
                mainHandler.post(MainActivity.this::renderDeviceList);
            }
        }

        @Override
        public void onScanFailed(int errorCode) {
            mainHandler.post(() -> setStatus("Scan failed: " + errorCode));
        }
    };

    private final BluetoothGattCallback gattCallback = new BluetoothGattCallback() {
        @Override
        public void onConnectionStateChange(BluetoothGatt bluetoothGatt, int status, int newState) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                mainHandler.post(() -> setStatus("Connection failed: " + status));
                closeGatt();
                return;
            }

            if (newState == BluetoothProfile.STATE_CONNECTED) {
                mainHandler.post(() -> setStatus("Connected. Discovering setup service."));
                if (hasConnectPermission()) {
                    bluetoothGatt.discoverServices();
                }
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                mainHandler.post(() -> setStatus("Disconnected."));
                closeGatt();
            }
        }

        @Override
        public void onServicesDiscovered(BluetoothGatt bluetoothGatt, int status) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                mainHandler.post(() -> setStatus("Service discovery failed: " + status));
                return;
            }
            mainHandler.post(() -> {
                setStatus("Setup service ready.");
                enqueueProvisioningWrites();
            });
        }

        @Override
        public void onCharacteristicWrite(BluetoothGatt bluetoothGatt, BluetoothGattCharacteristic characteristic, int status) {
            mainHandler.post(() -> {
                writeInFlight = false;
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    setStatus("Write failed for " + characteristic.getUuid() + ": " + status);
                    writeQueue.clear();
                    return;
                }
                writeNext();
            });
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        BluetoothManager manager = (BluetoothManager) getSystemService(Context.BLUETOOTH_SERVICE);
        bluetoothAdapter = manager != null ? manager.getAdapter() : null;
        if (bluetoothAdapter != null) {
            scanner = bluetoothAdapter.getBluetoothLeScanner();
        }
        setContentView(buildContentView());
        ensurePermissions();
    }

    @Override
    protected void onStop() {
        super.onStop();
        stopScan();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        closeGatt();
    }

    private ScrollView buildContentView() {
        int pad = dp(16);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText("ESP32 Sensor Setup");
        title.setTextSize(24);
        title.setGravity(Gravity.START);
        root.addView(title, matchWrap());

        statusText = new TextView(this);
        statusText.setText("Ready.");
        statusText.setPadding(0, dp(8), 0, dp(8));
        root.addView(statusText, matchWrap());

        Button scanButton = new Button(this);
        scanButton.setText("Scan");
        scanButton.setOnClickListener(v -> startScan());
        root.addView(scanButton, matchWrap());

        deviceList = new LinearLayout(this);
        deviceList.setOrientation(LinearLayout.VERTICAL);
        root.addView(deviceList, matchWrap());

        deviceNameInput = input("Device name", "living-room-sensor", false);
        ssidInput = input("Wi-Fi SSID", "", false);
        passwordInput = input("Wi-Fi password", "", true);
        mqttHostInput = input("MQTT host", "", false);
        mqttPortInput = input("MQTT port", "1883", false);
        mqttPortInput.setInputType(InputType.TYPE_CLASS_NUMBER);

        root.addView(deviceNameInput, matchWrap());
        root.addView(ssidInput, matchWrap());
        root.addView(passwordInput, matchWrap());
        root.addView(mqttHostInput, matchWrap());
        root.addView(mqttPortInput, matchWrap());

        Button provisionButton = new Button(this);
        provisionButton.setText("Provision");
        provisionButton.setOnClickListener(v -> provisionSelectedDevice());
        root.addView(provisionButton, matchWrap());

        ScrollView scrollView = new ScrollView(this);
        scrollView.addView(root);
        return scrollView;
    }

    private EditText input(String hint, String value, boolean password) {
        EditText editText = new EditText(this);
        editText.setHint(hint);
        editText.setText(value);
        editText.setSingleLine(true);
        editText.setInputType(password
                ? InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD
                : InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_NORMAL);
        return editText;
    }

    private void ensurePermissions() {
        if (bluetoothAdapter == null) {
            setStatus("Bluetooth is not available on this device.");
            return;
        }
        if (!bluetoothAdapter.isEnabled()) {
            setStatus("Enable Bluetooth before scanning.");
            return;
        }

        List<String> missing = new ArrayList<>();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            addMissing(missing, Manifest.permission.BLUETOOTH_SCAN);
            addMissing(missing, Manifest.permission.BLUETOOTH_CONNECT);
        } else {
            addMissing(missing, Manifest.permission.ACCESS_FINE_LOCATION);
        }

        if (!missing.isEmpty()) {
            requestPermissions(missing.toArray(new String[0]), REQUEST_BLE_PERMISSIONS);
        }
    }

    private void addMissing(List<String> missing, String permission) {
        if (checkSelfPermission(permission) != PackageManager.PERMISSION_GRANTED) {
            missing.add(permission);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQUEST_BLE_PERMISSIONS) {
            setStatus(hasScanPermission() && hasConnectPermission()
                    ? "Ready."
                    : "Bluetooth permissions are required.");
        }
    }

    private void startScan() {
        ensurePermissions();
        if (scanner == null || !hasScanPermission()) {
            return;
        }

        stopScan();
        devices.clear();
        selectedDevice = null;
        renderDeviceList();

        ScanFilter filter = new ScanFilter.Builder()
                .setServiceUuid(new ParcelUuid(SERVICE_UUID))
                .build();
        ScanSettings settings = new ScanSettings.Builder()
                .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
                .build();
        scanner.startScan(List.of(filter), settings, scanCallback);
        scanning = true;
        setStatus("Scanning for ESP32 setup service.");
        mainHandler.postDelayed(this::stopScan, 15000);
    }

    private void stopScan() {
        if (scanning && scanner != null && hasScanPermission()) {
            scanner.stopScan(scanCallback);
        }
        scanning = false;
    }

    private void renderDeviceList() {
        deviceList.removeAllViews();
        if (devices.isEmpty()) {
            TextView empty = new TextView(this);
            empty.setText("No setup devices found.");
            empty.setPadding(0, dp(8), 0, dp(8));
            deviceList.addView(empty, matchWrap());
            return;
        }

        for (BluetoothDevice device : devices.values()) {
            Button button = new Button(this);
            String label = deviceLabel(device);
            button.setText(selectedDevice != null && selectedDevice.getAddress().equals(device.getAddress())
                    ? label + " selected"
                    : label);
            button.setOnClickListener(v -> {
                selectedDevice = device;
                setStatus("Selected " + deviceLabel(device));
                renderDeviceList();
            });
            deviceList.addView(button, matchWrap());
        }
    }

    private String deviceLabel(BluetoothDevice device) {
        String name = null;
        if (hasConnectPermission()) {
            name = device.getName();
        }
        return (name != null && !name.isBlank() ? name : "ESP32 setup") + "  " + device.getAddress();
    }

    private void provisionSelectedDevice() {
        if (selectedDevice == null) {
            setStatus("Select a device first.");
            return;
        }
        if (!hasConnectPermission()) {
            ensurePermissions();
            return;
        }

        stopScan();
        closeGatt();
        setStatus("Connecting to " + deviceLabel(selectedDevice));
        gatt = selectedDevice.connectGatt(this, false, gattCallback, BluetoothDevice.TRANSPORT_LE);
    }

    private void enqueueProvisioningWrites() {
        BluetoothGattService service = gatt != null ? gatt.getService(SERVICE_UUID) : null;
        if (service == null) {
            setStatus("ESP32 setup service was not found.");
            return;
        }

        writeQueue.clear();
        writeInFlight = false;
        addWrite(service, DEVICE_NAME_UUID, deviceNameInput.getText().toString().trim());
        addWrite(service, WIFI_SSID_UUID, ssidInput.getText().toString().trim());
        addWrite(service, WIFI_PASSWORD_UUID, passwordInput.getText().toString());
        addWrite(service, MQTT_HOST_UUID, mqttHostInput.getText().toString().trim());
        addWrite(service, MQTT_PORT_UUID, portValue());
        addWrite(service, APPLY_UUID, "save");

        setStatus("Writing provisioning settings.");
        writeNext();
    }

    private String portValue() {
        String port = mqttPortInput.getText().toString().trim();
        return port.isEmpty() ? "1883" : port;
    }

    private void addWrite(BluetoothGattService service, UUID uuid, String value) {
        BluetoothGattCharacteristic characteristic = service.getCharacteristic(uuid);
        if (characteristic != null) {
            writeQueue.add(new PendingWrite(characteristic, value.getBytes(StandardCharsets.UTF_8)));
        }
    }

    private void writeNext() {
        if (gatt == null || writeInFlight) {
            return;
        }

        PendingWrite next = writeQueue.poll();
        if (next == null) {
            setStatus("Provisioning complete. The ESP32 should restart and join Wi-Fi.");
            closeGatt();
            return;
        }

        next.characteristic.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
        boolean started;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            started = gatt.writeCharacteristic(next.characteristic, next.value, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT)
                    == BluetoothGatt.GATT_SUCCESS;
        } else {
            next.characteristic.setValue(next.value);
            started = gatt.writeCharacteristic(next.characteristic);
        }

        writeInFlight = started;
        if (!started) {
            setStatus("Could not write " + next.characteristic.getUuid());
            writeQueue.clear();
        }
    }

    private boolean hasScanPermission() {
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.S
                || checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) == PackageManager.PERMISSION_GRANTED;
    }

    private boolean hasConnectPermission() {
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.S
                || checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED;
    }

    private void closeGatt() {
        if (gatt != null && hasConnectPermission()) {
            gatt.close();
        }
        gatt = null;
        writeQueue.clear();
        writeInFlight = false;
    }

    private void setStatus(String status) {
        statusText.setText(status);
    }

    private LinearLayout.LayoutParams matchWrap() {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        params.setMargins(0, dp(6), 0, dp(6));
        return params;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private static final class PendingWrite {
        final BluetoothGattCharacteristic characteristic;
        final byte[] value;

        PendingWrite(BluetoothGattCharacteristic characteristic, byte[] value) {
            this.characteristic = characteristic;
            this.value = value;
        }
    }
}
