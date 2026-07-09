# Provisioning and Integration

This firmware is an ESP32 Arduino/PlatformIO smart sensor for SHT41 and BME680
breakouts on the default I2C bus. It stores one JSON history record per minute
in LittleFS and publishes current readings over MQTT for standard home
automation tools.

## BLE provisioning

The device advertises a BLE setup service so credentials and the device name can
be changed without reflashing.

Service UUID:

```text
9e9a0001-6f3d-4f57-9e9f-8c2b9a5f1000
```

Characteristics:

| UUID suffix | Field | Access |
| --- | --- | --- |
| `0002` | Device name | Read/write |
| `0003` | Wi-Fi SSID | Read/write |
| `0004` | Wi-Fi password | Write |
| `0005` | MQTT host | Read/write |
| `0006` | MQTT port | Read/write |
| `0007` | Apply config | Write `save`, `apply`, or `1` |
| `0008` | Status JSON | Read/notify |

After writing the desired values, write `save` to the apply characteristic. The
device stores settings in ESP32 NVS and restarts.

## MQTT and Home Assistant

Set the MQTT host and optional port during BLE provisioning. The firmware
publishes Home Assistant MQTT discovery records under:

```text
homeassistant/sensor/<device>_<reading>/config
```

State is retained at:

```text
home/sensors/<device>/state
```

Availability is retained at:

```text
home/sensors/<device>/availability
```

The state payload is JSON:

```json
{
  "ts": 1783632952,
  "sht41": {
    "temperature_c": 21.7,
    "humidity_pct": 44.1
  },
  "bme680": {
    "temperature_c": 22.0,
    "humidity_pct": 43.8,
    "pressure_hpa": 1008.4,
    "gas_kohms": 117.2
  }
}
```

## HTTP diagnostics

When connected to Wi-Fi, the device serves:

| Path | Response |
| --- | --- |
| `/` | Device status JSON |
| `/latest` | Latest sensor sample JSON |
| `/history` | Historical samples as newline-delimited JSON |

## Historical data

The history file is stored at `/history.jsonl` in LittleFS. It is compacted when
it exceeds 256 KiB, keeping up to the latest 1440 records.

## Hardware notes

- Default ESP32 I2C pins are used by `Wire.begin()` for the selected board.
- SHT41 is auto-detected using the Adafruit SHT4x driver.
- BME680 is tried at both common I2C addresses, `0x76` and `0x77`.
- Use 3.3 V sensor breakouts or level shifting. Do not connect bare 5 V I2C
  signals to the ESP32.
