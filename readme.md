# **Gas Meter Pulse Logger & Email-Controlled Plotting System**

*A full end-to-end metering, logging, and remote-controlled plotting platform.*

---

## **Overview**

This project implements a complete gas-meter monitoring + remote control system using:

* **ESP32 DevKit-C** + **Hall sensor** → detects gas-meter magnetic pulses (0.01 m³ per pulse)
* **Sunflower solar-LiPo charging board** → powers ESP32 via battery or USB 5 V
* **Raspberry Pi** → logs MQTT events into SQLite
* **Mosquitto MQTT broker**
* **Python IMAP listener** → receives commands via Yahoo email
* **Python plotting engine** → returns plots or raw data via SMTP email

This lets you remotely email commands like:

```
plot last48
plot last30
plot 2025-01-01 2025-01-03
rawdata 2025-01-01 2025-01-03
sendlogs
reboot
stop
```

The entire system can be controlled from anywhere without SSH.

---

# **Hardware Architecture**

## **1. ESP32 → Hall Sensor → Gas Meter**

### **Hall Sensor Wiring**

Typical A3144 or equivalent digital Hall effect:

```
          Gas Meter Magnet
                ↓
        ┌──────────────────┐
        │   Hall Sensor    │
        └──────────────────┘
              |   |   |
              |   |   └── GND → ESP32 GND
              |   └────── VCC → 3.3V
              └────────── OUT → GPIO (e.g., GPIO 27)
```

### **ESP32 Connections**

```
   ESP32 DevKit-C
 ┌──────────────────────┐
 │ 3V3  ----------------------------- Hall sensor VCC
 │ GND  ----------------------------- Hall sensor GND
 │ GPIO27  -------------------------- Hall sensor OUT (digital)
 │ EN / RST…
 │ USB-5V → programming only
 └──────────────────────┘
```

The ESP32 uses **GPIO interrupt (falling edge)** to detect pulses.

---

## **2. ESP32 → Sunflower Solar Li-ion Charger**

The Sunflower board manages:

* **Solar input (optional)**
* **Li-ion battery**
* **5V USB input**
* **5V / 3.3V regulated outputs**

### Wiring

```
 Sunflower Board
 ┌──────────────────────────┐
 │ USB-5V IN  (optional)    │
 │ SOLAR IN   (optional)    │
 │ BAT+ / BAT–  → Li-ion    │
 │ 5V OUT  --------------------------→ ESP32 VIN (or 5V pin)
 │ 3.3V OUT -------------------------→ NOT USED (ESP32 uses onboard regulator)
 │ GND OUT --------------------------→ ESP32 GND
 └──────────────────────────┘
```

ESP32 should **not** be powered via the 3.3 V Sunflower output — let the ESP32 regulate from 5 V.

---

# **Full Hardware Schematic (ASCII)**

```
                     ┌──────────────────────┐
                     │   Sunflower Board    │
                     │  (Solar/LiPo/USB)    │
                     └──────────────────────┘
                               │5V OUT
                               ▼
                    ┌───────────────────────┐
                    │        ESP32          │
                    │   DevKit-C V4 (U.FL)  │
                    ├───────────────────────┤
                    │ VIN (5V) <────────────── Sunflower 5V
                    │ GND <──────────────────── Sunflower GND
                    │ GPIO27 <───── Hall OUT
                    │ 3V3  ───────── Hall VCC
                    │ GND  ───────── Hall GND
                    └───────────────────────┘
                               │
                               │ WiFi MQTT JSON
                               ▼
     ┌─────────────────────────────────────────────────┐
     │                  Raspberry Pi                   │
     │  Mosquitto MQTT Broker + SQLite Logger          │
     │  logs → mqtt_log.db                             │
     └─────────────────────────────────────────────────┘
                               │
                         Email commands
                               ▼
              ┌────────────────────────────────┐
              │ Python Email Listener (IMAP)   │
              │ - Receives commands            │
              │ - Runs plot/CSV exporters      │
              │ - Sends results over SMTP      │
              └────────────────────────────────┘
```

---

# **MQTT (Mosquitto) Service**

Install Mosquitto:

```bash
sudo apt install mosquitto mosquitto-clients
sudo systemctl enable mosquitto
sudo systemctl start mosquitto
```

ESP32 publishes JSON to:

```
topic: metering/counts
payload:
{
  "bin_s": 300,
  "counts": [0,1,0,2,...]
}
```

Test subscription on the Pi:

```bash
mosquitto_sub -t metering/# -v
```

---

# **SQLite Database**

Location:

```
/home/pi/mqtt_log.db
```

Schema:

```sql
CREATE TABLE log (
    timestamp REAL,      -- float epoch seconds (UTC)
    topic TEXT,
    message TEXT         -- JSON blob
);
```

MQTT → SQLite logger inserts rows like:

```
timestamp | metering/counts | {"bin_s":300,"counts":[0,1,0,...]}
```

Daily size stays small (<5MB/month).

---

# **Email Listener (IMAP Command Handler)**

Service file:

```
/etc/systemd/system/email_listener.service
```

Runs:

```
/home/pi/email_listener.py
```

Processes:

* plot last48
* plot last30
* plot <date1> <date2>
* rawdata <date1> <date2>
* sendlogs
* stop
* reboot

Only accepts commands when:

* Subject = **GAS_COMMAND**
* From trusted email address

---

# **Plotting Engine**

### Plot style:

* Each gas bin becomes a **vertical line** (vline)
* No compression or bar-loss even for 30 days
* Proper timezone handling (GMT+8)

### Adaptive tick spacing:

| Range    | Major ticks | Minor ticks |
| -------- | ----------- | ----------- |
| < 5 days | 1 hour      | 15 minutes  |
| ≥ 5 days | 1 day       | 1 hour      |

---

# **Cron Automation**

```
0 7 * * *  /home/pi/gas_plot.py last48 >> /home/pi/gas_plot.log 2>&1
```

---

# **Troubleshooting**

### Bars disappearing?

Use vertical-line plotting.
This README version fixes all Matplotlib compression issues.

### No IMAP connection?

Yahoo rate-limits; script retries automatically.

### No SMTP?

Try again; script restarts WiFi on 3rd failure.

### Log viewer:

```
tail -f /home/pi/email_listener.log
```

