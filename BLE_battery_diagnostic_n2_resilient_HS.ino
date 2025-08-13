#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "esp32-hal-timer.h"
#include "esp_bt.h"            // ADD: per esp_bt_controller_mem_release
#include "esp_heap_caps.h"     // ADD: per heap_caps_* metrics

// ---------- WiFi / MQTT ----------
const char* ssid       = "SSID";
const char* password   = "psw";
const char* mqtt_server= "192.168.1.xxx"; // your mqtt broker ip 
const char* mqtt_user  = "username";
const char* mqtt_pass  = "psw";

WiFiClient espClient;
PubSubClient client(espClient);

// ---------- BLE target ----------
static const char* TARGET_MAC = "00:00:00:C3:72:68"; // your BA100 mac
static BLEUUID serviceUUID("0000ae00-0000-1000-8000-00805f9b34fb"); // for service and char check your nRF app
static BLEUUID charUUID   ("0000ae02-0000-1000-8000-00805f9b34fb");

BLEClient*  pClient = nullptr;
BLERemoteCharacteristic* pRemoteCharacteristic = nullptr;
bool connected = false;

unsigned long lastMqttPublish = 0;
const unsigned long mqttInterval = 30000UL; // 30s

// ---------- Watchdog ----------
hw_timer_t* timer = nullptr;
void IRAM_ATTR resetModule() { esp_restart(); }

// ---------- Diagnostica ----------
unsigned long lastDebugReport = 0;
unsigned long lastBleTime = 0;

// Retry pacing / recovery
static unsigned long nextBleTryMs = 0;
static uint16_t bleFailStreak = 0;

// ---------- Heap monitor (ADD) ----------
static uint32_t heapMinEver = UINT32_MAX;              // traccia il minimo storico (se utile)
static const uint32_t HEAP_LARGEST_GUARD = 16 * 1024;  // soglia largest block (16KB)

// ---------- Client callbacks ----------
class MyClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient* c) override {
    (void)c;
  
  }
  void onDisconnect(BLEClient* c) override {
    (void)c;
    connected = false;
    Serial.println("⚠️ Evento onDisconnect: link BLE caduto");
  }
};

// MQTT
void reconnectMQTT() {
  if (!client.connected()) {
    Serial.print("🌐 Connessione MQTT...");
    if (client.connect("ESP32Client", mqtt_user, mqtt_pass)) {
      Serial.println("✅ MQTT connesso");
      String configPayload = R"({
        "name": "Stato ESP Duster",
        "state_topic": "Batteria/Duster/stato",
        "payload_on": "online",
        "payload_off": "offline",
        "device_class": "connectivity",
        "unique_id": "esp32_duster_status",
        "device": {
          "identifiers": ["esp32_duster_battery"],
          "name": "ESP32 Duster Battery",
          "manufacturer": "ESPHome-Style",
          "model": "ESP32-C3"
        }
      })";
      client.publish("homeassistant/binary_sensor/batteria_duster_stato/config", configPayload.c_str(), true);
      client.publish("Batteria/Duster/stato", "online", true);
    } else {
      Serial.print("❌ MQTT rc="); Serial.println(client.state());
    }
  }
}

// Notify
void notifyCallback(BLERemoteCharacteristic*, uint8_t* pData, size_t length, bool) {
  lastBleTime = millis();

  // payload "AA-BB-..."
  String payload;
  payload.reserve(length * 3); // ADD: evita riallocazioni ripetute
  Serial.print("🔔 Notifica ricevuta: ");
  for (size_t i = 0; i < length; i++) {
    if (pData[i] < 0x10) Serial.print("0");
    Serial.print(pData[i], HEX);
    if (i < length - 1) Serial.print("-");

    if (payload.length()) payload += "-";
    if (pData[i] < 0x10) payload += "0";
    payload += String(pData[i], HEX);
  }
  Serial.println();
  payload.toUpperCase();

  if (client.connected()) {
    unsigned long now = millis();
    if (now - lastMqttPublish >= mqttInterval) {
      bool sent1 = client.publish("Batteria/Duster", payload.c_str());
      bool sent2 = client.publish("Batteria/Duster/stato", "online", true);
      if (sent1 && sent2) {
        Serial.print("📤 Inviato via MQTT: "); Serial.println(payload);
        lastMqttPublish = now;
      } else {
        Serial.println("⚠️ Errore nell'invio MQTT.");
      }
    } else {
      Serial.println("⏱️ Invio MQTT ignorato (attendi)");
    }
  } else {
    Serial.println("⚠️ MQTT non connesso, impossibile inviare.");
  }
}

// Scan + connect via advertised device
bool scanAndConnect() {
  Serial.println("🔍 Scansione BLE in corso...");
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setActiveScan(true);

  //
  BLEScanResults* results = pBLEScan->start(10);
  int n = results ? results->getCount() : 0;
  Serial.print("📡 Trovati "); Serial.print(n); Serial.println(" dispositivi BLE:");

  BLEAdvertisedDevice targetDev;
  bool found = false;
  BLEAddress targetAddr(TARGET_MAC);

  for (int i = 0; i < n; i++) {
    BLEAdvertisedDevice d = results->getDevice(i);
    Serial.print(" - "); Serial.println(d.toString().c_str());
    if (d.getAddress().equals(targetAddr)) {
      targetDev = d;
      found = true;
    }
  }
  pBLEScan->stop();
  pBLEScan->clearResults();

  if (!found) {
    Serial.println("❌ Target non trovato nello scan");
    return false;
  }

  if (pClient) { if (pClient->isConnected()) pClient->disconnect(); delete pClient; pClient = nullptr; }
  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallbacks());

  Serial.println("🔗 Connessione al dispositivo BLE tramite advertised device...");
  if (!pClient->connect(&targetDev)) {
    Serial.println("❌ Connessione fallita");
    return false;
  }

  Serial.println("✅ Connesso, ricerca servizio...");
  BLERemoteService* pRemoteService = pClient->getService(serviceUUID);
  if (pRemoteService == nullptr) {
    Serial.println("❌ Servizio non trovato");
    pClient->disconnect();
    return false;
  }

  pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUID);
  if (pRemoteCharacteristic == nullptr) {
    Serial.println("❌ Caratteristica non trovata");
    pClient->disconnect();
    return false;
  }

  if (pRemoteCharacteristic->canNotify()) {
    // piccolo respiro prima della subscribe
    delay(150);
    pRemoteCharacteristic->registerForNotify(notifyCallback);
    Serial.println("📡 Notifiche abilitate.");
  } else {
    Serial.println("❌ La caratteristica non dichiara notify");
    pClient->disconnect();
    return false;
  }

  connected = true;
  lastBleTime = millis();
  Serial.println("🟢 BLE pronto");
  return true;
}

//  Setup
void setup() {
  Serial.begin(115200);
  delay(600);
  const char* sketchName = "BLE_battery_diagnostic_n2_resilient_HS";
  Serial.print("📄 Sketch attivo: ");
  Serial.println(sketchName);

  // ADD: libera RAM del Bluetooth classico prima di inizializzare il BLE
  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

  BLEDevice::init("");

  WiFi.begin(ssid, password);
  Serial.print("📶 Connessione WiFi");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(300); Serial.print(".");
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? " ✅" : " ❌");

  client.setServer(mqtt_server, 1883);
  reconnectMQTT();

  // Watchdog
  timer = timerBegin(1);
  timerAttachInterrupt(timer, &resetModule);
  timerAlarm(timer, 180000000, false, 0);
  timerStart(timer);

  if (!scanAndConnect()) {
    Serial.println("❌ Connessione BLE iniziale fallita.");
    connected = false;
    nextBleTryMs = millis() + 5000UL;
  }
}

// ---------- Loop ----------
void loop() {
  // watchdog
  if (timer) timerWrite(timer, 0);

  // MQTT
  if (!client.connected()) reconnectMQTT();
  client.loop();

  // Se il link fisico è caduto, aggiorna stato (evita "connesso logico")
  if (connected && (!pClient || !pClient->isConnected())) {
    Serial.println("⚠️ BLE disconnesso fisicamente, aggiorno stato");
    connected = false;
  }

  // Riconnessione BLE con backoff
  if (!connected && millis() >= nextBleTryMs) {
    bool ok = scanAndConnect();
    if (ok) {
      bleFailStreak = 0;
    } else {
      bleFailStreak++;
      // ogni 3 fallimenti, reset "soft" dello stack BLE
      if (bleFailStreak % 3 == 0) {
        Serial.println("♻️ Soft reset stack BLE (deinit/init)");
        BLEDevice::deinit(true);
        delay(200);
        BLEDevice::init("");
      }
    }
    nextBleTryMs = millis() + 5000UL; // prossimo tentativo tra 5s
  }

  // Diagnostica periodica
  if (millis() - lastDebugReport >= 30000UL) {
    lastDebugReport = millis();
    char heapBuf[16]; snprintf(heapBuf, sizeof(heapBuf), "%lu", ESP.getFreeHeap());
    client.publish("Batteria/Duster/debug_heap", heapBuf);
    client.publish("Batteria/Duster/debug_ble", (connected && pClient && pClient->isConnected()) ? "BLE CONNESSO" : "BLE NON CONNESSO");
    if (lastBleTime > 0 && millis() - lastBleTime > 120000UL) {
      client.publish("Batteria/Duster/debug_ble_timeout", "NESSUNA NOTIFICA DA OLTRE 2 MINUTI");
    }
    client.publish("Batteria/Duster/debug_mqtt", client.connected() ? "MQTT OK" : "MQTT DISCONNESSO");

    // ---- Heap telemetry avanzata (ADD) ----
    uint32_t free8     = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    uint32_t largest8  = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    uint32_t minfree8  = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    if (free8 < heapMinEver) heapMinEver = free8;

    char buf32[16];
    snprintf(buf32, sizeof(buf32), "%lu", (unsigned long)largest8);
    client.publish("Batteria/Duster/debug_heap_largest", buf32, true);
    snprintf(buf32, sizeof(buf32), "%lu", (unsigned long)minfree8);
    client.publish("Batteria/Duster/debug_heap_min", buf32, true);

    Serial.print("🧪 DIAG | MQTT=");
    Serial.print(client.connected() ? "OK" : "NO");
    Serial.print(" | BLE=");
    Serial.print((connected && pClient && pClient->isConnected()) ? "OK" : "NO");
    Serial.print(" | Heap=");
    Serial.print(ESP.getFreeHeap());
    Serial.print(" | Largest=");
    Serial.print(largest8);
    Serial.print(" | MinEver=");
    Serial.println(minfree8);

    // ---- Heap-guard: solo se NON connesso (ADD) ----
    if (!connected) {
      uint32_t largest8_chk = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
      if (largest8_chk < HEAP_LARGEST_GUARD) {
        client.publish("Batteria/Duster/debug_heap_guard", "BLE deinit/init per heap frammentato");
        Serial.println("♻️ Heap-guard: largest block basso, deinit/init BLE (non connesso)");

        BLEDevice::deinit(true);
        delay(200);
        BLEDevice::init("");
        delay(100);
      }
    }
  }

  delay(10);
}
