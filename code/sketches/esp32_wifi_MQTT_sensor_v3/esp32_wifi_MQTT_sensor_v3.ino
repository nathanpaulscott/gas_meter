#include <WiFi.h>
#include <PubSubClient.h>
#include "esp_timer.h"

#define SENSOR_PIN 34   // Hall sensor, external 10k pull-up

// ===== CONFIG =====
#define BIN_SECONDS      20
#define NUM_BINS         5

// =====================
const char* ssid        = "base2.4";
const char* password    = "samarajames";
const char* mqtt_server = "192.168.0.110";
const int   mqtt_port   = 1883;
const char* device_id   = "esp32_gas1";

WiFiClient espClient;
PubSubClient client(espClient);

// STATE
volatile uint32_t counts[NUM_BINS] = {0};
volatile uint32_t totalPulses      = 0;

uint32_t currentBin = 0;

uint64_t periodStart_us = 0;
uint64_t nextBin_us     = 0;

int lastRSSI   = -999;
uint32_t sendIndex = 0;

// =============================
// ISR — FALLING EDGE (real pulses only)
// =============================
void IRAM_ATTR pulseISR() {
    static uint64_t lastPulse = 0;
    uint64_t now = esp_timer_get_time();

    // 0.5s debounce — gas meter holds LOW for long time
    if (now - lastPulse < 500000ULL) return;
    lastPulse = now;

    counts[currentBin]++;
    totalPulses++;
}

// =============================
// WIFI + MQTT
// =============================
void wifiConnect() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    for (int i = 0; i < 80 && WiFi.status() != WL_CONNECTED; i++)
        delay(100);
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

// =============================
// JSON PUBLISH
// =============================
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

    client.publish("metering/counts", json.c_str());
    Serial.println(json);

    sendIndex++;
}

// =============================
// SETUP
// =============================
void setup() {
    Serial.begin(115200);
    delay(200);

    pinMode(SENSOR_PIN, INPUT);
    attachInterrupt(SENSOR_PIN, pulseISR, FALLING);

    periodStart_us = esp_timer_get_time();
    nextBin_us     = periodStart_us + (uint64_t)BIN_SECONDS * 1000000ULL;

    Serial.println("Setup complete (NO SLEEP, CLEAN ISR MODE)");
}

// =============================
// LOOP
// =============================
void loop() {
    uint64_t now = esp_timer_get_time();

    // ===== Bin rollover =====
    if (now >= nextBin_us) {
        Serial.printf("Bin Boundary: %u → %u\n", currentBin, currentBin + 1);

        currentBin++;
        nextBin_us += (uint64_t)BIN_SECONDS * 1000000ULL;

        // Trigger upload after final bin
        if (currentBin >= NUM_BINS) {
            Serial.println("FINAL BIN → Uploading…");

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

            // Reset bins
            memset((void*)counts, 0, sizeof(counts));
            totalPulses = 0;
            currentBin = 0;

            // Reset next period
            uint64_t now2 = esp_timer_get_time();
            nextBin_us = now2 + (uint64_t)BIN_SECONDS * 1000000ULL;

            Serial.println("UPLOAD DONE");
        }
    }

    delay(5); // light idle
}
