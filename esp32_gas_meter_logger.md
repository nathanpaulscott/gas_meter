# ESP32 Gas Meter Data Logger Project

## Overview
A low-power ESP32 node counts events (e.g., gas pulses) and periodically transmits them via MQTT to a Raspberry Pi.  
The Pi runs a Mosquitto broker and a Python subscriber that logs incoming data into an SQLite database.  
Everything auto-starts on reboot and runs unattended.

---

## 1️⃣  Raspberry Pi Setup

### User and Environment
- **User:** `pi`  
- **Password:** `snooP`
- **Python version:** 3.x  
- **Database:** `/home/pi/mqtt_log.db`
- **MQTT Broker:** Mosquitto  
  ```bash
  sudo apt install -y mosquitto mosquitto-clients python3-paho-mqtt sqlite3
  sudo systemctl enable mosquitto
  sudo systemctl start mosquitto
  ```

---

## 2️⃣  Python Subscriber

**File:** `/home/pi/mqtt_logger.py`

```python
#!/usr/bin/env python3
import paho.mqtt.client as mqtt
import sqlite3, time

DB_PATH = "/home/pi/mqtt_log.db"

# Ensure DB exists
conn = sqlite3.connect(DB_PATH)
c = conn.cursor()
c.execute("""CREATE TABLE IF NOT EXISTS log (
    timestamp REAL,
    topic TEXT,
    message TEXT
)""")
conn.commit()
conn.close()

def on_message(client, userdata, msg):
    data = msg.payload.decode(errors='ignore')
    ts = time.time()
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()
    c.execute("INSERT INTO log VALUES (?, ?, ?)", (ts, msg.topic, data))
    conn.commit()
    conn.close()
    print(f"{time.strftime('%H:%M:%S')}: {msg.topic} → {data}")

client = mqtt.Client()
client.on_message = on_message
client.connect("localhost", 1883, 60)
client.subscribe("#")
client.loop_forever()
```

**Make executable**
```bash
chmod +x /home/pi/mqtt_logger.py
```

---

## 3️⃣  Auto-Start with systemd

**Service file:** `/etc/systemd/system/mqtt_logger.service`

```ini
[Unit]
Description=MQTT Data Logger
After=network-online.target mosquitto.service
Wants=network-online.target

[Service]
User=pi
ExecStart=/usr/bin/python3 /home/pi/mqtt_logger.py
Restart=always
RestartSec=10
WorkingDirectory=/home/pi
StandardOutput=append:/home/pi/mqtt_messages.log
StandardError=append:/home/pi/mqtt_errors.log

[Install]
WantedBy=multi-user.target
```

**Enable and start**
```bash
sudo systemctl daemon-reload
sudo systemctl enable mqtt_logger.service
sudo systemctl start mqtt_logger.service
```

**Check status**
```bash
sudo systemctl status mqtt_logger.service
```

**View logs**
```bash
tail -f /home/pi/mqtt_messages.log
tail -f /home/pi/mqtt_errors.log
```

---

## 4️⃣  SQLite Database

**Schema**
```sql
CREATE TABLE log (
    timestamp REAL,
    topic TEXT,
    message TEXT
);
```

**Example query**
```bash
sqlite3 /home/pi/mqtt_log.db "SELECT * FROM log ORDER BY timestamp DESC LIMIT 10;"
```

---

## 5️⃣  ESP32 Firmware

**Arduino sketch:**

```cpp
#include <WiFi.h>
#include <PubSubClient.h>
#include <esp_sleep.h>

// --- Wi-Fi credentials ---
const char* ssid = "base2.4";
const char* password = "samarajames";

// --- MQTT broker details ---
const char* mqtt_server = "192.168.0.110";  // Raspberry Pi IP
const int mqtt_port = 1883;

WiFiClient espClient;
PubSubClient client(espClient);

// --- sleep configuration ---
const uint64_t SLEEP_SECS = 15; // 15 seconds for testing
const char* device_id = "esp32_gas1";

void setup_wifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  Serial.print("Connecting to WiFi");
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 40) {
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
  const char* payload =
    "{\"device\":\"esp32_gas1\",\"bin_s\":900,\"counts\":[12,15,14,16,13,22]}";

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
  btStop();
  esp_sleep_enable_timer_wakeup(SLEEP_SECS * 1000000ULL);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  delay(100);
  setup_wifi();

  if (mqtt_connect()) {
    publish_data();
    client.disconnect();
  }

  go_to_sleep();
}

void loop() {
  // never reached — deep sleep restarts the chip
}
```

### Behavior
- Boots from deep sleep  
- Connects to Wi-Fi  
- Connects to MQTT broker  
- Publishes JSON payload on topic `metering/counts`  
- Sleeps for 15 s (adjust `SLEEP_SECS` to e.g. 900 for 15 min)  
- Runs indefinitely with minimal power draw

---

## 6️⃣  Example Payload
```json
{
  "device": "esp32_gas1",
  "bin_s": 900,
  "counts": [12, 15, 14, 16, 13]
}
```
- `device`: unique ID of the ESP node  
- `bin_s`: bin duration in seconds (e.g., 900 = 15 min)  
- `counts`: array of 15 min pulse counts  
- Pi timestamp gives end time for the series  

---

## 7️⃣  Typical Serial Output (ESP32)
```
Connecting to WiFi....
WiFi connected!
IP address: 192.168.0.121
RSSI: -72 dBm
MQTT connected
Data published successfully.
Sleeping for 15 seconds...
[  3069][W][STA.cpp:137] _onStaArduinoEvent(): Reason: 8 - ASSOC_LEAVE
```

---

## 8️⃣  Notes
- **Power source:** USB-C power bank (5 V) works well.  
- **Deep sleep:** saves power between transmissions.  
- **Database timestamps:** originate from Pi for consistent UTC time base.  
- **Scalable:** multiple ESP nodes can publish under different topics/devices.

---

## 9️⃣  Future Enhancements
- Add sensor readouts instead of static test counts.  
- Implement Wi-Fi off except during transmission.  
- Add watchdog timer or retained MQTT last-will message.

---

**Project complete — fully autonomous data logging pipeline.**
