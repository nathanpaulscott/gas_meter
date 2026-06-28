#!/usr/bin/env python3
import sys
import sqlite3
import csv
from datetime import datetime, timezone, timedelta
import smtplib
import email
import os
import time

DB_PATH = "/home/pi/gasmon/mqtt_log.db"
TOPIC   = "metering/counts"
CSV_FILE = "/home/pi/gasmon/rawdata.csv"

EMAIL_TO      = "nathan.scott.rf@gmail.com"
EMAIL_ACCOUNT = "nathanpaulscott@yahoo.com"
SMTP_SERVER   = "smtp.mail.yahoo.com"
SMTP_PORT     = 465
PASSWORD_FILE = "/home/pi/gasmon/yp.sec"

with open(PASSWORD_FILE, "r") as f:
    EMAIL_PASSWORD = f.read().strip()

# ------------------------------------------------------
# Parse command args
# ------------------------------------------------------
if len(sys.argv) != 3:
    print("Usage: rawdata_dump.py <YYYY-MM-DD> <YYYY-MM-DD>")
    exit(1)

try:
    start_dt = datetime.strptime(sys.argv[1], "%Y-%m-%d").replace(tzinfo=timezone.utc)
    end_dt   = datetime.strptime(sys.argv[2], "%Y-%m-%d").replace(tzinfo=timezone.utc)
except:
    print("Invalid date format. Use YYYY-MM-DD")
    exit(1)

if end_dt < start_dt:
    print("End date must be >= start date")
    exit(1)

start_epoch = start_dt.timestamp()
end_epoch   = end_dt.timestamp()

# ------------------------------------------------------
# Fetch DB rows
# ------------------------------------------------------
conn = sqlite3.connect(DB_PATH)
cur = conn.cursor()

cur.execute("""
    SELECT timestamp, topic, message
    FROM log
    WHERE timestamp >= ?
      AND timestamp <= ?
    ORDER BY timestamp ASC
""", (start_epoch, end_epoch))

rows = cur.fetchall()
conn.close()

if not rows:
    print("NO DATA FOUND IN RANGE")
    exit(1)

# ------------------------------------------------------
# Write CSV
# ------------------------------------------------------
with open(CSV_FILE, "w", newline="") as f:
    writer = csv.writer(f)
    writer.writerow(["timestamp_epoch", "timestamp_local", "topic", "message"])

    for ts, topic, msg in rows:
        dt = datetime.fromtimestamp(ts, timezone(timedelta(hours=8)))
        writer.writerow([ts, dt.isoformat(), topic, msg])

print("CSV SAVED:", CSV_FILE)

# ------------------------------------------------------
# Send CSV via email
# ------------------------------------------------------
def send_email():
    msg = email.message.EmailMessage()
    msg["From"] = EMAIL_ACCOUNT
    msg["To"]   = EMAIL_TO
    msg["Subject"] = f"Raw Data CSV {sys.argv[1]} to {sys.argv[2]}"
    msg.set_content(f"Attached raw CSV data for {sys.argv[1]} → {sys.argv[2]}")

    with open(CSV_FILE, "rb") as f:
        msg.add_attachment(f.read(),
            maintype="text",
            subtype="csv",
            filename=os.path.basename(CSV_FILE)
        )

    for attempt in range(5):
        try:
            with smtplib.SMTP_SSL(SMTP_SERVER, SMTP_PORT) as smtp:
                smtp.login(EMAIL_ACCOUNT, EMAIL_PASSWORD)
                smtp.send_message(msg)
            print("EMAIL SENT OK")
            return
        except Exception as e:
            print("EMAIL ERROR:", e)
            time.sleep(5)

    print("FAILED TO SEND EMAIL")

send_email()