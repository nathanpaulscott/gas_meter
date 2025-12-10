#include <WiFi.h>
#include <PubSubClient.h>
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_bt.h"

#define SENSOR_PIN 34   // Hall sensor, external 10k pull-up

// ===== CONFIG =====
#define BIN_SECONDS       300      
#define NUM_BINS          12

// Debounce: after wake, pin must stay LOW this long
#define DEBOUNCE_LOW_US   (50000ULL)      // 50 ms

// Must stay HIGH this long before we go back to sleep
#define HIGH_STABLE_US    (10000000ULL)   // 10 s

// =====================
const char* ssid        = "base2.4";
const char* password    = "samarajames";
const char* mqtt_server = "192.168.0.110";
const int   mqtt_port   = 1883;
const char* device_id   = "esp32_gas1";

// MQTT topic (switch here for test/prod)
const char* MQTT_TOPIC = "metering/counts";  

WiFiClient   espClient;
PubSubClient client(espClient);

// STATE
volatile uint32_t counts[NUM_BINS] = {0};
volatile uint32_t totalPulses      = 0;

uint32_t currentBin = 0;
uint64_t nextBin_us = 0;

uint32_t sendIndex  = 0;
int      lastRSSI   = -999;

// ===============================================
// WIFI + MQTT
// ===============================================
void wifiConnect() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    for (int i = 0; i < 80 && WiFi.status() != WL_CONNECTED; i++) {
        delay(100);
    }
}

bool mqttConnect() {
    client.setServer(mqtt_server, mqtt_port);
    return client.connect(device_id);
}

void wifiOff() {
    client.disconnect();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

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

    client.publish(MQTT_TOPIC, json.c_str());
    Serial.println(json);

    sendIndex++;
}

// ===============================================
// HANDLE BIN ROLLOVER + POSSIBLE UPLOAD
// ===============================================
void handleBinsAndUpload() {
    while (true) {
        uint64_t now = esp_timer_get_time();
        if (now < nextBin_us) break;   // no rollover due yet

        Serial.printf("Bin Boundary: %u → %u\n", currentBin, currentBin + 1);
        currentBin++;
        nextBin_us += (uint64_t)BIN_SECONDS * 1000000ULL;

        // final bin → upload
        if (currentBin >= NUM_BINS) {
            Serial.println("FINAL BIN → Uploading…");

            setCpuFrequencyMhz(80);  // bump for WiFi
            wifiConnect();
            if (WiFi.status() == WL_CONNECTED && mqttConnect()) {
                lastRSSI = WiFi.RSSI();
                publishBins();

                unsigned long t1 = millis();
                while (millis() - t1 < 300) {
                    client.loop();
                    delay(10);
                }
            }

            wifiOff();
            setCpuFrequencyMhz(40);  // back down

            // Reset bins
            memset((void*)counts, 0, sizeof(counts));
            totalPulses = 0;
            currentBin  = 0;

            uint64_t now2 = esp_timer_get_time();
            nextBin_us = now2 + (uint64_t)BIN_SECONDS * 1000000ULL;

            Serial.println("UPLOAD DONE");
            break;   // after reset, leave
        }
    }
}

// ===============================================
// LOW-SIDE DEBOUNCE & PULSE COUNT
// Returns true if we counted a pulse this wake.
// ===============================================
bool handlePulseWake() {
    bool     validLow = true;
    uint64_t start    = esp_timer_get_time();

    // LOW-side debounce: must stay LOW for DEBOUNCE_LOW_US
    while (esp_timer_get_time() - start < DEBOUNCE_LOW_US) {
        if (digitalRead(SENSOR_PIN) != LOW) {
            validLow = false;
            break;
        }
        delay(1);   // 1 ms sample spacing
    }

    if (validLow && digitalRead(SENSOR_PIN) == LOW) {
        counts[currentBin]++;
        totalPulses++;
        Serial.printf("Debounced pulse in bin %u (total=%u)\n",
                      currentBin, totalPulses);
        return true;
    } else {
        Serial.println("Spurious wake (pin not stable LOW) → no pulse");
        return false;
    }
}

// ===============================================
// WAIT UNTIL GPIO HIGH IS STABLE FOR HIGH_STABLE_US
// ===============================================
void waitForHighStable() {
    uint64_t highStart_us = 0;

    while (true) {
        int       level = digitalRead(SENSOR_PIN);
        uint64_t  t_now = esp_timer_get_time();

        if (level == HIGH) {
            if (highStart_us == 0) {
                highStart_us = t_now;   // first HIGH seen
            }
            if (t_now - highStart_us >= HIGH_STABLE_US) {
                Serial.println("Magnet clear (HIGH stable) → can sleep");
                break;
            }
        } else {
            // went LOW again → restart high-stable timer
            highStart_us = 0;
        }

        delay(50);  // gentle polling
    }
}

// ===============================================
// SLEEP (wake on GPIO34 LOW OR timer)
// ===============================================
void goToSleep(uint64_t sleep_us) {
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    // Wake when SENSOR_PIN goes LOW
    esp_sleep_enable_ext1_wakeup(1ULL << SENSOR_PIN, ESP_EXT1_WAKEUP_ALL_LOW);

    // Wake for next bin boundary
    esp_sleep_enable_timer_wakeup(sleep_us);

    esp_light_sleep_start();
}

// ===============================================
// SETUP
// ===============================================
void setup() {
    Serial.begin(115200);
    delay(200);

    pinMode(SENSOR_PIN, INPUT);

    // Throttle CPU when idle
    setCpuFrequencyMhz(40);

    // Kill Bluetooth
    esp_bt_controller_disable();
    btStop();

    uint64_t now = esp_timer_get_time();
    nextBin_us = now + (uint64_t)BIN_SECONDS * 1000000ULL;

    Serial.println("Setup complete (WAKE-ON-LOW + BIN TIMER, low+high debounce)");
}

// ===============================================
// LOOP
// ===============================================
void loop() {
    uint64_t now   = esp_timer_get_time();
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

    // 1. First catch up any bin rollovers / uploads
    handleBinsAndUpload();

    bool pulseThisWake = false;

    // 2. PULSE EVENT (woke on LOW)
    if (cause == ESP_SLEEP_WAKEUP_EXT1) {
        pulseThisWake = handlePulseWake();
    }

    // 3. If we counted a pulse this wake:
    //    WAIT FOR HIGH-STABLE BEFORE SLEEP
    if (pulseThisWake) {
        waitForHighStable();
    }

    // 4. Before sleeping, re-check bins in case the HIGH-wait crossed a boundary
    handleBinsAndUpload();

    // 5. Sleep until next bin boundary
    uint64_t now3     = esp_timer_get_time();
    uint64_t sleep_us = (nextBin_us > now3) ? (nextBin_us - now3) : 1000ULL;
    if ((int64_t)sleep_us < 1000) sleep_us = 1000;   // enforce minimum

    goToSleep(sleep_us);
}