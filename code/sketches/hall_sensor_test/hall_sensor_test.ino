// A3213 Hall Sensor Counter with Timestamp and 120 ms Debounce
// Prints: "Pulse Detected, t=123456 ms, cnt=4"

const byte hallPin = 2;       // A3213 output pin
const byte ledPin  = 13;      // Onboard LED
volatile unsigned long pulseCount = 0;
volatile unsigned long lastInterruptTime = 0;

void onPulse() {
  unsigned long now = millis();

  // Debounce: ignore triggers within 120 ms of the last one
  if (now - lastInterruptTime > 120) {
    pulseCount++;
    lastInterruptTime = now;

    // Blink LED briefly (non-blocking)
    digitalWrite(ledPin, HIGH);
  }
}

void setup() {
  pinMode(hallPin, INPUT_PULLUP);  // A3213 output is open-drain-like, so pull-up is required
  pinMode(ledPin, OUTPUT);

  Serial.begin(115200);
  Serial.println("A3213 Hall sensor counter with 120 ms debounce started...");

  attachInterrupt(digitalPinToInterrupt(hallPin), onPulse, FALLING);
}

void loop() {
  static unsigned long lastPrint = 0;

  // Blink off after 50 ms if lit
  if (digitalRead(ledPin) == HIGH && millis() - lastInterruptTime > 50) {
    digitalWrite(ledPin, LOW);
  }

  // Periodically print count (optional)
  if (millis() - lastPrint > 1000) {
    lastPrint = millis();
    Serial.print("Total count: ");
    Serial.println(pulseCount);
  }
}
