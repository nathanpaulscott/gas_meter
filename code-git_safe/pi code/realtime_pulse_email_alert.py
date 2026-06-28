#!/usr/bin/env python3

import paho.mqtt.client as mqtt
import smtplib
import email
import os
import json


'''
instructions:
------------------------------
you need to manually run this when you have the esp32 in realtime puslse sending mode
Then this will trigger an email for each pulse recieved, so you get more up to date info
python3 /home/pi/gasmon/realtime_pulse_email_alert.py
'''

# ============================
# CONFIG
# ============================

MQTT_TOPIC = "metering/debug/pulse"

EMAIL_TO      = "nathan.scott.rf@gmail.com"
EMAIL_ACCOUNT = "nathanpaulscott@yahoo.com"
SMTP_SERVER   = "smtp.mail.yahoo.com"
SMTP_PORT     = 465

PASSWORD_FILE = "/home/pi/gasmon/yp.sec"

with open(PASSWORD_FILE) as f:
    EMAIL_PASSWORD = f.read().strip()


# ============================
# EMAIL FUNCTION
# ============================

def send_email(text):

    msg = email.message.EmailMessage()

    msg["From"] = EMAIL_ACCOUNT
    msg["To"] = EMAIL_TO
    msg["Subject"] = "GAS PULSE DETECTED"

    msg.set_content(text)

    with smtplib.SMTP_SSL(SMTP_SERVER, SMTP_PORT) as smtp:
        smtp.login(EMAIL_ACCOUNT, EMAIL_PASSWORD)
        smtp.send_message(msg)


# ============================
# MQTT CALLBACK
# ============================

def on_message(client, userdata, msg):

    payload = msg.payload.decode(errors="ignore")

    print("PULSE:", payload)

    try:
        data = json.loads(payload)
        pulse = data.get("pulse", "?")
        bin_i = data.get("bin", "?")
    except:
        pulse = "?"
        bin_i = "?"

    body = f"Pulse detected\n\npulse={pulse}\nbin={bin_i}\n\nraw:\n{payload}"

    send_email(body)


# ============================
# MAIN
# ============================

client = mqtt.Client()

client.on_message = on_message

client.connect("localhost",1883,60)

client.subscribe(MQTT_TOPIC)

print("Listening for pulses...")

client.loop_forever()