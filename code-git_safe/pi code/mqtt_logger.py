#!/usr/bin/env python3
import paho.mqtt.client as mqtt
import sqlite3, time, os

DB_PATH = "/home/pi/gasmon/mqtt_log.db"

# ensure DB exists
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