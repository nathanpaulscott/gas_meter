# Gas Meter Pulse Logger & Flow-Rate Plotting System

*A full end-to-end gas-meter pulse logger with SQLite storage, email-triggered commands, and daily gas flow-rate plots.*

---

## Overview

This project monitors a residential gas meter using a Hall-effect sensor and an ESP32. The gas meter produces one detectable magnetic pulse per **0.01 m³** of gas usage.

System components:

- **ESP32 DevKit-C** + **Allegro A3213 Hall-effect switch** → detects gas-meter magnetic pulses
- **Sunflower solar/LiPo charging board** → powers the ESP32 from battery, USB 5 V, and optionally solar
- **Raspberry Pi** → receives MQTT messages and logs them into SQLite
- **Mosquitto MQTT broker** → receives ESP32 pulse-count payloads
- **Python IMAP listener** → receives remote commands by email
- **Python plotting scripts** → return charts or raw data by SMTP email

Remote email commands include:

```text
plot last48
plot last30
plot 2025-01-01 2025-01-03
rawdata 2025-01-01 2025-01-03
sendlogs
reboot
stop
```

The system can be managed remotely without SSH access.

---

# Hardware Architecture

## 1. ESP32 → A3213 Hall Sensor → Gas Meter

### Hall Sensor Used

This build uses an **Allegro A3213-family digital Hall-effect switch** mounted near the rightmost gas-meter wheel.

Important behaviour:

- The sensor output is **digital**, not analog.
- The output is normally **HIGH** through the pull-up resistor.
- The output switches **LOW** when the gas-meter magnet is detected.
- The ESP32 detects pulses using a **falling-edge interrupt**.
- Each gas-meter pulse represents **0.01 m³** of gas.
- The A3213 is pole-independent: either north or south magnetic pole can operate it, provided the field is strong enough.

### A3213 SIP Package Orientation / Pinout

This README assumes the **3-pin SIP / through-hole A3213 package** used in this build.

For this package, looking at the **front / branded / bevelled face** of the sensor with the pins pointing downward:

```text
        FRONT / BRANDED / BEVELLED FACE

             ┌───────────────┐
             │     A3213     │
             │   Hall side   │
             └───────────────┘
                │    │    │
                │    │    └── Pin 3: VOUT
                │    └─────── Pin 2: GND
                └──────────── Pin 1: VDD

             left → right = VDD, GND, VOUT
```

Do **not** blindly copy this pin order for a different package. The SOT-23/LH package has a different physical layout.

### Confirmed Sensor Wiring

Actual wiring used in this build:

```text
A3213 VDD   -> ESP32 3V3
A3213 GND   -> ESP32 GND
A3213 VOUT  -> ESP32 GPIO34
10k pull-up -> from VOUT/GPIO34 node to ESP32 3V3
0.1 µF cap  -> between A3213 VDD and A3213 GND, close to the sensor body
```

### Wiring Diagram

```text
          Gas Meter Magnet
                ↓
        ┌──────────────────┐
        │  A3213 Hall      │
        │  front/bevelled  │
        │  face to magnet  │
        └──────────────────┘
              |   |   |
              |   |   └── VOUT -------------------> ESP32 GPIO34
              |   |                                     |
              |   |                                     +-- 10k pull-up --> ESP32 3V3
              |   └────── GND --------------------> ESP32 GND
              └────────── VDD --------------------> ESP32 3V3

        0.1 µF bypass capacitor:
        A3213 VDD ---||--- A3213 GND
```

### ESP32 Connections

```text
   ESP32 DevKit-C
 ┌──────────────────────┐
 │ 3V3  ----------------------------- A3213 VDD
 │ GND  ----------------------------- A3213 GND
 │ GPIO34 --------------------------- A3213 VOUT
 │                                      |
 │ 3V3  -------- 10k pull-up -----------+
 │ VIN / 5V <------------------------- Sunflower 5V OUT
 │ GND     <-------------------------- Sunflower GND
 └──────────────────────┘
```

### Important Hardware Notes

- **GPIO34 is input-only** on the ESP32 DevKit-C.
- **GPIO34 has no usable internal pull-up**, so the external **10k pull-up to 3.3 V is required**.
- Keep the A3213 powered from **3.3 V**, not 5 V, because its output is connected directly to the ESP32 input.
- The A3213 supply range allows 3.3 V operation.
- The bypass capacitor should be **between VDD and GND**, close to the sensor.
- Do **not** put the bypass capacitor from output to VDD.
- Sensor ground and ESP32 ground must be common.
- The magnetic sensing face is the front/branded/bevelled face; place this side toward the gas-meter magnet.

### Signal Behaviour

At the ESP32 input pin:

```text
Idle / no magnet:      about 3.3 V
Magnet detected:       about 0 V
Interrupt edge used:   FALLING
```

---

## 2. ESP32 → Sunflower Solar Li-ion Charger

The Sunflower board manages:

- **USB 5 V input**
- **Solar input** if fitted
- **Li-ion battery charging**
- **5 V / 3.3 V regulated outputs**

### Wiring

```text
 Sunflower Board
 ┌──────────────────────────┐
 │ USB-5V IN  (optional)    │
 │ SOLAR IN   (optional)    │
 │ BAT+ / BAT–  → Li-ion    │
 │ 5V OUT  --------------------------→ ESP32 VIN / 5V pin
 │ 3.3V OUT -------------------------→ not used for ESP32 power
 │ GND OUT --------------------------→ ESP32 GND
 └──────────────────────────┘
```

Use the Sunflower **5 V output** into the ESP32 VIN/5V input and let the ESP32 board regulate its own 3.3 V rail.

---

# Full Hardware Schematic (ASCII)

```text
                     ┌──────────────────────┐
                     │   Sunflower Board    │
                     │  Solar / LiPo / USB  │
                     └──────────────────────┘
                               │ 5V OUT
                               ▼
                    ┌───────────────────────┐
                    │        ESP32          │
                    │      DevKit-C V4      │
                    ├───────────────────────┤
                    │ VIN / 5V <──────────── Sunflower 5V
                    │ GND      <──────────── Sunflower GND
                    │ GPIO34   <──────────── A3213 VOUT
                    │ 3V3      ────────────> A3213 VDD
                    │ GND      ────────────> A3213 GND
                    │ 3V3      ── 10k ─────> VOUT/GPIO34 node
                    └───────────────────────┘
                                      ▲
                                      │
                            Hall signal node
                                      │
                    ┌───────────────────────┐
                    │      A3213 SIP        │
                    │ front/bevelled face   │
                    │ pins down, L→R:       │
                    │ VDD, GND, VOUT        │
                    └───────────────────────┘
                         │             │
                         └──|| 0.1 µF ─┘
                           VDD to GND

                               │
                               │ WiFi MQTT JSON
                               ▼
     ┌─────────────────────────────────────────────────┐
     │                  Raspberry Pi                   │
     │  Mosquitto MQTT Broker + SQLite Logger          │
     │  logs → /home/pi/gasmon/mqtt_log.db                    │
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

# MQTT Payload

The ESP32 publishes binned pulse counts to Mosquitto.

Topic:

```text
metering/counts
```

Example payload:

```json
{
  "bin_s": 300,
  "counts": [0, 1, 0, 2, 0, 0]
}
```

Meaning:

```text
bin_s = bin duration in seconds
counts[i] = number of gas-meter pulses detected in that bin
1 pulse = 0.01 m³
```

Test subscription on the Pi:

```bash
mosquitto_sub -t metering/# -v
```

---

# SQLite Database

Database location:

```text
/home/pi/gasmon/mqtt_log.db
```

Schema:

```sql
CREATE TABLE log (
    timestamp REAL,      -- float epoch seconds, UTC
    topic TEXT,
    message TEXT         -- JSON payload
);
```

Example stored row:

```text
timestamp | metering/counts | {"bin_s":300,"counts":[0,1,0,2,0]}
```

Total gas usage is calculated from raw pulse count:

```text
total_gas_m3 = total_pulses × 0.01
```

The smoothed plot is visual only. Do **not** use the smoothed line as the billing/source-of-truth total.

---

# Plotting Engine

There are two useful plot styles.

## 1. Raw bin-count plot

Older plot style:

- Each bin is shown as a vertical line/bar.
- Y value is pulse count in that bin.
- Useful for checking raw pulse/bucket behaviour.

## 2. Gas flow-rate plot — current preferred plot

Current script:

```text
/home/pi/gasmon/gas_plot_rate.py
```

Output file:

```text
/home/pi/gasmon/gas_plot_rate.png
```

The flow-rate plot converts binned pulse counts into an inferred gas flow-rate time series.

Processing strategy:

```text
raw DB bin counts
→ inferred individual pulse timestamps
→ interval-based flow-rate points
→ regular-grid resampling
→ centered exponential moving average
→ flow-rate line plot
```

Detailed logic:

1. Read each MQTT row from SQLite.
2. For each bin:
   - count = 0 → no pulse timestamp
   - count = 1 → one pulse timestamp at the bin centre
   - count = N → N pulse timestamps evenly spaced inside the bin
3. Sort all inferred pulse timestamps.
4. For each consecutive pulse pair:

```text
t_prev = previous pulse time
t_now  = current pulse time
dt_s   = t_now - t_prev
rate_m3_per_hour = 0.01 × 3600 / dt_s
rate_timestamp = midpoint(t_prev, t_now)
```

5. Resample the interval-rate series onto a regular time grid.
6. Smooth the regular-grid rate series using a centred exponential moving average.
7. Plot:
   - optional raw DB pulse-count bins on a secondary y-axis, or
   - optional unsmoothed resampled rate grid on the main y-axis, and
   - the centred EMA flow-rate line as the main red line

This avoids the older fixed N-point moving-average problem, where the same number of points could represent minutes during active gas use but many hours during quiet pilot-light use. The centred smoother uses both past and future samples, so it is for historical plotting only, not live prediction.

## Centered EMA smoothing settings

Current smoothing parameters in `gas_plot_rate.py`:

```python
RESAMPLE_GRID_S = 100.0       # regular time grid in seconds
CENTERED_EMA_RADIUS = 8       # samples before/after the current point
CENTERED_EMA_DECAY = 2.0      # lower = more local; higher = smoother/flatter
UNDERLAY_MODE = "pulses"      # "pulses" or "raw_rate"
```

Tuning guide:

```text
RESAMPLE_GRID_S lower       → finer grid, more detail
RESAMPLE_GRID_S higher      → coarser grid, calmer plot
CENTERED_EMA_RADIUS lower   → less smoothing
CENTERED_EMA_RADIUS higher  → more smoothing
CENTERED_EMA_DECAY lower    → more local weighting
CENTERED_EMA_DECAY higher   → flatter/smoother weighting
```

The current preferred setting is:

```python
RESAMPLE_GRID_S = 100.0
CENTERED_EMA_RADIUS = 8
CENTERED_EMA_DECAY = 2.0
UNDERLAY_MODE = "pulses"
```

## Python dependencies

The plotter requires NumPy and Matplotlib:

```bash
sudo apt install python3-numpy python3-matplotlib
```

## Flow-rate interpretation

Examples:

```text
1 pulse per hour:
0.01 m³ / 1 hour = 0.01 m³/h

5 pulses in 5 minutes:
0.05 m³ / (5/60 hour) = 0.6 m³/h
```

## Plot limitations

The database stores **counts per bin**, not exact pulse timestamps. When a bin has multiple pulses, the exact timing inside that bin is lost. The plot therefore uses evenly spaced inferred timestamps inside each bin.

The smoothed line is for visual interpretation only. It should not be used as the source of truth for total gas usage. Total gas usage should always come from raw pulse count:

```text
total_gas_m3 = total_pulses × 0.01
```

## Plot axis behaviour

The rate plot uses adaptive time ticks:

| Range | Major ticks | Minor ticks |
|---|---|---|
| Up to 2 days | 1 hour | 15 minutes |
| 2 to 5 days | 2 hours | 30 minutes |
| More than 5 days | 6 hours | 1 hour |

---

# Email Listener (IMAP Command Handler)

Service file:

```text
/etc/systemd/system/email_listener.service
```

Script:

```text
/home/pi/gasmon/email_listener.py
```

MQTT logger service:

```text
/etc/systemd/system/mqtt_logger.service
```

MQTT logger script:

```text
/home/pi/gasmon/mqtt_logger.py
```

Processes commands such as:

- `plot last48`
- `plot last30`
- `plot <date1> <date2>`
- `rawdata <date1> <date2>`
- `sendlogs`
- `stop`
- `reboot`

Only accepts commands when:

- Subject is **GAS_COMMAND**
- Sender is the trusted email address configured in the script

---

# Cron Automation

Daily gas flow-rate chart at 7:00am:

```cron
0 7 * * * /usr/bin/python3 /home/pi/gasmon/gas_plot_rate.py last48 >> /home/pi/gasmon/gas_plot.log 2>&1
```

Check installed crontab:

```bash
crontab -l
```

Manual test of the exact cron command:

```bash
/usr/bin/python3 /home/pi/gasmon/gas_plot_rate.py last48 >> /home/pi/gasmon/gas_plot.log 2>&1
```

Check log:

```bash
tail -50 /home/pi/gasmon/gas_plot.log
```

---

# Troubleshooting

## Hall sensor not switching correctly

Check:

- A3213 VDD is actually **3.3 V**.
- A3213 GND is common with ESP32 GND.
- Looking at the **front/branded/bevelled face**, pins down, the SIP pins are **VDD, GND, VOUT** from left to right.
- VOUT/GPIO34 idles near **3.3 V**.
- VOUT/GPIO34 drops near **0 V** when the magnet is present.
- The **10k pull-up** is from VOUT/GPIO34 to **3.3 V**.
- The **0.1 µF bypass capacitor** is between VDD and GND close to the sensor.
- The magnet is close enough to the front/branded/bevelled sensing face.

## Missing pulses

Most likely causes:

- Sensor too far from the gas-meter magnet.
- Sensor face not aligned with the magnet path.
- Wrong A3213 pin orientation.
- No pull-up, weak pull-up, or pull-up connected to the wrong rail.
- Noisy wiring.
- Debounce/filtering too aggressive in ESP32 firmware.
- GPIO34 not actually connected to VOUT.

## Flow-rate plot looks wrong

Check:

- The script is using `/home/pi/gasmon/gas_plot_rate.py`, not an old copy in `/home/pi`.
- NumPy and Matplotlib are installed and importable.
- The plot is using regular-grid resampling plus centred EMA smoothing, not the older Gaussian smoother, smoothing spline, or fixed N-point moving average.
- `CENTERED_EMA_RADIUS` is not too high. Higher values smooth more aggressively.
- `CENTERED_EMA_DECAY` is not too high. Higher values make the curve flatter.
- `RESAMPLE_GRID_S` is set to `100.0` for the current grid, or higher if a coarser plot is wanted.
- Raw total gas usage still matches `pulse_count × 0.01`.

## No IMAP connection

Yahoo may rate-limit or temporarily refuse login. The script retries automatically.

## No SMTP email

Try again; the script retries and restarts WiFi on repeated SMTP failure.

## Useful logs

Email listener log:

```bash
tail -f /home/pi/gasmon/email_listener.log
```

Gas plot cron log:

```bash
tail -f /home/pi/gasmon/gas_plot.log
```
