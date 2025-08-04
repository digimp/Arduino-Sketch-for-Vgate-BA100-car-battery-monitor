#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "esp32-hal-timer.h"

// WiFi e MQTT
const char* ssid = "YOUR_SSID";
const char* password = "PSW";
const char* mqtt_server = "IP_BROKER_MQTT";
const char* mqtt_user = "USERNAME";
const char* mqtt_pass = "PSW";

WiFiClient espClient;
PubSubClient client(espClient);

// BLE
static BLEAddress targetAddress("00:00:00:C3:72:68"); // THIS MAC OF YOUR BA100
static BLEUUID serviceUUID("0000ae00-0000-1000-8000-00805f9b34fb"); // FIND THIS CODE WITH nRF SMARTPHONE APP
static BLEUUID charUUID("0000ae02-0000-1000-8000-00805f9b34fb");  // FIND THIS CODE WITH nRF SMARTPHONE APP

BLEClient*  pClient;
BLERemoteCharacteristic* pRemoteCharacteristic;
bool connected = false;
unsigned long lastMqttPublish = 0;
const unsigned long mqttInterval = 30000;  # EVERY 30 SECONDS

// Watchdog (timer hardware)
hw_timer_t* timer = NULL;
void IRAM_ATTR resetModule() {
  esp_restart();
}

// Diagnostic
unsigned long lastDebugReport = 0;
unsigned long lastBleTime = 0;

void notifyCallback(BLERemoteCharacteristic* pBLERemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
  lastBleTime = millis();  // 🔁 time update last notification
  String payload = "";
  Serial.print("🔔 Received notification: ");
  for (size_t i = 0; i < length; i++) {
    if (pData[i] < 0x10) Serial.print("0");
    Serial.print(pData[i], HEX);
    if (i < length - 1) Serial.print("-");

    if (payload.length() > 0) payload += "-";
    if (pData[i] < 0x10) payload += "0";
    payload += String(pData[i], HEX);
  }
  Serial.println();

  if (client.connected()) {
    unsigned long now = millis();
    if (now - lastMqttPublish >= mqttInterval) {
      bool sent1 = client.publish("Battery/Car", payload.c_str());
      bool sent2 = client.publish("Battery/Car/state", "online", true);
      if (sent1 && sent2) {
        Serial.print("📤 Sent via MQTT: ");
        Serial.println(payload);
        lastMqttPublish = now;
      } else {
        Serial.println("⚠️ MQTT send error.");
      }
    } else {
      Serial.println("⏱️ MQTT send ignored (wait)");
    }
  } else {
    Serial.println("⚠️ MQTT not connected, impossible to send.");
  }
}

// BLE Connection
bool scanAndConnect() {
  Serial.println("🔍 BLE scanning...");
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setActiveScan(true);
  BLEScanResults results = *pBLEScan->start(10);
  Serial.print("📡 Found ");
  Serial.print(results.getCount());
  Serial.println(" BLE devices:");
  for (int i = 0; i < results.getCount(); i++) {
    BLEAdvertisedDevice d = results.getDevice(i);
    Serial.print(" - ");
    Serial.println(d.toString().c_str());
  }

  pClient = BLEDevice::createClient();
  Serial.println("🔗 Connecting to BLE device known...");
  if (!pClient->connect(targetAddress)) {
    Serial.println("❌ Connection failed.");
    return false;
  }

  Serial.println("✅ Connected, looking for service...");
  BLERemoteService* pRemoteService = pClient->getService(serviceUUID);
  if (pRemoteService == nullptr) {
    Serial.println("❌ Service not found.");
    pClient->disconnect();
    return false;
  }

  pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUID);
  if (pRemoteCharacteristic == nullptr) {
    Serial.println("❌ Char not found.");
    pClient->disconnect();
    return false;
  }

  if (pRemoteCharacteristic->canNotify()) {
    pRemoteCharacteristic->registerForNotify(notifyCallback);
    Serial.println("📡 Notification enabled.");
  }

  connected = true;
  return true;
}

// MQTT Connection
void reconnectMQTT() {
  if (!client.connected()) {
    Serial.print("🌐 Connecting MQTT...");
    if (client.connect("ESP32Client", mqtt_user, mqtt_pass)) {
      Serial.println("✅ MQTT connected");
      String configPayload = R"({
        "name": "State ESP Car",
        "state_topic": "Battery/Car/state",
        "payload_on": "online",
        "payload_off": "offline",
        "device_class": "connectivity",
        "unique_id": "esp32_car_status",
        "device": {
          "identifiers": ["esp32_car_battery"],
          "name": "ESP32 Car Battery",
          "manufacturer": "ESPHome-Style",
          "model": "ESP32-C3"
        }
      })";
      client.publish("homeassistant/binary_sensor/battery_car_state/config", configPayload.c_str(), true);
      client.publish("Battery/Car/state", "online", true);
    } else {
      Serial.print("❌ MQTT rc=");
      Serial.println(client.state());
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  const char* sketchName = "BLE_battery_diagnostic";
  Serial.print("📄 Sketch active: ");
  Serial.println(sketchName);

  BLEDevice::init("");

  WiFi.begin(ssid, password);
  Serial.print("📶 WiFi connection");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" ✅");

  client.setServer(mqtt_server, 1883);
  reconnectMQTT();

  timer = timerBegin(1);
  timerAttachInterrupt(timer, &resetModule);
  timerAlarm(timer, 180000000, false, 0);
  timerStart(timer);

  connected = scanAndConnect();
  if (!connected) {
    Serial.println("❌ First BLE connection failed.");
  }
}

void mqttLoop() {
  if (!client.connected()) {
    reconnectMQTT();
  }
  client.loop();
}

void loop() {
  mqttLoop();
  client.loop();

  // ⚠️ Detect connection BLE unintercepted
  if (connected && (!pClient || !pClient->isConnected())) {
    Serial.println("⚠️ BLE logically connected, but physically unconnected. Force reconnection.");
    connected = false;
  }

  // BLE reconnection if necessary
  if (!connected) {
    Serial.println("🔁 BLE reconnection attempt...");
    connected = scanAndConnect();
    if (!connected) {
      Serial.println("❌ BLE retry failed, reboot in 3 min if persists.");
    }
  }

  if (connected && timer) {
    timerWrite(timer, 0);
  }

  if (millis() - lastDebugReport >= 30000) {
    lastDebugReport = millis();

    char heapBuf[16];
    snprintf(heapBuf, sizeof(heapBuf), "%lu", ESP.getFreeHeap());
    client.publish("Battery/Car/debug_heap", heapBuf);

    if (pClient && pClient->isConnected()) {
      client.publish("Battery/Car/debug_ble", "BLE CONNECTED");
    } else {
      client.publish("Battery/Car/debug_ble", "BLE UNCONNECTED");
    }

    if (lastBleTime > 0 && millis() - lastBleTime > 120000) {
      client.publish("Battery/Car/debug_ble_timeout", "ANY NOTIFICATION FOR OVER 2 MINUTES");
    }

    if (!client.connected()) {
      client.publish("Battery/Car/debug_mqtt", "MQTT DISCONNECTED");
    } else {
      if (!client.publish("Battery/Car/debug_mqtt", "MQTT OK")) {
        client.publish("Battery/Car/debug_mqtt", "MQTT PUBLISH FAILED");
      }
    }
  }

  delay(1000);
}
