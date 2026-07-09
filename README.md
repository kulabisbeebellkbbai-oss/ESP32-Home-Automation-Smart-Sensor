# ESP32 Home Automation Smart Sensor

ESP32 smart sensor firmware for SHT41 and BME680 environmental sensing.

## Features

- BLE provisioning for device name, Wi-Fi credentials, and MQTT settings.
- SHT41 temperature and humidity readings.
- BME680 temperature, humidity, pressure, and gas-resistance readings.
- Home Assistant MQTT discovery and retained state publishing.
- On-device historical data in LittleFS as newline-delimited JSON.
- HTTP diagnostics for status, latest sample, and history export.

## Build

```bash
pio run
```

## Flash

```bash
pio run -t upload
```

See [docs/build-and-flash.md](docs/build-and-flash.md) and
[docs/provisioning-and-integration.md](docs/provisioning-and-integration.md).
