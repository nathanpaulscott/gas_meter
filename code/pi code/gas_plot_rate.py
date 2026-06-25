#!/usr/bin/env python3
# /home/pi/gas_plot_rate.py

import sys
import sqlite3
import json
import os
import time
import math
from bisect import bisect_left, bisect_right
from datetime import datetime, timezone, timedelta
from typing import Iterable, List, Sequence, Tuple

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.dates as mdates

import smtplib
import email


# ======================================================
# CONFIGURATION
# ======================================================

DB_PATH = "/home/pi/mqtt_log.db"
TOPIC = "metering/counts"
PNG_FILE = "/home/pi/gas_plot_rate.png"

EMAIL_TO = "nathan.scott.rf@gmail.com"
EMAIL_ACCOUNT = "nathanpaulscott@yahoo.com"
SMTP_SERVER = "smtp.mail.yahoo.com"
SMTP_PORT = 465

PASSWORD_FILE = "/home/pi/surf_data/yp.sec"

with open(PASSWORD_FILE, "r", encoding="utf-8") as f:
    EMAIL_PASSWORD = f.read().strip()

LOCAL_TZ = timezone(timedelta(hours=8))  # Perth GMT+8
MAX_RANGE_SEC = 31 * 86400
PULSE_M3 = 0.01

# Time-aware smoothing for the irregular pulse-derived flow-rate series.
# These are deliberately conservative so the smoothed line keeps the same
# shape as the raw inferred rate, with only the harsh edges softened.
SMOOTH_SIGMA_S = 180.0     # 3-minute Gaussian width
SMOOTH_RADIUS_S = 900.0    # only use neighbours within +/-15 minutes


# ======================================================
# ARGUMENT PARSING
# ======================================================

def parse_args(argv: Sequence[str]) -> Tuple[float, float, datetime, datetime]:
    args = list(argv)

    if len(args) == 1 and args[0].lower() == "last48":
        end_epoch = time.time()
        start_epoch = end_epoch - 48 * 3600
        start_dt = datetime.fromtimestamp(start_epoch, LOCAL_TZ)
        end_dt = datetime.fromtimestamp(end_epoch, LOCAL_TZ)
        return start_epoch, end_epoch, start_dt, end_dt

    if len(args) == 1 and args[0].lower() == "last30":
        end_epoch = time.time()
        start_epoch = end_epoch - 30 * 86400
        start_dt = datetime.fromtimestamp(start_epoch, LOCAL_TZ)
        end_dt = datetime.fromtimestamp(end_epoch, LOCAL_TZ)
        return start_epoch, end_epoch, start_dt, end_dt

    if len(args) == 2:
        try:
            start_dt = datetime.strptime(args[0], "%Y-%m-%d")
            end_dt = datetime.strptime(args[1], "%Y-%m-%d")
        except ValueError:
            print("Invalid date format. Use YYYY-MM-DD YYYY-MM-DD")
            raise SystemExit(1)

        if end_dt < start_dt:
            print("End date must be ≥ start date.")
            raise SystemExit(1)

        start_epoch = start_dt.replace(tzinfo=timezone.utc).timestamp()
        end_epoch = end_dt.replace(tzinfo=timezone.utc).timestamp()

        if end_epoch - start_epoch > MAX_RANGE_SEC:
            start_epoch = end_epoch - MAX_RANGE_SEC
            print("NOTE: Range > 31 days — trimmed to last 31 days.")

        start_dt_local = datetime.fromtimestamp(start_epoch, LOCAL_TZ)
        end_dt_local = datetime.fromtimestamp(end_epoch, LOCAL_TZ)
        return start_epoch, end_epoch, start_dt_local, end_dt_local

    print("Usage:\n  gas_plot_rate.py last48\n  gas_plot_rate.py last30\n  gas_plot_rate.py <YYYY-MM-DD> <YYYY-MM-DD>")
    raise SystemExit(1)


# ======================================================
# DATA FETCH
# ======================================================

def fetch_rows(db_path: str, topic: str, start_epoch: float, end_epoch: float) -> List[Tuple[float, str]]:
    conn = sqlite3.connect(db_path)
    cur = conn.cursor()
    cur.execute(
        """
        SELECT timestamp, message
        FROM log
        WHERE topic = ?
          AND timestamp >= ?
          AND timestamp <= ?
        ORDER BY timestamp ASC
        """,
        (topic, start_epoch, end_epoch),
    )
    rows = cur.fetchall()
    conn.close()
    return rows


# ======================================================
# BIN / PULSE RECONSTRUCTION
# ======================================================

def iter_bin_windows(row_end_epoch: float, counts: Sequence[int], bin_s: float) -> Iterable[Tuple[float, float, int]]:
    n_bins = len(counts)
    for i, count in enumerate(counts):
        bin_end = row_end_epoch - (n_bins - (i + 1)) * bin_s
        bin_start = bin_end - bin_s
        yield bin_start, bin_end, int(count)


def infer_pulse_times_in_bin(bin_start: float, bin_end: float, count: int) -> List[float]:
    if count <= 0:
        return []

    width = bin_end - bin_start
    if width <= 0:
        return []

    # Using sub-bin centers avoids placing pulses exactly on bin edges.
    return [
        bin_start + ((idx + 0.5) * width / count)
        for idx in range(count)
    ]


def build_inferred_pulse_epochs(rows: Sequence[Tuple[float, str]]) -> List[float]:
    pulse_epochs: List[float] = []

    for row_end_epoch, msg_json in rows:
        try:
            data = json.loads(msg_json)
            counts = data["counts"]
            bin_s = float(data["bin_s"])
        except (KeyError, TypeError, ValueError, json.JSONDecodeError) as exc:
            print(f"Skipping bad row at {row_end_epoch}: {exc}")
            continue

        if not isinstance(counts, list) or bin_s <= 0:
            print(f"Skipping malformed row at {row_end_epoch}")
            continue

        for bin_start, bin_end, count in iter_bin_windows(row_end_epoch, counts, bin_s):
            pulse_epochs.extend(infer_pulse_times_in_bin(bin_start, bin_end, count))

    pulse_epochs.sort()
    return pulse_epochs


# ======================================================
# RATE DERIVATION
# ======================================================

def build_rate_series(pulse_epochs: Sequence[float]) -> Tuple[List[datetime], List[float]]:
    rate_times: List[datetime] = []
    rate_vals: List[float] = []

    if len(pulse_epochs) < 2:
        return rate_times, rate_vals

    prev = pulse_epochs[0]
    for now in pulse_epochs[1:]:
        dt_s = now - prev
        if dt_s > 0:
            midpoint_epoch = prev + dt_s / 2.0
            rate_m3_per_hour = PULSE_M3 * 3600.0 / dt_s
            rate_times.append(datetime.fromtimestamp(midpoint_epoch, tz=LOCAL_TZ))
            rate_vals.append(rate_m3_per_hour)
        prev = now

    return rate_times, rate_vals


def smooth_irregular_time_series(
    rate_times: Sequence[datetime],
    values: Sequence[float],
    sigma_s: float = SMOOTH_SIGMA_S,
    radius_s: float = SMOOTH_RADIUS_S,
) -> List[float]:
    """
    Smooth an irregularly-spaced time series using Gaussian weights in
    real time, not by point count.

    This avoids the broken behaviour of an N-point moving average where
    9 points may represent minutes during active use but hours during
    quiet pilot-light use.
    """
    if not rate_times or not values:
        return []

    if len(rate_times) != len(values):
        raise ValueError("rate_times and values must have the same length")

    if len(values) < 3 or sigma_s <= 0 or radius_s <= 0:
        return list(values)

    epochs = [t.timestamp() for t in rate_times]
    vals = [float(v) for v in values]
    smoothed: List[float] = []

    for t in epochs:
        left = bisect_left(epochs, t - radius_s)
        right = bisect_right(epochs, t + radius_s)

        weighted_sum = 0.0
        weight_total = 0.0

        for j in range(left, right):
            dt_s = epochs[j] - t
            weight = math.exp(-0.5 * (dt_s / sigma_s) ** 2)
            weighted_sum += weight * vals[j]
            weight_total += weight

        if weight_total > 0:
            smoothed.append(weighted_sum / weight_total)
        else:
            smoothed.append(vals[len(smoothed)])

    return smoothed


# ======================================================
# PLOTTING
# ======================================================

def configure_time_axis(ax: plt.Axes, total_days: float) -> None:
    if total_days <= 2:
        ax.xaxis.set_major_locator(mdates.HourLocator(interval=1))
        ax.xaxis.set_minor_locator(mdates.MinuteLocator(byminute=[0, 15, 30, 45]))
        ax.xaxis.set_major_formatter(mdates.DateFormatter("%Y-%m-%d %H:%M", tz=LOCAL_TZ))
    elif total_days <= 5:
        ax.xaxis.set_major_locator(mdates.HourLocator(interval=2))
        ax.xaxis.set_minor_locator(mdates.MinuteLocator(byminute=[0, 30]))
        ax.xaxis.set_major_formatter(mdates.DateFormatter("%Y-%m-%d %H:%M", tz=LOCAL_TZ))
    else:
        ax.xaxis.set_major_locator(mdates.HourLocator(interval=6))
        ax.xaxis.set_minor_locator(mdates.HourLocator(interval=1))
        ax.xaxis.set_major_formatter(mdates.DateFormatter("%Y-%m-%d %H:%M", tz=LOCAL_TZ))


def plot_rate_series(
    rate_times: Sequence[datetime],
    rate_vals: Sequence[float],
    start_dt: datetime,
    end_dt: datetime,
    total_gas_m3: float,
    total_pulses: int,
    output_file: str,
) -> None:
    fig, ax = plt.subplots(figsize=(14, 5))

    if rate_times and rate_vals:
        smooth_vals = smooth_irregular_time_series(rate_times, rate_vals)

        # Raw inferred rate: keep it visible as a reference, but visually quiet.
        ax.plot(
            rate_times,
            rate_vals,
            linestyle=":",
            linewidth=0.8,
            alpha=0.50,
            color="blue",
            label="Raw inferred rate",
        )

        # Main visual line: time-aware smoothing, not point-count moving average.
        ax.plot(
            rate_times,
            smooth_vals,
            linewidth=1.2,
            alpha=0.95,
            color="red",
            label=f"Smoothed rate ({SMOOTH_SIGMA_S / 60:.0f} min Gaussian)",
        )

        max_y = max(max(rate_vals), max(smooth_vals))
        ymax = max_y * 1.10 if max_y > 0 else 1.0
        ax.set_ylim(bottom=0, top=ymax)
    else:
        ax.text(
            0.5,
            0.5,
            "Not enough pulses to derive a flow-rate series",
            ha="center",
            va="center",
            transform=ax.transAxes,
        )
        ax.set_ylim(bottom=0, top=1)

    total_days = max((end_dt.timestamp() - start_dt.timestamp()) / 86400.0, 1e-9)
    configure_time_axis(ax, total_days)

    ax.axhline(0, linewidth=0.8)
    ax.set_ylabel("Flow rate (m³/hour)")
    ax.set_title(
        "Gas Flow Rate – inferred from 0.01 m³ pulses\n"
        f"{start_dt.strftime('%Y-%m-%d %H:%M')} → {end_dt.strftime('%Y-%m-%d %H:%M')} (GMT+8)\n"
        f"Total usage: {total_gas_m3:.2f} m³  |  Pulses: {total_pulses}"
    )

    if rate_times:
        ax.legend(loc="upper right")

    plt.xticks(rotation=90, fontsize=6)
    plt.tight_layout()
    plt.savefig(output_file)
    plt.close(fig)
    print("SAVED PLOT:", output_file)


# ======================================================
# EMAIL
# ======================================================

def send_email(body: str, subj: str, files: Sequence[str], account: str, password: str) -> None:
    msg = email.message.EmailMessage()
    msg["From"] = account
    msg["To"] = EMAIL_TO
    msg["Subject"] = subj
    msg["Date"] = email.utils.formatdate(localtime=True)
    msg.set_content(body)

    for fpath in files:
        with open(fpath, "rb") as f:
            msg.add_attachment(
                f.read(),
                maintype="application",
                subtype="octet-stream",
                filename=os.path.basename(fpath),
            )

    for attempt in range(5):
        try:
            with smtplib.SMTP_SSL(SMTP_SERVER, SMTP_PORT) as smtp:
                smtp.login(account, password)
                smtp.send_message(msg)
            print("EMAIL SENT OK")
            return
        except Exception as exc:
            print("EMAIL ERROR:", exc)
            if attempt == 3:
                os.system("sudo ifconfig wlan0 down")
                time.sleep(15)
                os.system("sudo ifconfig wlan0 up")
            time.sleep(10)

    print("FAILED TO SEND EMAIL")


# ======================================================
# MAIN
# ======================================================

def main() -> None:
    start_epoch, end_epoch, start_dt, end_dt = parse_args(sys.argv[1:])
    rows = fetch_rows(DB_PATH, TOPIC, start_epoch, end_epoch)

    if not rows:
        print("NO DATA FOUND for requested date range.")
        raise SystemExit(1)

    pulse_epochs = build_inferred_pulse_epochs(rows)
    total_pulses = len(pulse_epochs)
    total_gas_m3 = total_pulses * PULSE_M3

    rate_times, rate_vals = build_rate_series(pulse_epochs)

    plot_rate_series(
        rate_times=rate_times,
        rate_vals=rate_vals,
        start_dt=start_dt,
        end_dt=end_dt,
        total_gas_m3=total_gas_m3,
        total_pulses=total_pulses,
        output_file=PNG_FILE,
    )

    body = (
        "\nGas Flow Rate Chart"
        f"\nRange: {start_dt.strftime('%Y-%m-%d %H:%M')} → {end_dt.strftime('%Y-%m-%d %H:%M')}"
        f"\nTotal usage: {total_gas_m3:.2f} m³"
        f"\nPulses: {total_pulses}"
        "\nPulse timing within each bin is inferred by even spacing."
        "\n(trimmed to 31 days if applicable)\n"
    )

    send_email(
        body=body,
        subj=f"Gas Flow Rate – {start_dt.date()} to {end_dt.date()}",
        files=[PNG_FILE],
        account=EMAIL_ACCOUNT,
        password=EMAIL_PASSWORD,
    )

    print("DONE.")


if __name__ == "__main__":
    main()