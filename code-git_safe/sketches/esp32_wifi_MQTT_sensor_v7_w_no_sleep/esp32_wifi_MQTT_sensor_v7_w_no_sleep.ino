#include <WiFi.h>
#include <PubSubClient.h>
#include "esp_timer.h"
#include "esp_system.h"

#define SENSOR_PIN 34   // Hall sensor (10k external pull-up)

// ===== DEBUG =====
#define DEBUG_ENABLED  false      // set false to silence debug output

#if DEBUG_ENABLED
  #define DBG_PRINT(x)        Serial.print(x)
  #define DBG_PRINTLN(x)      Serial.println(x)
  #define DBG_PRINTF(...)     Serial.printf(__VA_ARGS__)
#else
  #define DBG_PRINT(x)
  #define DBG_PRINTLN(x)
  #define DBG_PRINTF(...)
#endif

// ===== CONFIG =====
#define BIN_SECONDS      300
#define NUM_BINS         12

// ISR debounce
#define ISR_DEBOUNCE_US  500000ULL   // 500ms

// reboot timer
#define REBOOT_INTERVAL_MS (24ULL * 60ULL * 60ULL * 1000ULL)
uint64_t bootMillis = 0;

//##################################################################
// LIVE MQTT PULSE DEBUG (optional troubleshooting mode)
//
// false = original behaviour
// true  = keep WiFi/MQTT connected and publish one MQTT message
//         every time a pulse is detected
//##################################################################
#define LIVE_PULSE_DEBUG_ENABLED false
const char* MQTT_PULSE_TOPIC = "metering/debug/pulse";
//##################################################################

// =====================
// WIFI / MQTT CONFIG
// =====================
const char* ssid        = "base2.4";
const char* password    = "x";    //this is for git
const char* mqtt_server = "192.168.0.110";
const int   mqtt_port   = 1883;
const char* device_id   = "esp32_gas1";

// MQTT topic
const char* MQTT_TOPIC = "metering/counts";

WiFiClient   espClient;
PubSubClient client(espClient);

// =====================
// STATE
// =====================
volatile uint32_t counts[NUM_BINS] = {0};
volatile uint32_t totalPulses      = 0;
volatile uint64_t lastPulseUs      = 0;

uint32_t currentBin = 0;
uint64_t nextBin_us = 0;

uint32_t sendIndex = 0;
int      lastRSSI  = -999;

//##################################################################
// Live pulse debug state
//##################################################################
volatile uint32_t pendingPulseDebugCount = 0;
unsigned long lastLiveReconnectAttemptMs = 0;
//##################################################################


// ===============================================
// DEBUG HELPERS
// ===============================================
void debugPrintArraySnapshot() {
    if (!DEBUG_ENABLED) return;

    noInterrupts();
    uint32_t localCounts[NUM_BINS];
    uint32_t localTotal = totalPulses;
    for (int i = 0; i < NUM_BINS; i++) localCounts[i] = counts[i];
    interrupts();

    DBG_PRINT("[DEBUG] counts=[");
    for (int i = 0; i < NUM_BINS; i++) {
        DBG_PRINT(localCounts[i]);
        if (i < NUM_BINS - 1) DBG_PRINT(",");
    }
    DBG_PRINT("], total=");
    DBG_PRINTLN(localTotal);
}


// ===============================================
// ISR — FALLING EDGE COUNTER
// ===============================================
void IRAM_ATTR pulseISR() {
    static uint64_t lastAcceptedPulse = 0;
    uint64_t now = esp_timer_get_time();

    if (now - lastAcceptedPulse < ISR_DEBOUNCE_US) return;
    lastAcceptedPulse = now;

    counts[currentBin]++;
    totalPulses++;
    lastPulseUs = now;

    //##################################################################
    // latch pulse for optional live MQTT debug
    //##################################################################
    if (LIVE_PULSE_DEBUG_ENABLED) {
        pendingPulseDebugCount++;
    }
    //##################################################################
}


// ===============================================
// WIFI + MQTT
// ===============================================
void wifiConnect() {
    DBG_PRINTF("[WIFI] Connecting to SSID '%s'\n", ssid);

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    for (int i = 0; i < 80 && WiFi.status() != WL_CONNECTED; i++) {
        delay(100);
        if (DEBUG_ENABLED && (i % 10 == 0)) {
            DBG_PRINTF("[WIFI] Waiting... status=%d elapsed=%d ms\n", WiFi.status(), (i + 1) * 100);
        }
    }

    if (WiFi.status() == WL_CONNECTED) {
        DBG_PRINTF("[WIFI] Connected. IP=%s RSSI=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    } else {
        DBG_PRINTF("[WIFI] FAILED to connect. Final status=%d\n", WiFi.status());
    }
}

bool mqttConnect() {
    client.setServer(mqtt_server, mqtt_port);

    DBG_PRINTF("[MQTT] Connecting to %s:%d as '%s'\n", mqtt_server, mqtt_port, device_id);
    bool ok = client.connect(device_id);

    if (ok) {
        DBG_PRINTLN("[MQTT] Connected.");
    } else {
        DBG_PRINTF("[MQTT] FAILED. client.state()=%d\n", client.state());
    }

    return ok;
}

void wifiOff() {
    DBG_PRINTLN("[WIFI] Shutting down Wi-Fi / MQTT");
    client.disconnect();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

//##################################################################
// Ensure WiFi/MQTT is connected in live pulse debug mode
//##################################################################
void ensureLiveDebugConnection() {
    if (!LIVE_PULSE_DEBUG_ENABLED) return;

    if (WiFi.status() == WL_CONNECTED && client.connected()) return;

    unsigned long nowMs = millis();
    if (nowMs - lastLiveReconnectAttemptMs < 5000UL) return;
    lastLiveReconnectAttemptMs = nowMs;

    if (WiFi.status() != WL_CONNECTED) {
        wifiConnect();
    }

    if (WiFi.status() == WL_CONNECTED && !client.connected()) {
        mqttConnect();
    }
}
//##################################################################


// ===============================================
// JSON PUBLISH
// ===============================================
void publishBins() {
    String json = "{\"device\":\"";
    json += device_id;
    json += "\",\"idx\":";
    json += sendIndex;
    json += ",\"rssi\":";
    json += lastRSSI;
    json += ",\"bin_s\":";
    json += BIN_SECONDS;
    json += ",\"total\":";
    json += totalPulses;
    json += ",\"counts\":[";

    for (int i = 0; i < NUM_BINS; i++) {
        json += counts[i];
        if (i < NUM_BINS - 1) json += ",";
    }
    json += "]}";

    DBG_PRINTLN("[MQTT] Publishing payload:");
    DBG_PRINTLN(json);

    bool ok = client.publish(MQTT_TOPIC, json.c_str());

    if (ok) {
        DBG_PRINTF("[MQTT] Publish OK to topic '%s'\n", MQTT_TOPIC);
    } else {
        DBG_PRINTF("[MQTT] Publish FAILED to topic '%s'\n", MQTT_TOPIC);
    }

    Serial.println(json);
    sendIndex++;
}

//##################################################################
// Publish live pulse debug messages if any are pending
//##################################################################
void publishPulseDebugIfNeeded() {
    if (!LIVE_PULSE_DEBUG_ENABLED) return;
    if (WiFi.status() != WL_CONNECTED) return;
    if (!client.connected()) return;

    while (true) {
        uint32_t pending;
        uint32_t totalSnapshot;
        uint32_t binSnapshot;
        uint64_t pulseUsSnapshot;

        noInterrupts();
        pending = pendingPulseDebugCount;
        totalSnapshot = totalPulses;
        binSnapshot = currentBin;
        pulseUsSnapshot = lastPulseUs;
        interrupts();

        if (pending == 0) break;

        String json = "{\"device\":\"";
        json += device_id;
        json += "\",\"pulse\":";
        json += totalSnapshot;
        json += ",\"bin\":";
        json += binSnapshot;
        json += ",\"time_us\":";
        json += (unsigned long long)pulseUsSnapshot;
        json += ",\"uptime_ms\":";
        json += millis();
        json += "}";

        bool ok = client.publish(MQTT_PULSE_TOPIC, json.c_str());

        if (ok) {
            DBG_PRINTLN("[LIVE] pulse debug sent");
            DBG_PRINTLN(json);

            noInterrupts();
            if (pendingPulseDebugCount > 0) {
                pendingPulseDebugCount--;
            }
            interrupts();
        } else {
            DBG_PRINTLN("[LIVE] pulse debug publish failed");
            break;
        }

        client.loop();
        delay(5);
    }
}
//##################################################################


// ===============================================
// BIN ROLLOVER + POSSIBLE UPLOAD
// ===============================================
void handleBinsAndUpload() {
    while (true) {
        uint64_t now = esp_timer_get_time();
        if (now < nextBin_us) break;

        DBG_PRINTF("[BIN] Boundary crossed: %u -> %u\n", currentBin, currentBin + 1);
        debugPrintArraySnapshot();

        currentBin++;
        nextBin_us += (uint64_t)BIN_SECONDS * 1000000ULL;

        if (currentBin >= NUM_BINS) {
            DBG_PRINTLN("[UPLOAD] Final bin reached. Starting upload sequence.");

            //##################################################################
            // In live debug mode keep WiFi/MQTT up continuously.
            // In normal mode use original connect/upload/disconnect behaviour.
            //##################################################################
            if (LIVE_PULSE_DEBUG_ENABLED) {
                ensureLiveDebugConnection();
            } else {
                wifiConnect();
                if (WiFi.status() == WL_CONNECTED && !client.connected()) {
                    mqttConnect();
                }
            }
            //##################################################################

            if (WiFi.status() == WL_CONNECTED && client.connected()) {
                lastRSSI = WiFi.RSSI();
                publishBins();

                unsigned long t1 = millis();
                while (millis() - t1 < 300) {
                    client.loop();

                    //##################################################################
                    // Flush any live pulse debug messages while connected
                    //##################################################################
                    publishPulseDebugIfNeeded();
                    //##################################################################

                    delay(10);
                }
            } else {
                DBG_PRINTLN("[UPLOAD] Upload skipped because Wi-Fi or MQTT was not connected.");
            }

            //##################################################################
            // Only shut WiFi down in normal mode
            //##################################################################
            if (!LIVE_PULSE_DEBUG_ENABLED) {
                wifiOff();
            }
            //##################################################################

            memset((void*)counts, 0, sizeof(counts));
            totalPulses = 0;
            currentBin  = 0;

            uint64_t now2 = esp_timer_get_time();
            nextBin_us = now2 + (uint64_t)BIN_SECONDS * 1000000ULL;

            DBG_PRINTLN("[UPLOAD] Upload cycle complete. Bins reset.");

            if ((uint64_t)millis() - bootMillis >= REBOOT_INTERVAL_MS) {
                DBG_PRINTLN("[SYSTEM] 24h elapsed -> rebooting now");
                delay(100);
                esp_restart();
            }

            break;
        }
    }
}


// ===============================================
// SETUP
// ===============================================
void setup() {
    bootMillis = millis();

    Serial.begin(115200);
    delay(500);

    DBG_PRINTLN();
    DBG_PRINTLN("======================================");
    DBG_PRINTLN("[BOOT] ESP32 gas meter starting");
    DBG_PRINTF("[BOOT] device_id=%s\n", device_id);
    DBG_PRINTF("[BOOT] BIN_SECONDS=%d, NUM_BINS=%d\n", BIN_SECONDS, NUM_BINS);
    DBG_PRINTF("[BOOT] ISR_DEBOUNCE_US=%llu\n", ISR_DEBOUNCE_US);
    DBG_PRINTF("[BOOT] CPU freq at start=%u MHz\n", getCpuFrequencyMhz());
    DBG_PRINTF("[BOOT] Free heap=%u\n", ESP.getFreeHeap());
    DBG_PRINTLN("======================================");

    pinMode(SENSOR_PIN, INPUT);
    delay(20);

    DBG_PRINTF("[SENSOR] SENSOR_PIN=%d initial state=%d\n", SENSOR_PIN, digitalRead(SENSOR_PIN));
    DBG_PRINTLN("[SENSOR] Attaching FALLING-edge interrupt");

    attachInterrupt(SENSOR_PIN, pulseISR, FALLING);

    uint64_t now = esp_timer_get_time();
    nextBin_us = now + (uint64_t)BIN_SECONDS * 1000000ULL;

    //##################################################################
    // Bring up WiFi/MQTT at boot if live pulse debug mode is enabled
    //##################################################################
    if (LIVE_PULSE_DEBUG_ENABLED) {
        ensureLiveDebugConnection();
    }
    //##################################################################

    DBG_PRINTF("[BIN] First boundary in %d seconds\n", BIN_SECONDS);
    DBG_PRINTLN("[BOOT] Setup complete (NO SLEEP, ISR-based counting)");
}


// ===============================================
// LOOP
// ===============================================
void loop() {
    //##################################################################
    // Optional live pulse MQTT debug path
    //##################################################################
    if (LIVE_PULSE_DEBUG_ENABLED) {
        ensureLiveDebugConnection();

        if (client.connected()) {
            client.loop();
            publishPulseDebugIfNeeded();
        }

        handleBinsAndUpload();
        delay(50);
        return;
    }
    //##################################################################

    handleBinsAndUpload();
    delay(1000);
}