#!/usr/bin/env python3
# /home/pi/gas_plot_rate.py

import sys
import sqlite3
import json
import os
import time
from datetime import datetime, timezone, timedelta
from typing import Iterable, List, Sequence, Tuple

import numpy as np

try:
    from scipy.interpolate import UnivariateSpline
except ImportError:
    print("ERROR: scipy is required for smoothing spline plotting.")
    print("Install on Raspberry Pi OS with: sudo apt install python3-scipy")
    raise SystemExit(1)

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

# Spline plot settings.
# Raw pulse-derived rate samples are irregular. This code first resamples them
# onto a regular time grid, then applies a smoothing spline to that regular
# series. Smaller residual = follows raw line more closely. Larger residual =
# smoother/flatter curve.
RESAMPLE_GRID_S = 150.0       # 2.5 minute grid. Try 300.0 for 5 minutes.
SPLINE_RESIDUAL_M3H = 0.025    # typical allowed residual per point, in m³/hour
SPLINE_ORDER = 3              # cubic smoothing spline when enough points exist


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

    # Using sub-bin centres avoids placing pulses exactly on bin edges.
    # count=1 gives the bin centre. count=N spreads N pulses evenly inside the bin.
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
    """
    Convert inferred pulse timestamps into interval-rate samples.

    Each pulse represents 0.01 m³. For each consecutive pulse pair, the gas
    flow rate is assigned to the midpoint between those two pulses.
    """
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


def resample_rate_series(
    rate_times: Sequence[datetime],
    rate_vals: Sequence[float],
    grid_s: float = RESAMPLE_GRID_S,
) -> Tuple[List[datetime], List[float]]:
    """
    Resample the irregular midpoint rate samples onto a regular time grid.

    This uses simple linear interpolation. The regular grid then makes the
    smoothing spline behaviour more predictable than fitting directly to a
    highly irregular point sequence.
    """
    if not rate_times or not rate_vals:
        return [], []

    if len(rate_times) != len(rate_vals):
        raise ValueError("rate_times and rate_vals must have the same length")

    if len(rate_times) == 1:
        return list(rate_times), list(rate_vals)

    epochs = np.array([t.timestamp() for t in rate_times], dtype=float)
    vals = np.array(rate_vals, dtype=float)

    # Remove duplicate timestamps by averaging their values. UnivariateSpline
    # requires strictly increasing x values.
    unique_epochs: List[float] = []
    unique_vals: List[float] = []

    i = 0
    while i < len(epochs):
        t = epochs[i]
        same_vals = [vals[i]]
        i += 1
        while i < len(epochs) and epochs[i] == t:
            same_vals.append(vals[i])
            i += 1
        unique_epochs.append(float(t))
        unique_vals.append(float(np.mean(same_vals)))

    epochs = np.array(unique_epochs, dtype=float)
    vals = np.array(unique_vals, dtype=float)

    if len(epochs) < 2:
        return [datetime.fromtimestamp(float(epochs[0]), tz=LOCAL_TZ)], [float(vals[0])]

    grid_epochs = np.arange(epochs[0], epochs[-1] + grid_s, grid_s, dtype=float)
    grid_vals = np.interp(grid_epochs, epochs, vals)

    grid_times = [datetime.fromtimestamp(float(t), tz=LOCAL_TZ) for t in grid_epochs]
    return grid_times, [float(v) for v in grid_vals]


def smoothing_spline_rate_series(
    grid_times: Sequence[datetime],
    grid_vals: Sequence[float],
    residual_m3h: float = SPLINE_RESIDUAL_M3H,
    spline_order: int = SPLINE_ORDER,
) -> Tuple[List[datetime], List[float]]:
    """
    Apply a smoothing spline to a regular-grid gas flow-rate series.

    residual_m3h controls the smoothing strength:
      - smaller value: closer to raw/resampled data, less smoothing
      - larger value: smoother curve, more deviation from raw/resampled data

    Negative rates are clipped to zero because negative gas flow is not physical.
    """
    if not grid_times or not grid_vals:
        return [], []

    if len(grid_times) != len(grid_vals):
        raise ValueError("grid_times and grid_vals must have the same length")

    n = len(grid_vals)
    if n < 4:
        return list(grid_times), list(grid_vals)

    epochs = np.array([t.timestamp() for t in grid_times], dtype=float)
    y = np.array(grid_vals, dtype=float)

    # Use hours from the first sample to keep x values numerically small.
    x_hours = (epochs - epochs[0]) / 3600.0

    # Smoothing parameter s is the allowed sum of squared residuals.
    # Using n * residual^2 makes the knob intuitive in m³/hour units.
    residual_m3h = max(float(residual_m3h), 0.0)
    s = n * (residual_m3h ** 2)

    k = min(int(spline_order), n - 1)
    spline = UnivariateSpline(x_hours, y, k=k, s=s)
    y_smooth = spline(x_hours)

    # Gas flow cannot be negative. Spline overshoot can otherwise create
    # small negative dips around sharp transitions.
    y_smooth = np.maximum(y_smooth, 0.0)

    return list(grid_times), [float(v) for v in y_smooth]


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
        grid_times, grid_vals = resample_rate_series(rate_times, rate_vals)
        spline_times, spline_vals = smoothing_spline_rate_series(grid_times, grid_vals)

        # Raw inferred rate: visible as reference, but visually quiet.
        ax.plot(
            rate_times,
            rate_vals,
            linestyle=":",
            linewidth=0.8,
            alpha=0.35,
            color="blue",
            label="Raw inferred rate",
        )

        # Optional resampled reference. Kept very faint so it does not dominate.
        ax.plot(
            grid_times,
            grid_vals,
            linestyle="--",
            linewidth=0.7,
            alpha=0.20,
            color="gray",
            label=f"Resampled {RESAMPLE_GRID_S / 60:.1f} min grid",
        )

        # Main visual line: smoothing spline on the regular-grid rate series.
        ax.plot(
            spline_times,
            spline_vals,
            linewidth=1.5,
            alpha=0.95,
            color="red",
            label=f"Smoothing spline, residual {SPLINE_RESIDUAL_M3H:.2f} m³/h",
        )

        max_y = max(max(rate_vals), max(grid_vals), max(spline_vals))
        ymax = max_y * 1.10 if max_y > 0 else 1.0
        ax.set_ylim(bottom=0, top=ymax)
        ax.set_xlim(left=start_dt, right=end_dt)
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
        ax.set_xlim(left=start_dt, right=end_dt)

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
        f"\nResample grid: {RESAMPLE_GRID_S / 60:.1f} minutes"
        f"\nSmoothing spline residual: {SPLINE_RESIDUAL_M3H:.2f} m³/hour"
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
