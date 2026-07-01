#include <WiFi.h>
#include <PubSubClient.h>
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_idf_version.h"

#define SENSOR_PIN 34   // Hall sensor (10k external pull-up)

// ===== DEBUG =====
//debug test heatbeat so i can see the thing is working
#define SERIAL_HEARTBEAT_ENABLED false
#define SERIAL_HEARTBEAT_MS      5000UL

//regular debug
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
//##################################################################
// LIVE MQTT PULSE DEBUG (optional troubleshooting mode)
//
// false = original behaviour
// true  = keep WiFi/MQTT connected and publish one MQTT message
//         every time a pulse is detected
//##################################################################
#define LIVE_PULSE_DEBUG_ENABLED false
//##################################################################

// ===== CONFIG =====
#define BIN_SECONDS      300
#define NUM_BINS         12

// Keep this false for the same behaviour as your current sketch.
// Set true later if you want to test not power-cycling WiFi every hour.
#define KEEP_WIFI_ON_BETWEEN_UPLOADS false

// If no hourly upload attempt happens for this long, reboot.
// This catches scheduler/main-loop wedges without being too aggressive.
#define UPLOAD_OVERDUE_MS (90UL * 60UL * 1000UL)

// Hardware/task watchdog. This catches real lockups where loop() stops
// returning because code is stuck in WiFi/MQTT or some other blocking path.
#define HARD_WDT_ENABLED   1
#define HARD_WDT_TIMEOUT_S 30
bool hardWdtTaskRegistered = false;

// ISR debounce
#define ISR_DEBOUNCE_US  500000ULL   // 500ms

// reboot timer
#define REBOOT_INTERVAL_MS (24ULL * 60ULL * 60ULL * 1000ULL)
uint64_t bootMillis = 0;
unsigned long lastUploadAttemptMs = 0;

// =====================
// WIFI / MQTT CONFIG
// =====================
const char* ssid        = "base2.4";
const char* password    = "x";
const char* mqtt_server = "192.168.0.110";
const int   mqtt_port   = 1883;
const char* device_id   = "esp32_gas1";

// MQTT topic
const char* MQTT_PULSE_TOPIC = "metering/debug/pulse";
const char* MQTT_TOPIC = "metering/counts";

WiFiClient   espClient;
PubSubClient client(espClient);

// =====================
// STATE
// =====================
volatile uint32_t counts[NUM_BINS] = {0};
volatile uint32_t totalPulses      = 0;
volatile uint64_t lastPulseUs      = 0;

// currentBin is read inside the ISR, so keep it volatile.
volatile uint32_t currentBin = 0;
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
    uint32_t localBin = currentBin;
    interrupts();

    DBG_PRINT("[DEBUG] bin=");
    DBG_PRINT(localBin);
    DBG_PRINT(", counts=[");
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

    // Defensive guard: valid bins are 0..NUM_BINS-1 only.
    // The old sketch could briefly expose currentBin == NUM_BINS
    // during upload, causing counts[12] out-of-bounds corruption.
    uint32_t bin = currentBin;
    if (bin < NUM_BINS) {
        counts[bin]++;
        totalPulses++;
        lastPulseUs = now;
    }

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
    if (WiFi.status() == WL_CONNECTED) return;

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
    if (client.connected()) return true;

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
    DBG_PRINTLN("[WIFI] Shutting down MQTT");
    client.disconnect();

    if (KEEP_WIFI_ON_BETWEEN_UPLOADS) {
        DBG_PRINTLN("[WIFI] Keeping WiFi on between uploads");
        return;
    }

    DBG_PRINTLN("[WIFI] Shutting down WiFi");
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
// BIN SNAPSHOT
// ===============================================
void snapshotAndResetBins(uint32_t outCounts[NUM_BINS], uint32_t &outTotal) {
    uint64_t now = esp_timer_get_time();

    noInterrupts();
    for (int i = 0; i < NUM_BINS; i++) {
        outCounts[i] = counts[i];
        counts[i] = 0;
    }
    outTotal = totalPulses;
    totalPulses = 0;
    currentBin = 0;
    interrupts();

    // Start the next hour immediately. Pulses during WiFi/MQTT upload
    // are counted into bin 0 of the next hour, not lost or written OOB.
    nextBin_us = now + (uint64_t)BIN_SECONDS * 1000000ULL;
}


// ===============================================
// JSON PUBLISH
// ===============================================
bool publishBinsSnapshot(const uint32_t localCounts[NUM_BINS], uint32_t localTotal) {
    char json[384];
    int pos = 0;

    pos += snprintf(json + pos, sizeof(json) - pos,
                    "{\"device\":\"%s\",\"idx\":%lu,\"rssi\":%d,\"bin_s\":%d,\"total\":%lu,\"counts\":[",
                    device_id,
                    (unsigned long)sendIndex,
                    lastRSSI,
                    BIN_SECONDS,
                    (unsigned long)localTotal);

    for (int i = 0; i < NUM_BINS; i++) {
        pos += snprintf(json + pos, sizeof(json) - pos,
                        "%lu%s",
                        (unsigned long)localCounts[i],
                        (i < NUM_BINS - 1) ? "," : "");
    }

    pos += snprintf(json + pos, sizeof(json) - pos, "]}");

    if (pos < 0 || pos >= (int)sizeof(json)) {
        DBG_PRINTLN("[MQTT] JSON buffer too small; publish skipped");
        Serial.println("{\"error\":\"json_buffer_too_small\"}");
        return false;
    }

    DBG_PRINTLN("[MQTT] Publishing payload:");
    DBG_PRINTLN(json);

    bool ok = client.publish(MQTT_TOPIC, json);

    if (ok) {
        DBG_PRINTF("[MQTT] Publish OK to topic '%s'\n", MQTT_TOPIC);
    } else {
        DBG_PRINTF("[MQTT] Publish FAILED to topic '%s'\n", MQTT_TOPIC);
    }

    Serial.println(json);
    sendIndex++;
    return ok;
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
// SAFETY / LIVENESS REBOOT CHECKS
// ===============================================
void checkRebootConditions() {
    if ((uint64_t)millis() - bootMillis >= REBOOT_INTERVAL_MS) {
        DBG_PRINTLN("[SYSTEM] 24h elapsed -> rebooting now");
        delay(100);
        esp_restart();
    }

    if (millis() - lastUploadAttemptMs >= UPLOAD_OVERDUE_MS) {
        DBG_PRINTLN("[SYSTEM] Upload overdue -> rebooting now");
        delay(100);
        esp_restart();
    }
}


// ===============================================
// HARD TASK WATCHDOG
// ===============================================
void setupHardWatchdog() {
    hardWdtTaskRegistered = false;

#if !HARD_WDT_ENABLED
    return;
#endif

    // Important: do NOT blindly call esp_task_wdt_init().
    // On Arduino-ESP32 the TWDT may already be initialised by the core.
    // Calling init() anyway prints:
    //   task_wdt: esp_task_wdt_init(...): TWDT already initialized
    // even if we later handle the returned error.
    esp_err_t status = esp_task_wdt_status(NULL);

#if ESP_IDF_VERSION_MAJOR >= 5
    esp_task_wdt_config_t wdt_config = {};
    wdt_config.timeout_ms = HARD_WDT_TIMEOUT_S * 1000;
    wdt_config.idle_core_mask = (1 << portNUM_PROCESSORS) - 1;
    wdt_config.trigger_panic = true;
#endif

    if (status == ESP_ERR_INVALID_STATE) {
        // TWDT is not running yet, so it is safe to initialise it.
#if ESP_IDF_VERSION_MAJOR >= 5
        esp_err_t err = esp_task_wdt_init(&wdt_config);
#else
        esp_err_t err = esp_task_wdt_init(HARD_WDT_TIMEOUT_S, true);
#endif
        if (err != ESP_OK) {
            DBG_PRINTF("[WDT] init failed: %d\n", err);
            return;
        }
    }
#if ESP_IDF_VERSION_MAJOR >= 5
    else {
        // TWDT is already running. Reconfigure it instead of calling init().
        esp_err_t err = esp_task_wdt_reconfigure(&wdt_config);

        if (err == ESP_OK) {
            DBG_PRINTF("[WDT] reconfigured: %d seconds\n", HARD_WDT_TIMEOUT_S);
        } else {
            DBG_PRINTF("[WDT] reconfigure failed: %d\n", err);
            // Do not return here. Still try to subscribe the loop task below.
        }
    }
#endif

    // If current task is already subscribed, we are done.
    status = esp_task_wdt_status(NULL);
    if (status == ESP_OK) {
        hardWdtTaskRegistered = true;
        DBG_PRINTF("[WDT] loop task already subscribed: %d seconds\n", HARD_WDT_TIMEOUT_S);
        return;
    }

    // TWDT is running, but this Arduino loop task is not subscribed yet.
    // Subscribe it before ever calling esp_task_wdt_reset().
    esp_err_t err = esp_task_wdt_add(NULL);
    if (err == ESP_OK || esp_task_wdt_status(NULL) == ESP_OK) {
        hardWdtTaskRegistered = true;
        DBG_PRINTF("[WDT] loop task subscribed: %d seconds\n", HARD_WDT_TIMEOUT_S);
        return;
    }

    DBG_PRINTF("[WDT] loop task subscribe failed: %d\n", err);
}

void feedHardWatchdog() {
#if HARD_WDT_ENABLED
    if (hardWdtTaskRegistered) {
        esp_task_wdt_reset();
    }
#endif
}


// ===============================================
// BIN ROLLOVER + POSSIBLE UPLOAD
// ===============================================
void handleBinsAndUpload() {
    while (true) {
        uint64_t now = esp_timer_get_time();
        if (now < nextBin_us) break;

        uint32_t localBin;
        noInterrupts();
        localBin = currentBin;
        interrupts();

        DBG_PRINTF("[BIN] Boundary crossed: %u -> %u\n", localBin, localBin + 1);
        debugPrintArraySnapshot();

        if (localBin + 1 >= NUM_BINS) {
            DBG_PRINTLN("[UPLOAD] Final bin reached. Snapshotting and starting upload sequence.");

            uint32_t uploadCounts[NUM_BINS];
            uint32_t uploadTotal = 0;
            snapshotAndResetBins(uploadCounts, uploadTotal);

            lastUploadAttemptMs = millis();

            //##################################################################
            // In live debug mode keep WiFi/MQTT up continuously.
            // In normal mode use the configured connect/upload/disconnect behaviour.
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
                publishBinsSnapshot(uploadCounts, uploadTotal);

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

            DBG_PRINTLN("[UPLOAD] Upload cycle complete. Bins already reset for next hour.");
            checkRebootConditions();
            break;
        } else {
            noInterrupts();
            currentBin = localBin + 1;
            interrupts();

            nextBin_us += (uint64_t)BIN_SECONDS * 1000000ULL;
        }
    }
}



void serialHeartbeat() {
#if SERIAL_HEARTBEAT_ENABLED
    static unsigned long lastHeartbeatMs = 0;

    if (millis() - lastHeartbeatMs >= SERIAL_HEARTBEAT_MS) {
        lastHeartbeatMs = millis();

        noInterrupts();
        uint32_t localBin = currentBin;
        uint32_t localTotal = totalPulses;
        uint64_t localLastPulseUs = lastPulseUs;
        interrupts();

        Serial.printf("[ALIVE] ms=%lu bin=%lu total=%lu lastPulseUs=%llu wifi=%d mqtt=%d heap=%u\n",
                      (unsigned long)millis(),
                      (unsigned long)localBin,
                      (unsigned long)localTotal,
                      (unsigned long long)localLastPulseUs,
                      WiFi.status(),
                      client.connected(),
                      ESP.getFreeHeap());
    }
#endif
}


// ===============================================
// SETUP
// ===============================================
void setup() {
    bootMillis = millis();
    lastUploadAttemptMs = millis();

    Serial.begin(115200);
    delay(5000);

    DBG_PRINTLN();
    DBG_PRINTLN("======================================");
    DBG_PRINTLN("[BOOT] ESP32 gas meter starting");
    DBG_PRINTF("[BOOT] device_id=%s\n", device_id);
    DBG_PRINTF("[BOOT] BIN_SECONDS=%d, NUM_BINS=%d\n", BIN_SECONDS, NUM_BINS);
    DBG_PRINTF("[BOOT] ISR_DEBOUNCE_US=%llu\n", ISR_DEBOUNCE_US);
    DBG_PRINTF("[BOOT] KEEP_WIFI_ON_BETWEEN_UPLOADS=%d\n", KEEP_WIFI_ON_BETWEEN_UPLOADS);
    DBG_PRINTF("[BOOT] CPU freq at start=%u MHz\n", getCpuFrequencyMhz());
    DBG_PRINTF("[BOOT] Free heap=%u\n", ESP.getFreeHeap());
    DBG_PRINTLN("======================================");

    WiFi.persistent(false);
    WiFi.setSleep(false);
    client.setSocketTimeout(5);     // limit MQTT connect/socket blocking
    client.setBufferSize(384);      // match fixed JSON buffer size

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

    setupHardWatchdog();

    DBG_PRINTF("[BIN] First boundary in %d seconds\n", BIN_SECONDS);
    DBG_PRINTLN("[BOOT] Setup complete (NO SLEEP, ISR-based counting)");
}


// ===============================================
// LOOP
// ===============================================
void loop() {
    // Feed the hard task watchdog only if this task was actually subscribed.
    // Calling esp_task_wdt_reset() when the task was not added causes:
    // "task_wdt: esp_task_wdt_reset(...): task not found".
    feedHardWatchdog();

    serialHeartbeat();

    checkRebootConditions();

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
