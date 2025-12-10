#!/usr/bin/env python3
import sys
import sqlite3
import json
import os
import time
from datetime import datetime, timezone, timedelta
import matplotlib.dates as mdates

# ------------------ HEADLESS PLOTTING ------------------
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

# ------------------ EMAIL ------------------
import smtplib
import email

# ======================================================
# CONFIGURATION
# ======================================================

DB_PATH   = "/home/pi/mqtt_log.db"
TOPIC     = "metering/counts"
PNG_FILE  = "/home/pi/gas_plot_range.png"

EMAIL_TO      = "nathan.scott.rf@gmail.com"
EMAIL_ACCOUNT = "nathanpaulscott@yahoo.com"
SMTP_SERVER   = "smtp.mail.yahoo.com"
SMTP_PORT     = 465

PASSWORD_FILE = "/home/pi/surf_data/yp.sec"

with open(PASSWORD_FILE, "r") as f:
    EMAIL_PASSWORD = f.read().strip()

LOCAL_TZ   = timezone(timedelta(hours=8))   # Perth GMT+8
MAX_RANGE_SEC = 31 * 86400


# ======================================================
# ARGUMENT PARSING
# ======================================================

args = sys.argv[1:]

if len(args) == 1 and args[0].lower() == "last48":
    end_epoch = time.time()
    start_epoch = end_epoch - 48 * 3600
    start_dt = datetime.fromtimestamp(start_epoch, LOCAL_TZ)
    end_dt   = datetime.fromtimestamp(end_epoch, LOCAL_TZ)

elif len(args) == 1 and args[0].lower() == "last30":
    end_epoch = time.time()
    start_epoch = end_epoch - 30 * 86400
    start_dt = datetime.fromtimestamp(start_epoch, LOCAL_TZ)
    end_dt   = datetime.fromtimestamp(end_epoch, LOCAL_TZ)

elif len(args) == 2:
    try:
        start_dt = datetime.strptime(args[0], "%Y-%m-%d")
        end_dt   = datetime.strptime(args[1], "%Y-%m-%d")
    except:
        print("Invalid date format. Use YYYY-MM-DD YYYY-MM-DD")
        exit(1)

    if end_dt < start_dt:
        print("End date must be ≥ start date.")
        exit(1)

    start_epoch = start_dt.replace(tzinfo=timezone.utc).timestamp()
    end_epoch   = end_dt.replace(tzinfo=timezone.utc).timestamp()

    if end_epoch - start_epoch > MAX_RANGE_SEC:
        start_epoch = end_epoch - MAX_RANGE_SEC
        print(f"NOTE: Range > 31 days — trimmed to last 31 days.")

else:
    print("Usage:\n  gas_plot.py last48\n  gas_plot.py last30\n  gas_plot.py <YYYY-MM-DD> <YYYY-MM-DD>")
    exit(1)


# ======================================================
# FETCH DATA
# ======================================================

conn = sqlite3.connect(DB_PATH)
cur = conn.cursor()

cur.execute("""
    SELECT timestamp, message
    FROM log
    WHERE topic = ?
      AND timestamp >= ?
      AND timestamp <= ?
    ORDER BY timestamp ASC
""", (TOPIC, start_epoch, end_epoch))

rows = cur.fetchall()
conn.close()

if not rows:
    print("NO DATA FOUND for requested date range.")
    exit(1)


# ======================================================
# EXPAND BINS USING CORRECT MIDPOINT LOGIC
# ======================================================

times = []
vals = []

for ts_epoch, msg_json in rows:
    data = json.loads(msg_json)
    counts = data["counts"]
    bin_s  = data["bin_s"]
    N = len(counts)

    for i, v in enumerate(counts):
        midpoint = ts_epoch - (N-(i+1))*bin_s - (bin_s/2)
        dt = datetime.fromtimestamp(midpoint, tz=LOCAL_TZ)
        times.append(dt)
        vals.append(v)


# ======================================================
# SORT
# ======================================================

pairs = sorted(zip(times, vals))
times, vals = zip(*pairs)


# ======================================================
# PLOT USING VLINESS BARS
# ======================================================

plt.figure(figsize=(14,5))
ax = plt.gca()

# Vertical bars (scatter-bar hybrid)
ax.vlines(times, 0, vals,
          color="blue",
          alpha=0.6,
          linewidth=1.2)

plt.title(f"Gas Usage – 0.01 m³ Count Per Bin\n"
          f"{start_dt.date()} → {end_dt.date()} (GMT+8)")

# Zero-axis line
ax.axhline(0, color="black", linewidth=0.8)

# ---- FORCE X-AXIS TO SIT AT EXACTLY y = 0 ----
ax.set_ylim(bottom=0)

# ======================================================
# ADAPTIVE TICKS
# ======================================================

total_days = (end_epoch - start_epoch) / 86400

if total_days <= 5:
    # High-res: 15-min ticks, hourly labels
    ax.xaxis.set_major_locator(mdates.HourLocator(interval=1))
    ax.xaxis.set_minor_locator(mdates.MinuteLocator(byminute=[0, 15, 30, 45]))
    ax.xaxis.set_major_formatter(
        mdates.DateFormatter("%Y-%m-%d %H:%M", tz=LOCAL_TZ)
    )
else:
    # Long range (> 5 days): 3-hourly labels, hourly minor grid
    ax.xaxis.set_major_locator(mdates.HourLocator(interval=3))
    ax.xaxis.set_minor_locator(mdates.HourLocator(interval=1))
    ax.xaxis.set_major_formatter(
        mdates.DateFormatter("%Y-%m-%d %H:%M", tz=LOCAL_TZ)
    )

plt.xticks(rotation=90, fontsize=6)
plt.yticks(range(0, max(vals)+1))
plt.tight_layout()
plt.savefig(PNG_FILE)

print("SAVED PLOT:", PNG_FILE)


# ======================================================
# EMAIL SEND FUNCTION
# ======================================================

def send_email(body, subj, files, account, password):
    msg = email.message.EmailMessage()
    msg["From"]    = account
    msg["To"]      = EMAIL_TO
    msg["Subject"] = subj
    msg["Date"]    = email.utils.formatdate(localtime=True)
    msg.set_content(body)

    for fpath in files:
        with open(fpath, "rb") as f:
            msg.add_attachment(
                f.read(),
                maintype="application",
                subtype="octet-stream",
                filename=os.path.basename(fpath)
            )

    for attempt in range(5):
        try:
            with smtplib.SMTP_SSL(SMTP_SERVER, SMTP_PORT) as smtp:
                smtp.login(account, password)
                smtp.send_message(msg)
            print("EMAIL SENT OK")
            return
        except Exception as e:
            print("EMAIL ERROR:", e)
            if attempt == 3:
                os.system("sudo ifconfig wlan0 down")
                time.sleep(15)
                os.system("sudo ifconfig wlan0 up")
            time.sleep(10)

    print("FAILED TO SEND EMAIL")


# ======================================================
# SEND EMAIL
# ======================================================

BODY = (f"\nGas Usage Chart\nRange: {start_dt.date()} → {end_dt.date()}"
        f"\n(trimmed to 31 days if applicable)\n")

send_email(
    body=BODY,
    subj=f"Gas Meter Usage – {start_dt.date()} to {end_dt.date()}",
    files=[PNG_FILE],
    account=EMAIL_ACCOUNT,
    password=EMAIL_PASSWORD
)

print("DONE.")