#include <WiFi.h>

const char* ssid = "base2.4";
const char* password = "samarajames";

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nConnecting to WiFi...");

  WiFi.begin(ssid, password);

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 20) {
    delay(500);
    Serial.print(".");
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n❌ Failed to connect. Check SSID/password or range.");
  }
}

void loop() {
  // Keep checking connection every 5s
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Connected");
  } else {
    Serial.println("Disconnected");
  }
  delay(5000);
}
