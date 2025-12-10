#include <WiFi.h>
#include <PubSubClient.h>
#include <esp_sleep.h>
#include "driver/gpio.h"
#include "esp_timer.h"

#define SENSOR_PIN 34         // Hall pulse input pin (NO internal pullup! Use external 10k)

// ====== CONFIG ======
#define BIN_SECONDS      5   //300      // seconds per bin (5 min)
#define NUM_BINS         4   //12       // number of bins per upload
#define UPLOAD_PERIOD_S  20  //3600     // publish every X seconds (BIN_SECONDS * NUM_BINS)

// =====================
const char* ssid        = "base2.4";
const char* password    = "samarajames";
const char* mqtt_server = "192.168.0.110";
const int   mqtt_port   = 1883;
const char* device_id   = "esp32_gas1";

WiFiClient   espClient;
PubSubClient client(espClient);

// Shared state
volatile uint32_t counts[NUM_BINS] = {0};   // per-bin pulse counts
volatile uint32_t totalPulses      = 0;     // total pulses this period (optional/debug)

volatile uint64_t periodStart_us   = 0;     // start of current upload window (RTC µs)
volatile uint64_t lastPulseTime_us = 0;     // last pulse time (for debounce)

const uint64_t BIN_US           = (uint64_t)BIN_SECONDS     * 1000000ULL;
const uint64_t UPLOAD_PERIOD_US = (uint64_t)UPLOAD_PERIOD_S * 1000000ULL;
const uint64_t DEBOUNCE_US      = 500000ULL;   // 500ms debounce (in microseconds)

int      lastRSSI  = -999;
uint32_t sendIndex = 0;

// =============================
//   CORE PULSE COUNTING LOGIC
// =============================
static inline void IRAM_ATTR countPulseAtTime(uint64_t now_us) {
  // Read period start; only written by main with interrupts disabled => safe
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

  // RTC-based debounce (works across light sleep)
  if (now_us - lastPulseTime_us < DEBOUNCE_US) return;
  lastPulseTime_us = now_us;

  countPulseAtTime(now_us);
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
  // btStop(); // leave commented unless you really need BT off
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
//      SLEEP / WAKE HANDLING
// =============================

// Compute remaining time until end of current period and go to light sleep.
// After wake, detect if the wake was via GPIO and, if so, count the wake pulse
// if the ISR never saw it.
void prepareAndEnterSleep() {
  // Compute remaining time to end of period
  uint64_t now_us = esp_timer_get_time();
  uint64_t ps     = periodStart_us;
  uint64_t target = ps + UPLOAD_PERIOD_US;

  uint64_t sleep_us;
  if (target > now_us) {
    sleep_us = target - now_us;
  } else {
    // Already past the nominal boundary; wake almost immediately
    sleep_us = 1000ULL; // 1 ms
  }

  // Set wake sources: timer (to ensure upload) + GPIO (pulse)
  esp_sleep_enable_timer_wakeup(sleep_us);
  gpio_wakeup_enable(GPIO_NUM_34, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();

  // Enter light sleep; execution resumes after this call on wake
  esp_light_sleep_start();

  // Check what woke us
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

  if (cause == ESP_SLEEP_WAKEUP_GPIO) {
    // Treat the GPIO wake as a candidate pulse that the ISR *might* have missed.
    uint64_t wake_us = esp_timer_get_time();

    noInterrupts();
    uint64_t last = lastPulseTime_us;
    bool missed = (wake_us - last >= DEBOUNCE_US);

    if (missed) {
      // ISR never saw this edge; count exactly one pulse here.
      lastPulseTime_us = wake_us;
      countPulseAtTime(wake_us);
    }
    interrupts();

    if (missed) {
      Serial.println("Wake via GPIO: wake pulse counted in handler.");
    }
  }
}

// =============================
//            SETUP
// =============================
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(SENSOR_PIN, INPUT);
  // Attach ISR once; never detach to avoid race windows
  attachInterrupt(SENSOR_PIN, pulseISR, FALLING);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  Serial.print("WAKE REASON: ");
  Serial.println((int)cause);

  // Start of first upload period
  periodStart_us   = esp_timer_get_time();
  lastPulseTime_us = 0;

  Serial.println("Setup done, entering main loop...");
}

// =============================
//             LOOP
// =============================
void loop() {
  uint64_t now_us = esp_timer_get_time();

  // Read period start (no concurrency issue: only main writes, ISR only reads)
  uint64_t ps = periodStart_us;
  uint64_t elapsed = now_us - ps;

  if (elapsed >= UPLOAD_PERIOD_US) {
    Serial.println("\n== UPLOAD CYCLE START ==============");

    // ===== SNAPSHOT BINS & RESET UNDER LOCK =====
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

    // --- Networking and publishing ---
    Serial.print("Connecting to WiFi ");
    Serial.println(ssid);

    wifiConnect();

    if (WiFi.status() == WL_CONNECTED) {
      lastRSSI = WiFi.RSSI();
      Serial.printf("RSSI: %d dBm\n", lastRSSI);

      if (mqttConnect()) {
        Serial.println("MQTT OK → Publishing bins...");
        publishBins(binsCopy, NUM_BINS, totalCopy);

        // Give MQTT/TCP time to flush
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

    Serial.println("== END CYCLE — GOING BACK TO SLEEP ==");
  }

  // Sleep between pulses/uploads.
  // Wakes on:
  //   - gas meter pulse (GPIO34), OR
  //   - timer expiry at end of upload period
  prepareAndEnterSleep();
}
