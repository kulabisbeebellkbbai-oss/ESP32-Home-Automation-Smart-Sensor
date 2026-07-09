# Build and Flash

## Install dependencies

PlatformIO resolves firmware dependencies from `platformio.ini`.

## Build

```bash
pio run
```

## Flash

Connect the ESP32 over USB, then run:

```bash
pio run -t upload
```

## Serial monitor

```bash
pio device monitor -b 115200
```

The boot log reports whether SHT41 and BME680 were detected and when Wi-Fi or
MQTT connects.
