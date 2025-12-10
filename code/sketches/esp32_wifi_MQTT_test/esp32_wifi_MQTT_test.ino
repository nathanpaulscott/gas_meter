#include <WiFi.h>
#include <PubSubClient.h>
#include <esp_sleep.h>

// --- Wi-Fi credentials ---
const char* ssid = "base2.4";
const char* password = "samarajames";

// --- MQTT broker details ---
const char* mqtt_server = "192.168.0.110";  // <-- your Raspberry Pi IP
const int mqtt_port = 1883;

WiFiClient espClient;
PubSubClient client(espClient);

// --- sleep configuration ---
const uint64_t SLEEP_SECS = 15;//15 * 60;  // 15 minutes
const char* device_id = "esp32_gas1";

void setup_wifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  Serial.print("Connecting to WiFi");
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 40) { // 20 seconds max
    delay(500);
    Serial.print(".");
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    Serial.print("RSSI: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else {
    Serial.println("\nWiFi connection failed, sleeping anyway.");
  }
}

bool mqtt_connect() {
  client.setServer(mqtt_server, mqtt_port);
  if (client.connect(device_id)) {
    Serial.println("MQTT connected");
    return true;
  } else {
    Serial.print("MQTT failed, rc=");
    Serial.println(client.state());
    return false;
  }
}

void publish_data() {
  // Example JSON payload
  const char* payload =
    "{\"device\":\"esp32_gas1\",\"bin_s\":900,\"counts\":[12,15,14,16,13,26]}";

  if (client.publish("metering/counts", payload)) {
    Serial.println("Data published successfully.");
  } else {
    Serial.println("Publish failed.");
  }
}

void go_to_sleep() {
  Serial.printf("Sleeping for %llu seconds...\n", SLEEP_SECS);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  btStop();  // disable Bluetooth
  esp_sleep_enable_timer_wakeup(SLEEP_SECS * 1000000ULL);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  delay(100);

  setup_wifi();

  if (mqtt_connect()) {
    publish_data();
    // Let MQTT complete send before sleep
    client.loop();
    delay(500);  // give Wi-Fi/MQTT stack time to transmit fully
    client.disconnect();
    delay(200);  // small gap before Wi-Fi shutdown
  }

  go_to_sleep();
}

void loop() {
  // never reached — deep sleep restarts the chip
}

