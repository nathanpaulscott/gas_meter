#include <Arduino.h>

#define SENSOR_PIN 34   // A3213 output

int lastState = -1;

void setup()
{
    Serial.begin(115200);
    delay(500);

    pinMode(SENSOR_PIN, INPUT);

    Serial.println("A3213 Hall sensor live test");
    Serial.println("Watching for pin state changes...");
}

void loop()
{
    int s = digitalRead(SENSOR_PIN);

    if (s != lastState)
    {
        Serial.printf("PIN CHANGE -> %d   time(ms)=%lu\n", s, millis());
        lastState = s;
    }

    delay(5);
}