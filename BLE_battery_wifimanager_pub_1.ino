/*
 * BLE Battery Monitor — ESP32-C3 → MQTT → Home Assistant
 * --------------------------------------------------------
 * Legge notifiche BLE da un sender BA100 e le pubblica via MQTT.
 * Al primo avvio (o se la rete WiFi non è disponibile) genera un
 * hotspot "ESP32-Duster" per la configurazione via browser.
 *
 * CONFIGURAZIONE INIZIALE:
 *   1. Modifica TARGET_MAC con il MAC del tuo sender BLE e parametri serviceUUID e charUUID (usare app nRF Connect)
 *   2. Flasha lo sketch sulla ESP32-C3
 *   3. Connettiti all'AP "ESP32-Duster" (password: configesp32)
 *   4. Apri il browser su 192.168.4.1 e inserisci:
 *      - Rete WiFi e password
 *      - MQTT server, user, password
 *   5. Salva — la ESP si riavvia e si connette automaticamente
 *
 * LIBRERIE RICHIESTE (Library Manager):
 *   - WiFiManager (tzapu)
 *   - PubSubClient (knolleary)
 *
 * BOARD: ESP32C3 Dev Module
 *   - USB CDC On Boot: Enabled
 *   - Flash Size: 4MB
 *   - Partition Scheme: No OTA 2MB app / 2MB SPIFFS
 *   - CPU Frequency: 160MHz
 *   - Core Espressif: 3.2.1
 * --------------------------------------------------------
 */

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include "esp32-hal-timer.h"
#include "esp_bt.h"
#include "esp_heap_caps.h"

// ---------- Preferences (NVS) ----------
Preferences prefs; 

// ---------- Parametri configurabili (con default) ----------
char mqtt_server[40] = "0.0.0.0";
char mqtt_user[32]   = "mqtt_user";
char mqtt_pass[32]   = "your_password";

// ---------- WiFi / MQTT ----------
WiFiClient espClient;
PubSubClient client(espClient);

// ---------- BLE target ----------
static const char* TARGET_MAC = "XX:XX:XX:XX:XX:XX"; 
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

// ---------- Heap monitor ----------
static uint32_t heapMinEver = UINT32_MAX;
static const uint32_t HEAP_LARGEST_GUARD = 16 * 1024;

// ---------- Wi-Fi watchdog ----------
static unsigned long nextWiFiCheck = 0;
static const unsigned long WIFI_CHECK_MS = 5000;

// ---------- Wi-Fi reconnect (no AP mode — credenziali già in flash) ----------  
static void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.print("📶 Reconnect WiFi");
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(); // WiFiManager ha già salvato ssid/password in flash
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
    delay(300); Serial.print(".");
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? " ✅" : " ❌");
}

// ---------- BLE long-away refresh ----------
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
    nextBleTryMs = 0;
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

  String payload;
  payload.reserve(length * 3);
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

// ---------- Connessione BLE ----------  
bool scanAndConnect() {
  // Mantiene MQTT attivo prima di eventuale scan sincrono
  client.loop();

  // (1) Tentativo IMMEDIATO via MAC
  if (pClient) { if (pClient->isConnected()) pClient->disconnect(); delete pClient; pClient = nullptr; }
  pClient = BLEDevice::createClient();
  static MyClientCallbacks cb;
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
  pBLEScan->setInterval(160);
  pBLEScan->setWindow(120);

  BLEScanResults* results = pBLEScan->start(10);
  int n = results ? results->getCount() : 0;
  Serial.print("📡 Trovati "); Serial.print(n); Serial.println(" dispositivi BLE:");

  BLEAdvertisedDevice targetDev;
  bool found = false;
  BLEAddress targetAddr(TARGET_MAC);

  for (int i = 0; i < n; i++) {
    BLEAdvertisedDevice d = results->getDevice(i);
    if (d.getAddress().equals(targetAddr)) {
      Serial.print(" - TARGET: "); Serial.println(d.toString().c_str());
      targetDev = d;
      found = true;
      break;
    }
  }
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

// ---------- Carica parametri MQTT da NVS ----------
void loadPrefs() { 
  prefs.begin("espconfig", true);
  if (prefs.isKey("mqtt_server")) prefs.getString("mqtt_server", mqtt_server, sizeof(mqtt_server));
  if (prefs.isKey("mqtt_user"))   prefs.getString("mqtt_user",   mqtt_user,   sizeof(mqtt_user));
  if (prefs.isKey("mqtt_pass"))   prefs.getString("mqtt_pass",   mqtt_pass,   sizeof(mqtt_pass));
  prefs.end();
  Serial.println("📦 Config caricata da NVS");
  Serial.print("   MQTT server: "); Serial.println(mqtt_server);
}                                                               

// ---------- Salva parametri MQTT in NVS ----------
void savePrefs() {  
  prefs.begin("espconfig", false); 
  prefs.putString("mqtt_server", mqtt_server);
  prefs.putString("mqtt_user",   mqtt_user);
  prefs.putString("mqtt_pass",   mqtt_pass);
  prefs.end();
  Serial.println("💾 Config salvata in NVS");
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  delay(600);
  const char* sketchName = "BLE_battery_wifimanager_pub_v1";
  Serial.print("📄 Sketch attivo: ");
  Serial.println(sketchName);

  // Carica parametri MQTT da NVS (sovrascrive i default se presenti)
  loadPrefs();

  // Libera RAM del Bluetooth prima di inizializzare il BLE
  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

  BLEDevice::init("");

  BLEScan* bootScan = BLEDevice::getScan();
  bootScan->setActiveScan(true);
  bootScan->setInterval(160);
  bootScan->setWindow(120);

  // ----- FASE WiFi: WiFiManager -----
  // BLE è già init ma AP mode sarà breve e solo al primo boot
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  WiFiManager wm;

  // Parametri custom nella pagina AP
  WiFiManagerParameter p_mqtt_server("mqtt_srv",  "MQTT Server",  mqtt_server, 40);
  WiFiManagerParameter p_mqtt_user  ("mqtt_user", "MQTT User",    mqtt_user,   32);
  WiFiManagerParameter p_mqtt_pass  ("mqtt_pass", "MQTT Password",mqtt_pass,   32);

  wm.addParameter(&p_mqtt_server);
  wm.addParameter(&p_mqtt_user);
  wm.addParameter(&p_mqtt_pass);

  wm.setConfigPortalTimeout(180);
  wm.setTitle("ESP32 Duster Config");

  Serial.println("📶 WiFiManager: tentativo connessione...");
  bool wifiOk = wm.autoConnect("ESP32-Duster", "your_ap_password");

  if (!wifiOk) { 
    Serial.println("⏱️ WiFiManager timeout, riavvio...");
    delay(500);
    esp_restart();
  } 

  Serial.println("✅ WiFi connesso: " + WiFi.localIP().toString());

  // Salva parametri MQTT se modificati via pagina AP
  bool changed = false;
  if (strcmp(p_mqtt_server.getValue(), mqtt_server) != 0) { strncpy(mqtt_server, p_mqtt_server.getValue(), 40); changed = true; }
  if (strcmp(p_mqtt_user.getValue(),   mqtt_user)   != 0) { strncpy(mqtt_user,   p_mqtt_user.getValue(),   32); changed = true; }
  if (strcmp(p_mqtt_pass.getValue(),   mqtt_pass)   != 0) { strncpy(mqtt_pass,   p_mqtt_pass.getValue(),   32); changed = true; }
  if (changed) savePrefs();

  client.setServer(mqtt_server, 1883); // usa mqtt_server da NVS
  reconnectMQTT();

  // Watchdog
  timer = timerBegin(1);
  timerAttachInterrupt(timer, &resetModule);
  timerAlarm(timer, 180000000, false, 0);
  timerStart(timer);

  if (!scanAndConnect()) {
    Serial.println("❌ Connessione BLE iniziale fallita.");
    connected = false;
    nextBleTryMs = 0;
  }
}

// ---------- Loop ---------- 
void loop() {
  // watchdog come sketch diagnostic basico
  if (timer && connected) timerWrite(timer, 0);

  // Wi-Fi watchdog
  if (millis() >= nextWiFiCheck) { ensureWiFi(); nextWiFiCheck = millis() + WIFI_CHECK_MS; }

  // MQTT
  if (!client.connected()) reconnectMQTT();
  client.loop();


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

  }

  delay(10);
}
