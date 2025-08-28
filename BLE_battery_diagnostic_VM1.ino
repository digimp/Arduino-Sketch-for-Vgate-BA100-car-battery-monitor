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
const char* password   = "PSW";
const char* mqtt_server= "192.168.1.XXX";
const char* mqtt_user  = "mqtt_user";
const char* mqtt_pass  = "PSW";

WiFiClient espClient;
PubSubClient client(espClient);

// ---------- BLE target ----------
static const char* TARGET_MAC = "00:00:00:C3:72:68"; // BA100
static BLEUUID serviceUUID("0000ae00-0000-1000-8000-00805f9b34fb");
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
// static uint16_t bleFailStreak = 0; // (non usato)

// ---------- Heap monitor ----------
static uint32_t heapMinEver = UINT32_MAX;              // traccia il minimo storico
static const uint32_t HEAP_LARGEST_GUARD = 16 * 1024;  // soglia minima largest block (16KB)

// ---------- Wi-Fi watchdog (ADD) ----------
static unsigned long nextWiFiCheck = 0;
static const unsigned long WIFI_CHECK_MS = 5000; // 5s

static void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.print("📶 Reconnect WiFi");
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(ssid, password);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
    delay(300); Serial.print(".");
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? " ✅" : " ❌");
}

// ---------- BLE long-away refresh (ADD) ----------
static unsigned long lastBleRefresh = 0;
static const unsigned long BLE_REFRESH_COOLDOWN = 10UL * 60UL * 1000UL; // 10 min

// ---------- Client callbacks ----------
class MyClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient* c) override {
    (void)c;
  }
  void onDisconnect(BLEClient* c) override {
    (void)c;
    connected = false;
    nextBleTryMs = 0; // ritenta subito
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
  payload.reserve(length * 3); // evita riallocazioni ripetute
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

// ---------- Connessione BLE (priorità MAC diretto; scan breve fallback) ----------
bool scanAndConnect() {
  // Mantieni MQTT vivo PRIMA di eventuale scan sincrono
  client.loop();

  // (1) Tentativo IMMEDIATO via MAC
  if (pClient) { if (pClient->isConnected()) pClient->disconnect(); delete pClient; pClient = nullptr; }
  pClient = BLEDevice::createClient();
  static MyClientCallbacks cb;                 // (ADD) callback statico riusabile
  pClient->setClientCallbacks(&cb);

  BLEAddress addr(TARGET_MAC);
  Serial.println("🔗 Connessione diretta via MAC...");
  if (pClient->connect(addr)) {
    Serial.println("✅ Connesso, ricerca servizio...");
    BLERemoteService* pRemoteService = pClient->getService(serviceUUID);
    if (!pRemoteService) { Serial.println("❌ Servizio non trovato"); pClient->disconnect(); return false; }
    pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUID);
    if (!pRemoteCharacteristic){ Serial.println("❌ Caratteristica non trovata"); pClient->disconnect(); return false; }
    if (pRemoteCharacteristic->canNotify()) {
      delay(120);
      pRemoteCharacteristic->registerForNotify(notifyCallback);
      Serial.println("📡 Notifiche abilitate (direct).");
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

  // (2) Fallback: scan (10s) + connect tramite advertised device
  Serial.println("🔍 Scan BLE (10s) per advertised device...");
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(160);   // ~100ms
  pBLEScan->setWindow(120);     // ~75ms

  BLEScanResults* results = pBLEScan->start(10);  // Come nella diagnostic basica
  int n = results ? results->getCount() : 0;
  Serial.print("📡 Trovati "); Serial.print(n); Serial.println(" dispositivi BLE:");

  BLEAdvertisedDevice targetDev;
  bool found = false;
  BLEAddress targetAddr(TARGET_MAC);

  for (int i = 0; i < n; i++) {
    BLEAdvertisedDevice d = results->getDevice(i);
    // Log minimale (evita flood): stampa solo il target o 1-2 righe
    if (d.getAddress().equals(targetAddr)) {
      Serial.print(" - TARGET: "); Serial.println(d.toString().c_str());
      targetDev = d;
      found = true;
      break;
    }
  }
  // (lasciato invariato: per evitare il warning, rimuovere lo stop)
  // pBLEScan->stop();
  pBLEScan->clearResults();

  if (!found) {
    Serial.println("❌ Target non trovato nello scan breve");
    return false;
  }

  Serial.println("🔗 Connessione tramite advertised device...");
  if (!pClient->connect(&targetDev)) {
    Serial.println("❌ Connessione (advertised) fallita");
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
    delay(120);
    pRemoteCharacteristic->registerForNotify(notifyCallback);
    Serial.println("📡 Notifiche abilitate (advertised).");
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
  const char* sketchName = "BLE_battery_diagnostic_VM1";
  Serial.print("📄 Sketch attivo: ");
  Serial.println(sketchName);

  // Libera RAM del Bluetooth classico prima di inizializzare il BLE
  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

  BLEDevice::init("");

  BLEScan* bootScan = BLEDevice::getScan();
  bootScan->setActiveScan(true);
  bootScan->setInterval(160);          // ~100 ms
  bootScan->setWindow(120);            // ~75 ms

  // Wi-Fi: STA + no sleep
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

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
    nextBleTryMs = 0; // pronto a ritentare subito
  }
}

// ---------- Loop ----------
void loop() {
  // watchdog come sketch diagnostic basico
  if (timer && connected) timerWrite(timer, 0);

  // Wi-Fi watchdog (ADD)
  if (millis() >= nextWiFiCheck) { ensureWiFi(); nextWiFiCheck = millis() + WIFI_CHECK_MS; }

  // MQTT
  if (!client.connected()) reconnectMQTT();
  client.loop();

  // start funzioni reconnect diagnostic basic version

  if (connected && (!pClient || !pClient->isConnected())) {
    Serial.println("⚠️ BLE risulta connesso logicamente, ma disconnesso a livello fisico. Forzo riconnessione.");
    connected = false;
  }

  // Riconnessione BLE se necessario
  if (!connected) {
    Serial.println("🔁 Tentativo di riconnessione BLE...");
    connected = scanAndConnect();
    if (!connected) {
      Serial.println("❌ Retry BLE fallito, reboot fra 3 min se persiste.");
    }
  }

  if (connected && timer) {
    timerWrite(timer, 0);
  }

  // End funzioni reconnect diagnostic basic version

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

    // ---- Heap telemetry avanzata ----
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

    // ---- Heap-guard DISABILITATO ----
    /*
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
    */
  }

  delay(10);
}
