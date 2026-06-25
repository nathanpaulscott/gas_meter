# Gas Meter Pulse Logger & Email-Controlled Plotting System

*A full end-to-end metering, logging, and remote-controlled plotting platform.*

---

## Overview

This project implements a complete gas-meter monitoring + remote control system using:

- **ESP32 DevKit-C** + **Hall sensor** → detects gas-meter magnetic pulses (0.01 m³ per pulse)
- **Sunflower solar-LiPo charging board** → powers ESP32 via battery or USB 5 V
- **Raspberry Pi** → logs MQTT events into SQLite
- **Mosquitto MQTT broker**
- **Python IMAP listener** → receives commands via Yahoo email
- **Python plotting engine** → returns plots or raw data via SMTP email

This lets you remotely email commands like:

```text
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

# Hardware Architecture

## 1. ESP32 → Hall Sensor → Gas Meter

### Hall Sensor Used

This build uses an **Allegro A3213-family digital Hall-effect switch** mounted under the rightmost gas-meter wheel.

Important behavior:

- The sensor output is **digital**, not analog:
  - **HIGH** = idle / no trigger
  - **LOW** = magnet detected
- The output is used with a **pull-up resistor**
- The sensor is powered from **3.3 V**
- The sensor should have a **local bypass capacitor between VCC and GND**, close to the sensor body

### Confirmed Sensor Wiring

Actual wiring used in this build:

```text
Hall sensor VCC   -> ESP32 3V3
Hall sensor GND   -> ESP32 GND
Hall sensor OUT   -> ESP32 GPIO34
10k pull-up       -> from OUT/GPIO34 node to 3V3
Bypass capacitor  -> between VCC and GND at sensor
```

### Wiring Diagram

```text
          Gas Meter Magnet
                ↓
        ┌──────────────────┐
        │  Hall Sensor     │
        │   (A3213)        │
        └──────────────────┘
              |   |   |
              |   |   └── GND --------------------> ESP32 GND
              |   └────── VCC --------------------> ESP32 3V3
              └────────── OUT --------------------> ESP32 GPIO34
                               |
                               +-- 10k resistor --> ESP32 3V3

        0.1 µF bypass capacitor:
        sensor VCC ---||--- sensor GND
```

### ESP32 Connections

```text
   ESP32 DevKit-C
 ┌──────────────────────┐
 │ 3V3  ----------------------------- Hall sensor VCC
 │ GND  ----------------------------- Hall sensor GND
 │ GPIO34 --------------------------- Hall sensor OUT------- 10k pull-up resistor to 3V3
 │ EN / RST…
 │ USB-5V → programming only
 └──────────────────────┘
```

### Important Notes

- **GPIO34 is input-only** on the ESP32 DevKit-C and should be treated as a plain input.
- Do **not** rely on an internal pull-up for GPIO34.
- The external **10k pull-up to 3.3 V** is required for this wiring arrangement.
- The bypass capacitor should be **between sensor VCC and sensor GND**, not from output to VCC.

### Signal Behavior

At the ESP32 input pin, the Hall signal should look like:

- about **3.3 V** when idle
- about **0 V** when triggered by the gas-meter magnet

The ESP32 typically detects pulses using a **falling-edge interrupt** because the signal goes from HIGH to LOW when the magnet is present.

---

## 2. ESP32 → Sunflower Solar Li-ion Charger

The Sunflower board manages:

- **Solar input (optional)**
- **Li-ion battery**
- **5V USB input**
- **5V / 3.3V regulated outputs**

### Wiring

```text
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

# Full Hardware Schematic (ASCII)

```text
                     ┌──────────────────────┐
                     │   Sunflower Board    │
                     │  (Solar/LiPo/USB)    │
                     └──────────────────────┘
                               │5V OUT
                               ▼
                    ┌───────────────────────┐
                    │        ESP32          │
                    │   DevKit-C V4         │
                    ├───────────────────────┤
                    │ VIN (5V) <────────────── Sunflower 5V
                    │ GND <──────────────────── Sunflower GND
                    │ GPIO34 <───── Hall OUT
                    │ 3V3  ───────── Hall VCC
                    │ GND  ───────── Hall GND
                    │ 3V3  ───────── 10k pull-up
                    └───────────────────────┘
                               ▲
                               │
                  10k pull-up ─┘
                               │
                         Hall signal node
                               │
                    ┌───────────────────────┐
                    │      Hall Sensor      │
                    │        A3213          │
                    ├───────────────────────┤
                    │ VCC ───────────── 3V3 │
                    │ GND ───────────── GND │
                    │ OUT ───────── GPIO34  │
                    └───────────────────────┘
                         │             │
                         └──|| 0.1 µF ─┘
                           VCC to GND

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

# MQTT (Mosquitto) Service

Install Mosquitto:

```bash
sudo apt install mosquitto mosquitto-clients
sudo systemctl enable mosquitto
sudo systemctl start mosquitto
```

ESP32 publishes JSON to:

```text
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

# SQLite Database

Location:

```text
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

```text
timestamp | metering/counts | {"bin_s":300,"counts":[0,1,0,...]}
```

Daily size stays small (<5MB/month).

---

# Email Listener (IMAP Command Handler)

Service file:

```text
/etc/systemd/system/email_listener.service
```

Runs:

```text
/home/pi/email_listener.py
```

Processes:

- `plot last48`
- `plot last30`
- `plot <date1> <date2>`
- `rawdata <date1> <date2>`
- `sendlogs`
- `stop`
- `reboot`

Only accepts commands when:

- Subject = **GAS_COMMAND**
- From trusted email address

---

# Plotting Engine

Plot style:

- Each gas bin becomes a **vertical line** (`vline`)
- No compression or bar-loss even for 30 days
- Proper timezone handling (GMT+8)

Adaptive tick spacing:

| Range | Major ticks | Minor ticks |
|---|---|---|
| < 5 days | 1 hour | 15 minutes |
| ≥ 5 days | 1 day | 1 hour |

---

# Cron Automation

```bash
0 7 * * * /home/pi/gas_plot.py last48 >> /home/pi/gas_plot.log 2>&1
```

---

# Troubleshooting

### Hall sensor not switching correctly?

Check:

- sensor VCC is actually **3.3 V**
- signal line idles near **3.3 V**
- signal line drops near **0 V** when magnet is present
- pull-up resistor is **10k from OUT to 3.3 V**
- bypass capacitor is between **VCC and GND**
- ground is common between sensor and ESP32

### Missing pulses?

Most likely causes:

- sensor placement / distance / alignment to meter magnet
- pulse filtering / debounce logic in firmware
- noisy signal wiring
- weak or broken pull-up path

### Bars disappearing?

Use vertical-line plotting. This README version fixes the Matplotlib compression issue.

### No IMAP connection?

Yahoo rate-limits; script retries automatically.

### No SMTP?

Try again; script restarts WiFi on 3rd failure.

### Log viewer

```bash
tail -f /home/pi/email_listener.log
```
