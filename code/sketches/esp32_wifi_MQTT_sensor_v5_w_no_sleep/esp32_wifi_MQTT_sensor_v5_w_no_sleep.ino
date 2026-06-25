#include <WiFi.h>
#include <PubSubClient.h>
#include "esp_timer.h"
#include "esp_bt.h"
#include "esp_system.h"

#define SENSOR_PIN 34   // Hall sensor (10k external pull-up)

// ===== CONFIG =====
#define BIN_SECONDS      300
#define NUM_BINS         12

// ISR debounce (mirror sleep version timing scale)
#define ISR_DEBOUNCE_US  1000000ULL   // 1s

// =====================
// WIFI / MQTT CONFIG
// =====================
const char* ssid        = "base2.4";
const char* password    = "samarajames";
const char* mqtt_server = "192.168.0.110";
const int   mqtt_port   = 1883;
const char* device_id   = "esp32_gas1";

// MQTT topic (switch same way as sleep code)
const char* MQTT_TOPIC = "metering/counts";

WiFiClient   espClient;
PubSubClient client(espClient);

// =====================
// STATE
// =====================
volatile uint32_t counts[NUM_BINS] = {0};
volatile uint32_t totalPulses      = 0;

uint32_t currentBin = 0;
uint64_t nextBin_us = 0;

uint32_t sendIndex = 0;
int      lastRSSI  = -999;


// ===============================================
// ISR — FALLING EDGE COUNTER
// Mirrors the sleep version: stable low → pulse counted
// No sleep debounce needed, just timing-based lockout.
// ===============================================
void IRAM_ATTR pulseISR() {
    static uint64_t lastPulse = 0;
    uint64_t now = esp_timer_get_time();

    // block repeat triggers for ~50ms
    if (now - lastPulse < ISR_DEBOUNCE_US) return;
    lastPulse = now;

    counts[currentBin]++;
    totalPulses++;
}


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
// JSON PUBLISH (identical to sleep version)
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
// BIN ROLLOVER + POSSIBLE UPLOAD
// Same architecture as sleep version
// ===============================================
void handleBinsAndUpload() {
    while (true) {
        uint64_t now = esp_timer_get_time();
        if (now < nextBin_us) break;

        Serial.printf("Bin Boundary: %u → %u\n", currentBin, currentBin + 1);
        currentBin++;
        nextBin_us += (uint64_t)BIN_SECONDS * 1000000ULL;

        // Final bin reached → upload & reset
        if (currentBin >= NUM_BINS) {
            Serial.println("FINAL BIN → Uploading…");

            setCpuFrequencyMhz(80);
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
            setCpuFrequencyMhz(40);

            // Reset all bins
            memset((void*)counts, 0, sizeof(counts));
            totalPulses = 0;
            currentBin  = 0;

            uint64_t now2 = esp_timer_get_time();
            nextBin_us = now2 + (uint64_t)BIN_SECONDS * 1000000ULL;

            Serial.println("UPLOAD DONE");
            break;
        }
    }
}


// ===============================================
// SETUP
// ===============================================
void setup() {
    Serial.begin(115200);
    delay(200);

    pinMode(SENSOR_PIN, INPUT);
    attachInterrupt(SENSOR_PIN, pulseISR, FALLING);

    // match sleep-version CPU baseline
    setCpuFrequencyMhz(40);

    // Kill Bluetooth
    esp_bt_controller_disable();
    btStop();

    uint64_t now = esp_timer_get_time();
    nextBin_us = now + (uint64_t)BIN_SECONDS * 1000000ULL;

    Serial.println("Setup complete (NO SLEEP, ISR-based counting)");
}


// ===============================================
// LOOP
// Just check bins periodically
// ===============================================
void loop() {
    handleBinsAndUpload();
    delay(1000);   // light idle
}
