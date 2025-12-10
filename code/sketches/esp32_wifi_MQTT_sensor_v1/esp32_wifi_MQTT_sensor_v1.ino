#include <WiFi.h>
#include <PubSubClient.h>
#include <esp_sleep.h>

#define SENSOR_PIN 34         // Hall pulse input pin (NO internal pullup! Must use external 10k pull-up)

// ====== TEST CONFIG ======
#define BIN_SECONDS     300     // seconds per bin
#define NUM_BINS        12     // number of bins per upload
#define UPLOAD_PERIOD_S 3600    // publish every Xs for testing => bin_s * n_bins 
// ===== PROD SETTINGS =====
// #define BIN_SECONDS     900     // 15 min
// #define NUM_BINS        96      // 96 bins = 24h
// #define UPLOAD_PERIOD_S 86400   // upload once per day
// =========================

const char* ssid        = "base2.4";
const char* password    = "samarajames";
const char* mqtt_server = "192.168.0.110";
const int   mqtt_port   = 1883;
const char* device_id   = "esp32_gas1";

volatile uint32_t pulseCounter = 0;
uint32_t counts[NUM_BINS] = {0};

WiFiClient espClient;
PubSubClient client(espClient);

uint32_t lastBinTime  = 0;
uint32_t startTime    = 0;
int currentBin        = 0;
int lastRSSI = -999;     // <<< NEW — stores RSSI for JSON

uint32_t sendIndex    = 0;     // <<< NEW — increments EVERY publish

// =============================
//     FIXED ISR WITH DEBOUNCE
// =============================
void IRAM_ATTR pulseISR() {
  static unsigned long last = 0;
  unsigned long now = millis();
  if (now - last > 120) {     // 120ms debounce – prevents double counts & noise
    pulseCounter++;
    last = now;
  }
}

// =============================
//       POWER SAVING SLEEP
// =============================
void enterLightSleep() {
  //sets the wakeup timer to the wifi report period
  esp_sleep_enable_timer_wakeup((uint64_t)UPLOAD_PERIOD_S * 1000000ULL);
  //enter light sleep, it will wake up on gpio interrupt from a magnetic pulse, increment counter n sleep again.  It will also wake up automatically for the wifi update 
  esp_light_sleep_start();
}

// =============================
//      BIN SAMPLING LOGIC
// =============================
void sampleBins() {
  uint32_t now = millis();
  if (lastBinTime == 0) lastBinTime = now;

  if (now - lastBinTime >= BIN_SECONDS * 1000UL) {

    counts[currentBin] += pulseCounter;
    pulseCounter = 0;

    currentBin++;

    if (currentBin >= NUM_BINS) {
      currentBin = 0;   // FULL SET → NEXT publish cycle
    }

    lastBinTime += BIN_SECONDS * 1000UL;
  }
}

// =============================
//      WIFI + MQTT
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

// =============================
//      JSON PUBLISH
// =============================
void publishBins() {

  String json = "{\"device\":\"";
  json += device_id;
  json += "\",\"idx\":";
  json += sendIndex;
  json += ",\"rssi\":";
  json += lastRSSI;            // <<< NEW FIELD
  json += ",\"bin_s\":";
  json += BIN_SECONDS;
  json += ",\"counts\":[";

  for (int i = 0; i < NUM_BINS; i++) {
    json += counts[i];
    if (i < NUM_BINS - 1) json += ",";
  }
  json += "]}";

  client.publish("metering/counts", json.c_str());
  Serial.println(json);

  sendIndex++;                     // <<< increment AFTER successful publish
}

// =============================
//         WIFI OFF
// =============================
void wifiOff() {
  client.disconnect();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  btStop();
}

// =============================
//           SETUP
// =============================
void setup() {
  Serial.begin(115200);
  delay(300);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  Serial.print("WAKE REASON: ");
  Serial.println((int)cause);

  pinMode(SENSOR_PIN, INPUT);
  attachInterrupt(SENSOR_PIN, pulseISR, FALLING);

  startTime   = millis();
  lastBinTime = startTime;

}

void loop() {

  sampleBins();
  uint32_t now = millis();

  if (now - startTime >= UPLOAD_PERIOD_S * 1000UL) {

    Serial.println("== WAKE CYCLE START =================");

    Serial.print("Connecting to WiFi ");
    Serial.println(ssid);
    unsigned long t0 = millis();
    wifiConnect();
    Serial.printf("WiFi status=%d (0=IDLE,3=OK) in %lu ms\n",
                  WiFi.status(), millis() - t0);

    if (WiFi.status() == WL_CONNECTED) {
      lastRSSI = WiFi.RSSI();      // <<< STORE RSSI
      Serial.printf("RSSI: %d dBm\n", lastRSSI);
    } else {
      Serial.println("!! WIFI FAILED — SKIPPING MQTT");
      goto SLEEP_NOW;
    }

    Serial.println("MQTT connecting...");
    if (mqttConnect()) {
      Serial.println("MQTT OK → Publishing bins...");
      publishBins();

      // Try to force MQTT/TCP buffer out (if your PubSubClient supports it)
      // If this line causes a compile error, just comment it out or delete it.
      client.flush();
      
      // ensure MQTT TX buffer actually leaves the radio
      unsigned long t0 = millis();
      while (millis() - t0 < 300) {   // 300ms is safe
        client.loop();
        delay(10);
      }
    } else {
      Serial.print("!! MQTT FAILED, rc=");
      Serial.println(client.state());
    }

    Serial.println("Disconnecting WiFi...");
    wifiOff();

SLEEP_NOW:

    Serial.println("== END CYCLE — GOING BACK TO SLEEP ==\n");

    // RESET BINS FOR NEXT PERIOD
    memset(counts, 0, sizeof(counts));
    pulseCounter = 0;
    currentBin   = 0;
    lastBinTime  = now;
    startTime    = now;
  }

  enterLightSleep();
}