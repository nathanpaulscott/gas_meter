// Reed Switch Counter with Timestamp and 120 ms Debounce
// Prints: "Pulse Detected, t=123456 ms, cnt=4"

const byte reedPin = 2;      // Reed switch input pin
const byte ledPin  = 13;     // Onboard LED
volatile unsigned long pulseCount = 0;
volatile unsigned long lastInterruptTime = 0;

void onPulse() {
  unsigned long now = millis();

  // Debounce: ignore triggers within 120 ms of the last one
  if (now - lastInterruptTime > 120) {
    pulseCount++;
    lastInterruptTime = now;

    // Blink LED briefly
    digitalWrite(ledPin, HIGH);
    delay(50);
    digitalWrite(ledPin, LOW);

    // Print timestamp and count
    Serial.print("Pulse Detected, t=");
    Serial.print(now);
    Serial.print(" ms, cnt=");
    Serial.println(pulseCount);
  }
}

void setup() {
  pinMode(reedPin, INPUT_PULLUP);
  pinMode(ledPin, OUTPUT);
  attachInterrupt(digitalPinToInterrupt(reedPin), onPulse, FALLING);

  Serial.begin(115200);
  Serial.println("Reed switch counter with 120 ms debounce started...");
}

void loop() {
  // Nothing to do — handled in interrupt
}

