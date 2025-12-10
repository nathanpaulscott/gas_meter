#include <WiFi.h>
#include <PubSubClient.h>
#include "esp_timer.h"

#define SENSOR_PIN 34         // Hall pulse input pin (NO internal pullup! Use external 10k)

// ====== CONFIG (TEST/PROD) ======
// Adjust these for final deployment (e.g. 300 / 12 / 3600)
#define BIN_SECONDS      20 //20      // seconds per bin
#define NUM_BINS         5   //5       // number of bins per upload
#define UPLOAD_PERIOD_S  100  //100     // publish every X seconds

// =====================
const char* ssid        = "base2.4";
const char* password    = "samarajames";
const char* mqtt_server = "192.168.0.110";
const int   mqtt_port   = 1883;
const char* device_id   = "esp32_gas1";

WiFiClient   espClient;
PubSubClient client(espClient);

// Shared state (ISR <-> main)
volatile uint32_t counts[NUM_BINS] = {0};   // per-bin pulse counts
volatile uint32_t totalPulses      = 0;     // total pulses this period

volatile uint64_t periodStart_us   = 0;     // start of current upload window (RTC µs)
volatile uint64_t lastPulseTime_us = 0;     // last pulse time (for debounce)

const uint64_t BIN_US           = (uint64_t)BIN_SECONDS     * 1000000ULL;
const uint64_t UPLOAD_PERIOD_US = (uint64_t)UPLOAD_PERIOD_S * 1000000ULL;
const uint64_t DEBOUNCE_US      = 500000ULL;   // 0.5 s debounce (tune as needed)

int      lastRSSI  = -999;
uint32_t sendIndex = 0;

// Debug: show when a pulse is actually counted
volatile bool     debugPulseFlag    = false;
volatile uint64_t debugPulseTime_us = 0;

// =============================
//   CORE PULSE COUNTING LOGIC
// =============================
static inline void IRAM_ATTR countPulseAtTime(uint64_t now_us) {
  // periodStart_us is only written in main with interrupts disabled
  uint64_t ps = periodStart_us;
  uint64_t elapsed = now_us - ps;

  uint32_t bin = elapsed / BIN_US;
  if (bin >= NUM_BINS) {
    bin = NUM_BINS - 1;   // clamp late pulses into last bin
  }

  counts[bin]++;
  totalPulses++;
}

// =============================
//     ISR — COUNT & BIN PULSE
// =============================
void IRAM_ATTR pulseISR() {
  uint64_t now_us = esp_timer_get_time();

  // RTC-based debounce
  if (now_us - lastPulseTime_us < DEBOUNCE_US) return;
  lastPulseTime_us = now_us;

  countPulseAtTime(now_us);

  // Mark for debug print in loop()
  debugPulseTime_us = now_us;
  debugPulseFlag    = true;
}

// =============================
//          WIFI / MQTT
// =============================
void wifiConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  // Wait up to ~8s
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

// =============================
//          JSON PUBLISH
// =============================
void publishBins(const uint32_t* bins, size_t numBins, uint32_t totalInPeriod) {
  String json = "{\"device\":\"";
  json += device_id;
  json += "\",\"idx\":";
  json += sendIndex;
  json += ",\"rssi\":";
  json += lastRSSI;
  json += ",\"bin_s\":";
  json += BIN_SECONDS;
  json += ",\"total\":";
  json += totalInPeriod;
  json += ",\"counts\":[";

  for (size_t i = 0; i < numBins; i++) {
    json += bins[i];
    if (i < numBins - 1) json += ",";
  }
  json += "]}";

  client.publish("metering/counts", json.c_str());
  Serial.println(json);

  sendIndex++;
}

// =============================
//            SETUP
// =============================
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(SENSOR_PIN, INPUT);

  // Initialize timing BEFORE enabling ISR
  periodStart_us   = esp_timer_get_time();
  lastPulseTime_us = 0;

  attachInterrupt(SENSOR_PIN, pulseISR, FALLING);

  Serial.println("Setup done, always-on mode.");
}

// =============================
//             LOOP
// =============================
void loop() {
  // Debug: print when a pulse was actually counted
  if (debugPulseFlag) {
    noInterrupts();
    debugPulseFlag = false;
    uint64_t t_us  = debugPulseTime_us;
    uint32_t tp    = totalPulses;
    interrupts();

    Serial.printf("PULSE counted at %.3f s, totalPulses=%u\n",
                  t_us / 1e6, tp);
  }

  uint64_t now_us = esp_timer_get_time();
  uint64_t ps     = periodStart_us;
  uint64_t elapsed = now_us - ps;

  if (elapsed >= UPLOAD_PERIOD_US) {
    Serial.println("\n== UPLOAD CYCLE START ==============");

    // Snapshot bins & reset under lock
    uint32_t binsCopy[NUM_BINS];
    uint32_t totalCopy;

    noInterrupts();
    for (int i = 0; i < NUM_BINS; i++) {
      binsCopy[i] = counts[i];
      counts[i]   = 0;
    }
    totalCopy      = totalPulses;
    totalPulses    = 0;
    periodStart_us = esp_timer_get_time();   // next period starts "now"
    interrupts();

    // Networking + publish
    Serial.print("Connecting to WiFi ");
    Serial.println(ssid);

    wifiConnect();

    if (WiFi.status() == WL_CONNECTED) {
      lastRSSI = WiFi.RSSI();
      Serial.printf("RSSI: %d dBm\n", lastRSSI);

      if (mqttConnect()) {
        Serial.println("MQTT OK → Publishing bins...");
        publishBins(binsCopy, NUM_BINS, totalCopy);

        // Give MQTT/TCP a moment to flush
        unsigned long t1 = millis();
        while (millis() - t1 < 300) {
          client.loop();
          delay(10);
        }
      } else {
        Serial.print("!! MQTT FAILED, rc=");
        Serial.println(client.state());
      }
    } else {
      Serial.println("!! WIFI FAILED — SKIPPING MQTT");
    }

    Serial.println("Disconnecting WiFi...");
    wifiOff();

    Serial.println("== END CYCLE =======================");
  }

  // Always-on: just idle here
  delay(10);   // small yield so we’re not spin-burning
}
