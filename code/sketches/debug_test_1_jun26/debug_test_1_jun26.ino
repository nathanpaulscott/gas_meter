// /hall_poll_plus_interrupt_count.ino

#include <Arduino.h>

static constexpr uint8_t SENSOR_PIN = 34;
static constexpr uint32_t SAMPLE_PERIOD_US = 1000;   // 1 ms
static constexpr uint16_t SAMPLES_PER_BLOCK = 1000;  // 1 second

volatile uint32_t interruptCountThisBlock = 0;

uint8_t samples[SAMPLES_PER_BLOCK];
char sampleLine[SAMPLES_PER_BLOCK + 1];

void IRAM_ATTR onFalling() {
  interruptCountThisBlock++;
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(SENSOR_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(SENSOR_PIN), onFalling, FALLING);

  Serial.println("# hall_poll_plus_interrupt_count");
  Serial.println("# 1 = HIGH = idle");
  Serial.println("# 0 = LOW = magnet active");
  Serial.println("# INT_COUNT,<block>,<count>");
  Serial.println("# SAMPLES,<block>,<1000 bits>");
}

void loop() {
  static uint32_t blockNumber = 0;
  uint32_t nextUs = micros();

  noInterrupts();
  interruptCountThisBlock = 0;
  interrupts();

  for (uint16_t i = 0; i < SAMPLES_PER_BLOCK; i++) {
    while ((int32_t)(micros() - nextUs) < 0) {
    }

    samples[i] = (uint8_t)digitalRead(SENSOR_PIN);
    nextUs += SAMPLE_PERIOD_US;
  }

  uint32_t interruptCountSnapshot;
  noInterrupts();
  interruptCountSnapshot = interruptCountThisBlock;
  interrupts();

  for (uint16_t i = 0; i < SAMPLES_PER_BLOCK; i++) {
    sampleLine[i] = samples[i] ? '1' : '0';
  }
  sampleLine[SAMPLES_PER_BLOCK] = '\0';

  Serial.printf("INT_COUNT,%lu,%lu\n",
                (unsigned long)blockNumber,
                (unsigned long)interruptCountSnapshot);

  Serial.printf("SAMPLES,%lu,%s\n",
                (unsigned long)blockNumber,
                sampleLine);

  blockNumber++;
}