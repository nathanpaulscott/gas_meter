// Reed Switch Oscilloscope using Serial Plotter
// Shows HIGH (1) when open, LOW (0) when magnet closes

const byte reedPin = 2;

void setup() {
  pinMode(reedPin, INPUT_PULLUP);
  Serial.begin(115200);
  Serial.println("time,reed");
}

void loop() {
  int state = digitalRead(reedPin);
  Serial.println(state);   // Print 1 (open) or 0 (closed)
  delay(2);                // Sample every 2 ms (adjust as needed)
}