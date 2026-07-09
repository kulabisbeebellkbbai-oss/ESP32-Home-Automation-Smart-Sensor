#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <memory>
#include <time.h>

#include <Adafruit_BME680.h>
#include <Adafruit_SHT4x.h>

#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

namespace {

constexpr const char *kFirmwareVersion = "0.1.0";
constexpr const char *kDefaultDeviceName = "esp32-smart-sensor";
constexpr const char *kHistoryPath = "/history.jsonl";
constexpr size_t kHistoryMaxBytes = 256 * 1024;
constexpr size_t kHistoryKeepRecords = 1440;
constexpr uint32_t kSensorIntervalMs = 30000;
constexpr uint32_t kHistoryIntervalMs = 60000;
constexpr uint32_t kMqttIntervalMs = 30000;
constexpr uint32_t kWifiRetryMs = 10000;
constexpr uint32_t kBleProvisioningWindowMs = 5 * 60000;

constexpr const char *kProvisioningServiceUuid = "9e9a0001-6f3d-4f57-9e9f-8c2b9a5f1000";
constexpr const char *kDeviceNameCharUuid = "9e9a0002-6f3d-4f57-9e9f-8c2b9a5f1000";
constexpr const char *kWifiSsidCharUuid = "9e9a0003-6f3d-4f57-9e9f-8c2b9a5f1000";
constexpr const char *kWifiPasswordCharUuid = "9e9a0004-6f3d-4f57-9e9f-8c2b9a5f1000";
constexpr const char *kMqttHostCharUuid = "9e9a0005-6f3d-4f57-9e9f-8c2b9a5f1000";
constexpr const char *kMqttPortCharUuid = "9e9a0006-6f3d-4f57-9e9f-8c2b9a5f1000";
constexpr const char *kApplyCharUuid = "9e9a0007-6f3d-4f57-9e9f-8c2b9a5f1000";
constexpr const char *kStatusCharUuid = "9e9a0008-6f3d-4f57-9e9f-8c2b9a5f1000";

struct DeviceConfig {
  String deviceName = kDefaultDeviceName;
  String wifiSsid;
  String wifiPassword;
  String mqttHost;
  uint16_t mqttPort = 1883;
};

struct SensorSample {
  bool shtOk = false;
  bool bmeOk = false;
  float shtTemperatureC = NAN;
  float shtHumidityPct = NAN;
  float bmeTemperatureC = NAN;
  float bmeHumidityPct = NAN;
  float bmePressureHpa = NAN;
  float bmeGasKohms = NAN;
  uint64_t timestamp = 0;
};

Preferences prefs;
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
WebServer server(80);
Adafruit_SHT4x sht4;
Adafruit_BME680 bme;
DeviceConfig config;
SensorSample latestSample;

bool shtPresent = false;
bool bmePresent = false;
bool bleRunning = false;
bool mqttDiscoveryPublished = false;
uint32_t lastSensorReadMs = 0;
uint32_t lastHistoryWriteMs = 0;
uint32_t lastMqttPublishMs = 0;
uint32_t lastWifiAttemptMs = 0;

String sanitizeTopic(const String &value) {
  String out;
  out.reserve(value.length());
  for (size_t i = 0; i < value.length(); i++) {
    const char c = value[i];
    if (isalnum(static_cast<unsigned char>(c))) {
      out += static_cast<char>(tolower(c));
    } else if (c == '-' || c == '_' || c == ' ') {
      out += '_';
    }
  }
  out.trim();
  return out.length() ? out : kDefaultDeviceName;
}

uint64_t currentTimestamp() {
  const time_t now = time(nullptr);
  if (now > 1700000000) {
    return static_cast<uint64_t>(now);
  }
  return millis() / 1000;
}

String baseTopic() {
  return "home/sensors/" + sanitizeTopic(config.deviceName);
}

void loadConfig() {
  prefs.begin("sensor", true);
  config.deviceName = prefs.getString("name", kDefaultDeviceName);
  config.wifiSsid = prefs.getString("ssid", "");
  config.wifiPassword = prefs.getString("pass", "");
  config.mqttHost = prefs.getString("mqttHost", "");
  config.mqttPort = prefs.getUShort("mqttPort", 1883);
  prefs.end();
}

void saveConfig() {
  prefs.begin("sensor", false);
  prefs.putString("name", config.deviceName.length() ? config.deviceName : kDefaultDeviceName);
  prefs.putString("ssid", config.wifiSsid);
  prefs.putString("pass", config.wifiPassword);
  prefs.putString("mqttHost", config.mqttHost);
  prefs.putUShort("mqttPort", config.mqttPort ? config.mqttPort : 1883);
  prefs.end();
}

String statusJson() {
  JsonDocument doc;
  doc["name"] = config.deviceName;
  doc["wifi"] = WiFi.status() == WL_CONNECTED;
  doc["ip"] = WiFi.localIP().toString();
  doc["mqtt"] = mqtt.connected();
  doc["sht41"] = shtPresent;
  doc["bme680"] = bmePresent;
  doc["firmware"] = kFirmwareVersion;
  String out;
  serializeJson(doc, out);
  return out;
}

class StringWriteCallback : public BLECharacteristicCallbacks {
 public:
  explicit StringWriteCallback(String *target) : target_(target) {}

  void onWrite(BLECharacteristic *characteristic) override {
    *target_ = characteristic->getValue().c_str();
  }

 private:
  String *target_;
};

class PortWriteCallback : public BLECharacteristicCallbacks {
 public:
  void onWrite(BLECharacteristic *characteristic) override {
    const String value = characteristic->getValue().c_str();
    const int port = value.toInt();
    if (port > 0 && port <= 65535) {
      config.mqttPort = static_cast<uint16_t>(port);
    }
  }
};

class ApplyWriteCallback : public BLECharacteristicCallbacks {
 public:
  explicit ApplyWriteCallback(BLECharacteristic *status) : status_(status) {}

  void onWrite(BLECharacteristic *characteristic) override {
    const String value = characteristic->getValue().c_str();
    if (value == "1" || value.equalsIgnoreCase("save") || value.equalsIgnoreCase("apply")) {
      saveConfig();
      status_->setValue(statusJson().c_str());
      status_->notify();
      WiFi.disconnect(true);
      delay(100);
      ESP.restart();
    }
  }

 private:
  BLECharacteristic *status_;
};

BLECharacteristic *addTextCharacteristic(BLEService *service, const char *uuid, const String &initialValue,
                                         BLECharacteristicCallbacks *callbacks) {
  BLECharacteristic *characteristic = service->createCharacteristic(
      uuid, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  characteristic->setValue(initialValue.c_str());
  characteristic->setCallbacks(callbacks);
  return characteristic;
}

void startBleProvisioning() {
  if (bleRunning) {
    return;
  }

  BLEDevice::init((config.deviceName + " setup").c_str());
  BLEServer *bleServer = BLEDevice::createServer();
  BLEService *service = bleServer->createService(kProvisioningServiceUuid);

  addTextCharacteristic(service, kDeviceNameCharUuid, config.deviceName, new StringWriteCallback(&config.deviceName));
  addTextCharacteristic(service, kWifiSsidCharUuid, config.wifiSsid, new StringWriteCallback(&config.wifiSsid));
  addTextCharacteristic(service, kWifiPasswordCharUuid, "", new StringWriteCallback(&config.wifiPassword));
  addTextCharacteristic(service, kMqttHostCharUuid, config.mqttHost, new StringWriteCallback(&config.mqttHost));

  BLECharacteristic *mqttPort = service->createCharacteristic(
      kMqttPortCharUuid, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  mqttPort->setValue(String(config.mqttPort).c_str());
  mqttPort->setCallbacks(new PortWriteCallback());

  BLECharacteristic *status = service->createCharacteristic(
      kStatusCharUuid, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  status->addDescriptor(new BLE2902());
  status->setValue(statusJson().c_str());

  BLECharacteristic *apply = service->createCharacteristic(kApplyCharUuid, BLECharacteristic::PROPERTY_WRITE);
  apply->setCallbacks(new ApplyWriteCallback(status));

  service->start();
  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(kProvisioningServiceUuid);
  advertising->setScanResponse(true);
  BLEDevice::startAdvertising();
  bleRunning = true;
  Serial.println("BLE provisioning started");
}

void setupSensors() {
  Wire.begin();
  shtPresent = sht4.begin();
  if (shtPresent) {
    sht4.setPrecision(SHT4X_HIGH_PRECISION);
    sht4.setHeater(SHT4X_NO_HEATER);
  }

  bmePresent = bme.begin(0x76) || bme.begin(0x77);
  if (bmePresent) {
    bme.setTemperatureOversampling(BME680_OS_8X);
    bme.setHumidityOversampling(BME680_OS_2X);
    bme.setPressureOversampling(BME680_OS_4X);
    bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
    bme.setGasHeater(320, 150);
  }

  Serial.printf("SHT41: %s, BME680: %s\n", shtPresent ? "present" : "missing",
                bmePresent ? "present" : "missing");
}

void connectWifi() {
  if (!config.wifiSsid.length() || WiFi.status() == WL_CONNECTED || millis() - lastWifiAttemptMs < kWifiRetryMs) {
    return;
  }

  lastWifiAttemptMs = millis();
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(sanitizeTopic(config.deviceName).c_str());
  WiFi.begin(config.wifiSsid.c_str(), config.wifiPassword.c_str());
  Serial.printf("Connecting to Wi-Fi SSID %s\n", config.wifiSsid.c_str());
}

void setupTime() {
  if (config.wifiSsid.length()) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  }
}

void readSensors() {
  latestSample.timestamp = currentTimestamp();

  if (shtPresent) {
    sensors_event_t humidity;
    sensors_event_t temperature;
    latestSample.shtOk = sht4.getEvent(&humidity, &temperature);
    if (latestSample.shtOk) {
      latestSample.shtTemperatureC = temperature.temperature;
      latestSample.shtHumidityPct = humidity.relative_humidity;
    }
  }

  if (bmePresent) {
    latestSample.bmeOk = bme.performReading();
    if (latestSample.bmeOk) {
      latestSample.bmeTemperatureC = bme.temperature;
      latestSample.bmeHumidityPct = bme.humidity;
      latestSample.bmePressureHpa = bme.pressure / 100.0F;
      latestSample.bmeGasKohms = bme.gas_resistance / 1000.0F;
    }
  }
}

void writeSampleJson(Print &out, const SensorSample &sample) {
  JsonDocument doc;
  doc["ts"] = sample.timestamp;
  if (sample.shtOk) {
    doc["sht41"]["temperature_c"] = sample.shtTemperatureC;
    doc["sht41"]["humidity_pct"] = sample.shtHumidityPct;
  }
  if (sample.bmeOk) {
    doc["bme680"]["temperature_c"] = sample.bmeTemperatureC;
    doc["bme680"]["humidity_pct"] = sample.bmeHumidityPct;
    doc["bme680"]["pressure_hpa"] = sample.bmePressureHpa;
    doc["bme680"]["gas_kohms"] = sample.bmeGasKohms;
  }
  serializeJson(doc, out);
}

String sampleJson(const SensorSample &sample) {
  JsonDocument doc;
  doc["ts"] = sample.timestamp;
  if (sample.shtOk) {
    doc["sht41"]["temperature_c"] = sample.shtTemperatureC;
    doc["sht41"]["humidity_pct"] = sample.shtHumidityPct;
  }
  if (sample.bmeOk) {
    doc["bme680"]["temperature_c"] = sample.bmeTemperatureC;
    doc["bme680"]["humidity_pct"] = sample.bmeHumidityPct;
    doc["bme680"]["pressure_hpa"] = sample.bmePressureHpa;
    doc["bme680"]["gas_kohms"] = sample.bmeGasKohms;
  }
  String out;
  serializeJson(doc, out);
  return out;
}

void compactHistoryIfNeeded() {
  File history = LittleFS.open(kHistoryPath, "r");
  if (!history || history.size() <= kHistoryMaxBytes) {
    if (history) {
      history.close();
    }
    return;
  }

  std::unique_ptr<String[]> records(new String[kHistoryKeepRecords]);
  size_t count = 0;
  while (history.available()) {
    const String line = history.readStringUntil('\n');
    if (!line.length()) {
      continue;
    }
    records[count % kHistoryKeepRecords] = line;
    count++;
  }
  history.close();

  File compacted = LittleFS.open("/history.tmp", "w");
  const size_t start = count > kHistoryKeepRecords ? count % kHistoryKeepRecords : 0;
  const size_t kept = min(count, kHistoryKeepRecords);
  for (size_t i = 0; i < kept; i++) {
    const size_t index = (start + i) % kHistoryKeepRecords;
    compacted.println(records[index]);
  }
  compacted.close();
  LittleFS.remove(kHistoryPath);
  LittleFS.rename("/history.tmp", kHistoryPath);
}

void appendHistory() {
  if (!latestSample.shtOk && !latestSample.bmeOk) {
    return;
  }

  File history = LittleFS.open(kHistoryPath, "a");
  if (!history) {
    Serial.println("Failed to open history log");
    return;
  }
  writeSampleJson(history, latestSample);
  history.println();
  history.close();
  compactHistoryIfNeeded();
}

void handleLatest() {
  server.send(200, "application/json", sampleJson(latestSample));
}

void handleHistory() {
  File history = LittleFS.open(kHistoryPath, "r");
  if (!history) {
    server.send(200, "application/x-ndjson", "");
    return;
  }
  server.streamFile(history, "application/x-ndjson");
  history.close();
}

void setupHttp() {
  server.on("/", []() { server.send(200, "application/json", statusJson()); });
  server.on("/latest", handleLatest);
  server.on("/history", handleHistory);
  server.begin();
}

void publishDiscoverySensor(const char *objectId, const char *name, const char *stateClass, const char *deviceClass,
                            const char *unit, const char *valueTemplate) {
  JsonDocument doc;
  const String uid = sanitizeTopic(config.deviceName) + "_" + objectId;
  const String stateTopic = baseTopic() + "/state";
  const String availabilityTopic = baseTopic() + "/availability";
  const String configTopic = "homeassistant/sensor/" + uid + "/config";

  doc["name"] = name;
  doc["unique_id"] = uid;
  doc["state_topic"] = stateTopic;
  doc["availability_topic"] = availabilityTopic;
  doc["value_template"] = valueTemplate;
  if (stateClass) {
    doc["state_class"] = stateClass;
  }
  if (deviceClass) {
    doc["device_class"] = deviceClass;
  }
  if (unit) {
    doc["unit_of_measurement"] = unit;
  }
  JsonObject device = doc["device"].to<JsonObject>();
  device["identifiers"][0] = sanitizeTopic(config.deviceName);
  device["name"] = config.deviceName;
  device["manufacturer"] = "Codex";
  device["model"] = "ESP32 SHT41 BME680 Smart Sensor";
  device["sw_version"] = kFirmwareVersion;

  String payload;
  serializeJson(doc, payload);
  mqtt.publish(configTopic.c_str(), payload.c_str(), true);
}

void publishMqttDiscovery() {
  publishDiscoverySensor("sht41_temperature", "SHT41 Temperature", "measurement", "temperature", "C",
                         "{{ value_json.sht41.temperature_c }}");
  publishDiscoverySensor("sht41_humidity", "SHT41 Humidity", "measurement", "humidity", "%",
                         "{{ value_json.sht41.humidity_pct }}");
  publishDiscoverySensor("bme680_temperature", "BME680 Temperature", "measurement", "temperature", "C",
                         "{{ value_json.bme680.temperature_c }}");
  publishDiscoverySensor("bme680_humidity", "BME680 Humidity", "measurement", "humidity", "%",
                         "{{ value_json.bme680.humidity_pct }}");
  publishDiscoverySensor("bme680_pressure", "BME680 Pressure", "measurement", "pressure", "hPa",
                         "{{ value_json.bme680.pressure_hpa }}");
  publishDiscoverySensor("bme680_gas", "BME680 Gas Resistance", "measurement", nullptr, "kOhm",
                         "{{ value_json.bme680.gas_kohms }}");
  mqttDiscoveryPublished = true;
}

void connectMqtt() {
  if (!config.mqttHost.length() || WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (!mqtt.connected()) {
    mqtt.setServer(config.mqttHost.c_str(), config.mqttPort);
    const String clientId = sanitizeTopic(config.deviceName);
    if (mqtt.connect(clientId.c_str(), (baseTopic() + "/availability").c_str(), 1, true, "offline")) {
      mqtt.publish((baseTopic() + "/availability").c_str(), "online", true);
      mqttDiscoveryPublished = false;
      Serial.println("Connected to MQTT");
    }
  }

  if (mqtt.connected() && !mqttDiscoveryPublished) {
    publishMqttDiscovery();
  }
}

void publishSample() {
  if (!mqtt.connected() || (!latestSample.shtOk && !latestSample.bmeOk)) {
    return;
  }

  const String payload = sampleJson(latestSample);
  mqtt.publish((baseTopic() + "/state").c_str(), payload.c_str(), true);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(250);
  loadConfig();
  LittleFS.begin(true);
  setupSensors();
  setupHttp();
  setupTime();
  startBleProvisioning();
}

void loop() {
  const uint32_t now = millis();

  connectWifi();
  connectMqtt();
  mqtt.loop();
  server.handleClient();

  if (now - lastSensorReadMs >= kSensorIntervalMs || lastSensorReadMs == 0) {
    lastSensorReadMs = now;
    readSensors();
  }

  if (now - lastHistoryWriteMs >= kHistoryIntervalMs || lastHistoryWriteMs == 0) {
    lastHistoryWriteMs = now;
    appendHistory();
  }

  if (now - lastMqttPublishMs >= kMqttIntervalMs || lastMqttPublishMs == 0) {
    lastMqttPublishMs = now;
    publishSample();
  }

  if (!config.wifiSsid.length() || WiFi.status() != WL_CONNECTED || now < kBleProvisioningWindowMs) {
    startBleProvisioning();
  }
}
