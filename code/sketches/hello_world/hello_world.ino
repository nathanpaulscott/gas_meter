// ESP32 "Hello World" Blink Test

const byte ledPin = 2;  // Onboard LED on most ESP32 DevKit boards (GPIO2)

void setup() {
  pinMode(ledPin, OUTPUT);
  Serial.begin(115200);
  Serial.println("ESP32 Hello World — LED blink test started!");
}

void loop() {
  digitalWrite(ledPin, HIGH);
  delay(500);
  digitalWrite(ledPin, LOW);
  delay(500);
 a Serial.println("Blink");
}