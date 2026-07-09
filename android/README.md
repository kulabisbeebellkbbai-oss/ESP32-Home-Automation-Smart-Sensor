# Android Provisioning App

Native Android app for provisioning the ESP32 Home Automation Smart Sensor over
BLE.

## Build

```bash
cd android
gradle --no-daemon assembleDebug
```

The debug APK is written to:

```text
android/app/build/outputs/apk/debug/app-debug.apk
```

## Flow

1. Power the ESP32 sensor so it advertises the setup BLE service.
2. Open the app and allow Bluetooth permissions.
3. Scan, select the ESP32 setup device, enter device name, Wi-Fi credentials,
   and MQTT broker settings.
4. Tap Provision. The app writes the BLE characteristics and sends `save`.

The UUIDs match `docs/provisioning-and-integration.md`.
