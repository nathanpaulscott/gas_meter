# **Gas Meter Pulse Logger & Email-Controlled Plotting System**

*A full end-to-end metering, logging, and remote-controlled plotting platform.*

---

## **Overview**

This project implements a **complete gas-meter monitoring system** using:

* **ESP32** with Hall sensor ? detects gas-meter magnetic pulses (0.01 m� per pulse)
* **Raspberry Pi** ? logs data via MQTT into SQLite
* **Python email listener** ? accepts commands via email (Yahoo IMAP)
* **Plot engine** ? generates charts (48-hour, 30-day, or arbitrary date range)
* **Email sender** ? returns plots or raw CSV automatically

The system allows you to remotely request:

* **Fresh usage plots via email**
* **Raw meter data dumps**
* **Remote reboot**
* **Stop listener**
* **Send internal logs**

Everything is controlled entirely over email, making it extremely robust even when remote SSH is unavailable.

---

## **System Architecture**

```
 ?????????????????      WiFi/MQTT       ?????????????????
 ?   ESP32 Meter  ?  ??????????????????  ? Raspberry Pi   ?
 ?  Pulse Sensor  ?                      ? MQTT Listener  ?
 ?????????????????                      ? SQLite Logging ?
                                         ?????????????????
                                                ?
                     Email Commands             ?
            ?????????????????????????????????????
            ?
??????????????????????????
? Email Listener (IMAP)  ?
? - Checks commands      ?
? - Runs plots           ?
? - Sends results via    ?
?   Yahoo SMTP           ?
??????????????????????????
          ?
          ?
   Remote Control via Email
```

---

## **ESP32 Firmware Logic (Summary)**

* GPIO interrupt catches **falling-edge** magnetic pulse.
* Debounce handling filters noise.
* Sensor wakes from deep sleep, stores pulse.
* Sensor writes JSON every **bin_s (300s)** containing:

  ```json
  { "counts": [0,1,0,2,...], "bin_s": 300 }
  ```
* JSON published to MQTT topic:

  ```
  metering/counts
  ```

---

## **Raspberry Pi: MQTT ? SQLite Logger**

SQLite schema:

```sql
CREATE TABLE log (
    timestamp REAL,
    topic TEXT,
    message TEXT
);
```

Each MQTT payload is stored with:

* UNIX epoch timestamp
* topic (`metering/counts`)
* raw JSON message

---

## **Email Listener (Command Processor)**

File: **email_listener.py**

Runs automatically via systemd service:

```
sudo systemctl enable email_listener.service
sudo systemctl start  email_listener.service
```

### **Supported Commands (send by email)**

| Command                         | Meaning            |
| ------------------------------- | ------------------ |
| `plot last48`                   | Plot last 48 hours |
| `plot last30`                   | Plot last 30 days  |
| `plot YYYY-MM-DD YYYY-MM-DD`    | Plot date range    |
| `rawdata YYYY-MM-DD YYYY-MM-DD` | Send CSV dump      |
| `sendlogs`                      | Send listener logs |
| `stop`                          | Stop the listener  |
| `reboot`                        | Reboot the Pi      |

Only emails from the **trusted sender** are accepted.

---

## **Plot Engine (gas_plot.py)**

### **Plot Types**

1. **Vertical bar-style lines (vlines)**

   * Smooth, high-density plotting
   * No lost bars due to Matplotlib compression
   * Good representation of low-frequency pilot-flame pulses

2. **Adaptive Tick Logic**

   * If range < **5 days**:

     * Major ticks = **1 hour**
     * Minor ticks = **15 minutes**
   * If range ? **5 days**:

     * Major ticks = **1 day**
     * Minor ticks = **1 hour**

3. **Axis Format**

   * X-axis moves to exactly **y = 0**
   * Tick font = **size 6**

### **Plot Example**

You receive a plot like this by email:

* X-axis = exact timestamps
* Y-axis = pulse count (0.01 m�)
* Each event = vertical blue bar (alpha 0.6)

---

## **Raw Data Export (CSV)**

Email:

```
rawdata 2025-01-01 2025-01-03
```

Listener runs exporter:

* Flattens each 300-second bin
* Computes precise midpoint `dt_meas` for each pulse bin
* Writes CSV:

Columns:

| dt_orig | dt_meas | count |
| ------- | ------- | ----- |

---

## **Cron Automation**

Daily 7:00am auto-generated plot:

```
0 7 * * * /home/pi/gas_plot.py last48 >> /home/pi/gas_plot.log 2>&1
```

---

## **Systemd Service**

`/etc/systemd/system/email_listener.service`

```
[Unit]
Description=Email Listener for Gas Commands
After=network.target

[Service]
ExecStart=/usr/bin/python3 /home/pi/email_listener.py
WorkingDirectory=/home/pi
Restart=always

[Install]
WantedBy=multi-user.target
```

Control:

```
systemctl restart email_listener
systemctl status  email_listener
journalctl -u email_listener
```

---

## **Troubleshooting**

### **Listener not responding**

* Check log:

  ```
  tail -f /home/pi/email_listener.log
  ```

### **Plots missing bars**

* Ensure ESP32 is sending JSON with correct bin_s
* Ensure SQLite is not corrupt:

  ```
  sqlite3 mqtt_log.db "SELECT COUNT(*) FROM log;"
  ```

### **Email commands not executing**

* Subject **must** be `GAS_COMMAND`
* Sender must match the **trusted sender**

### **Yahoo IMAP flakiness**

Common � listener already retries automatically.

---

## **Future Enhancements**

* Daily & hourly gas usage aggregation
* Annotated pilot-flame pattern detection
* Leak detection via abnormal baseline consumption
* Combine electricity + gas on shared timeline
* Push notifications

---

If you want this README exported as a PDF, or want architectural diagrams, SVGs, or renderings, I can generate them.
