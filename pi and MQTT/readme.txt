The pi is on wifi network ssid: base2.4 with p: samarajames

the pi is on ip 192.168.0.110 
the password to log into the pi is snooP

The pi has MQTT broker running


---------------------------
To see the MQTT status:
from a cmd on your laptop:
>wsl
>ssh pi@192.168.0.110
###put in the password: snooP

then
>sudo systemctl status mosquitto
----------------------------


to make the broker listen to any incoming connections:
-----------------------------
That tells Mosquitto:
- listen on port 1883 (the default MQTT port),
- accept connections from any network interface,
- allow anonymous (no-login) clients — fine for LAN testing.

make this file here: /etc/mosquitto/conf.d/local.conf

put these contents
---------------------
listener 1883
allow_anonymous true
---------------------

----------------------------------
ESP32 code

Filename: esp32_gas_meter.ino

Purpose:

Connects to your Wi-Fi (base2.4, pw samarajames)

Connects to local MQTT broker on your Pi (192.168.0.110, port 1883)

Publishes gas metering data in JSON format

Sleeps between transmissions to conserve power

Key JSON structure:

{
  "device": "esp32_gas1",
  "bin_s": 900,
  "counts": [12, 15, 14, 16, 13, 22]
}


**Main logic flow:**

1. Boot (or wake from deep sleep)
2. Connect to Wi-Fi
3. Connect to MQTT broker
4. Publish JSON data to topic `metering/counts`
5. Disconnect and go into deep sleep for 15 s (can be increased, e.g. 15 min or 24 h)
6. Repeat

---------------------------------------



### **Raspberry Pi**
python code is here:
/home/pi/mqtt_logger.py

to edit:
nano /home/pi/mqtt_logger.py


graph code is here
/home/pi/gas_plot.py

to edit:
nano /home/pi/gas_plot.py



DB is here:
/home/pi/mqtt_log.db

to see all msgs:
>sqlite3 /home/pi/mqtt_log.db "SELECT * FROM log;"


to clear db
sqlite3 /home/pi/mqtt_log.db "DELETE FROM log;"
sudo systemctl restart mosquitto
sudo systemctl restart mqtt_logger.service

to check num records:
>sqlite3 /home/pi/mqtt_log.db "SELECT COUNT(*) FROM log;"

to see the incoming mqtt msgs live:
mosquitto_sub -h localhost -t metering/counts -v


**Purpose:**
Acts as a data gateway and storage server.

**Configuration summary:**

* **User:** `pi`
* **Password:** `snooP`
* **Broker:** Mosquitto (running locally on port 1883)
* **Database:** SQLite3
* **Collector script

---

### **System Operation Summary**

1. ESP32 wakes ? connects ? publishes data ? sleeps.
2. Raspberry Pi (always on) runs Mosquitto + Python listener.
3. Listener parses JSON, timestamps it (UTC), and writes to SQLite.
4. Time accuracy is from Pi (stable mains-powered clock).
5. Historical bin times can be reconstructed from Pi timestamp + `bin_s` × index.

---


I have the script to pull the last 48hrs of data and send the plot to my email each morning at 10am

To run it on demand:
open a windows cmd
>wsl
now ssh into the pi
>ssh pi@192.168.0.110
###put in the password: snooP
then run the script and check your email
>./gas_plot.py



email listener
---------------------
-run code:
python3 /home/pi/email_listener.py

-edit code
nano /home/pi/email_listener.py

-copy code:
cat /home/pi/email_listener.py

-check listener service status:
systemctl status email_listener.service

-restart listener service after code changes:
sudo systemctl daemon-reload
sudo systemctl restart email_listener.service
systemctl status email_listener.service


-check the listener log:
cat /home/pi/email_listener.log

-live stream the log
tail -f /home/pi/email_listener.log

-check the listener err:
cat /home/pi/email_listener.err

-live stream the err
tail -f /home/pi/email_listener.err



usage
---------------
from nathan.scott.rf@gmail.com
send en email to nathanpaulscott@yahoo.com
subj => gas_command
body text
- plot last48 => plot last 48 hrs
- plot last30 => plot last 30 days
- plot 2025-01-17 2025-12-21 => plots the given date range (limits to 30 days from last date)
- rawdata 2025-01-17 2025-12-21 => gives the db csv dump for the given date range
- reboot => reboots the pi
- logs => email the latest logs and err to me
- stop => stops the script and service




cronjob
----------------
it runs the 48hr plot once a day at 10am
edit it here....
crontab -e



The raw data dump and email code is here
---------------------
/home/pi/rawdata_dump.py {d1} {d2}
-run with :
python3 /home/pi/rawdata_dump.py {d1} {d2}

the plot code is here:
-----------------------
/home/pi/gas_plot.py {d1} {d2}
-run with :
python3 /home/pi/gas_plot.py {d1} {d2}