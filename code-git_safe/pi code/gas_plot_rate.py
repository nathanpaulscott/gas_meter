#!/usr/bin/env python3
# /home/pi/gas_plot_rate.py

import sys
import sqlite3
import json
import os
import time
from datetime import datetime, timezone, timedelta
from typing import Iterable, List, Optional, Sequence, Tuple

import numpy as np

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.dates as mdates

import smtplib
import email


# ======================================================
# CONFIGURATION
# ======================================================

DB_PATH = "/home/pi/gasmon/mqtt_log.db"
TOPIC = "metering/counts"
PNG_FILE = "/home/pi/gasmon/gas_plot_rate.png"

EMAIL_TO = "nathan.scott.rf@gmail.com"
EMAIL_ACCOUNT = "nathanpaulscott@yahoo.com"
SMTP_SERVER = "smtp.mail.yahoo.com"
SMTP_PORT = 465

#passwords are stored locally only...
###########################################
PASSWORD_FILE = "/home/pi/gasmon/yp.sec"
with open(PASSWORD_FILE, "r", encoding="utf-8") as f:
    EMAIL_PASSWORD = f.read().strip()
###########################################

LOCAL_TZ = timezone(timedelta(hours=8))  # Perth GMT+8
MAX_RANGE_SEC = 31 * 86400
PULSE_M3 = 0.01

# Centered exponential moving-average settings.
# Raw pulse-derived rate samples are irregular. This code first resamples them
# onto a regular time grid, then smooths that regular series using a centered
# exponential-weighted average. The smoother uses past and future samples.
RESAMPLE_GRID_S = 100.0       # 2 minute grid. Try 300.0 for a calmer curve.
CENTERED_EMA_RADIUS = 8       # 8 before + current + 8 after = 17 samples.
CENTERED_EMA_DECAY = 2.0      # lower = more local; higher = flatter/smoother.

# Blue underlay mode.
#   "pulses"   = original raw DB pulse-count bins on a right-side y-axis
#   "raw_rate" = unsmoothed derived flow-rate grid on the main y-axis
UNDERLAY_MODE = "pulses"


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


def build_raw_bin_count_series(rows: Sequence[Tuple[float, str]]) -> Tuple[List[datetime], List[int]]:
    """
    Build the original raw gas-pulse bin-count series for underlay plotting.

    This is the direct DB/bin-count view: one timestamp per bin midpoint and
    one value equal to the number of pulses detected in that bin. It is kept
    separate from the derived flow-rate series because the units are different.
    """
    bin_times: List[datetime] = []
    bin_counts: List[int] = []

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
            midpoint_epoch = bin_start + (bin_end - bin_start) / 2.0
            bin_times.append(datetime.fromtimestamp(midpoint_epoch, tz=LOCAL_TZ))
            bin_counts.append(int(count))

    pairs = sorted(zip(bin_times, bin_counts))
    if not pairs:
        return [], []

    times, counts = zip(*pairs)
    return list(times), list(counts)


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
    centered smoother behaviour more predictable than fitting directly to a
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

    # Remove duplicate timestamps by averaging their values.
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


def centered_exponential_average(
    grid_times: Sequence[datetime],
    grid_vals: Sequence[float],
    radius: int = CENTERED_EMA_RADIUS,
    decay: float = CENTERED_EMA_DECAY,
) -> Tuple[List[datetime], List[float]]:
    """
    Smooth a regular-grid gas flow-rate series using a centered exponential
    weighted average.

    For each point, the smoother looks both backward and forward by `radius`
    samples. Nearby samples receive more weight than distant samples.
    """
    if not grid_times or not grid_vals:
        return [], []

    if len(grid_times) != len(grid_vals):
        raise ValueError("grid_times and grid_vals must have the same length")

    y = np.array(grid_vals, dtype=float)
    n = len(y)

    if n < 3:
        return list(grid_times), [float(v) for v in y]

    radius = max(1, int(radius))
    decay = max(float(decay), 1e-9)

    offsets = np.arange(-radius, radius + 1, dtype=float)
    weights_full = np.exp(-np.abs(offsets) / decay)

    smooth = np.empty_like(y)

    for i in range(n):
        left = max(0, i - radius)
        right = min(n, i + radius + 1)

        # Slice the full symmetric weight vector to match the truncated data
        # window at the start/end of the series.
        w_left = radius - (i - left)
        w_right = radius + (right - i)
        weights = weights_full[w_left:w_right]
        vals = y[left:right]

        smooth[i] = float(np.sum(weights * vals) / np.sum(weights))

    return list(grid_times), [float(v) for v in smooth]


# ======================================================
# PLOTTING
# ======================================================

def configure_time_axis(ax: plt.Axes, total_days: float) -> None:
    # This is intentionally the same locator/formatter logic as the old
    # working single-axis plot.
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


def style_xaxis_like_old_plot(
    fig: plt.Figure,
    ax_rate: plt.Axes,
    ax_secondary: Optional[plt.Axes] = None,
) -> None:
    """
    Force the bottom x-axis to behave like the old working plot.

    If a secondary axis exists for the raw pulse-count underlay, it must not
    own or draw x-axis labels. The primary rate axis always owns the vertical
    date labels.
    """
    if ax_secondary is not None:
        ax_secondary.tick_params(
            axis="x",
            which="both",
            bottom=False,
            top=False,
            labelbottom=False,
            labeltop=False,
        )

    ax_rate.tick_params(axis="x", which="major", labelrotation=90, labelsize=6, labelbottom=True)

    fig.canvas.draw()
    for label in ax_rate.get_xticklabels(which="major"):
        label.set_rotation(90)
        label.set_fontsize(6)
        label.set_horizontalalignment("center")
        label.set_verticalalignment("top")
        label.set_visible(True)


def underlay_description() -> str:
    mode = UNDERLAY_MODE.lower().strip()
    if mode == "pulses":
        return "raw gas-pulse counts per bin"
    if mode == "raw_rate":
        return "unsmoothed resampled flow-rate grid"
    return f"unknown UNDERLAY_MODE={UNDERLAY_MODE!r}"


def plot_pulse_count_underlay(
    ax_rate: plt.Axes,
    raw_bin_times: Sequence[datetime],
    raw_bin_counts: Sequence[int],
    start_dt: datetime,
    end_dt: datetime,
) -> plt.Axes:
    """
    Plot the original DB pulse-count bins as a blue underlay on a secondary
    right-side y-axis.

    This is the original raw data view: count per bin, not derived flow rate.
    """
    ax_pulse = ax_rate.twinx()
    ax_pulse.set_zorder(1)
    ax_rate.set_zorder(2)
    ax_rate.patch.set_visible(False)

    if raw_bin_times and raw_bin_counts:
        pulse_times = [t for t, c in zip(raw_bin_times, raw_bin_counts) if c > 0]
        pulse_counts = [c for c in raw_bin_counts if c > 0]

        if pulse_times:
            ax_pulse.vlines(
                pulse_times,
                0,
                pulse_counts,
                linestyle="-",
                linewidth=1.0,
                alpha=0.25,
                color="blue",
                label="Raw gas pulses per bin",
            )

        max_pulse_count = max(raw_bin_counts) if raw_bin_counts else 0
        ax_pulse.set_ylim(bottom=0, top=max(max_pulse_count * 1.15, 1.0))
    else:
        ax_pulse.set_ylim(bottom=0, top=1)

    ax_pulse.set_ylabel("Raw pulse count per bin")
    ax_pulse.set_xlim(left=start_dt, right=end_dt)
    return ax_pulse


def plot_unsmoothed_rate_underlay(
    ax_rate: plt.Axes,
    grid_times: Sequence[datetime],
    grid_vals: Sequence[float],
) -> None:
    """
    Plot the unsmoothed derived flow-rate grid as a blue underlay on the main
    flow-rate y-axis.

    This is not the raw DB count data. It is the regular-grid rate series before
    the centered EMA is applied.
    """
    if not grid_times or not grid_vals:
        return

    ax_rate.plot(
        grid_times,
        grid_vals,
        linestyle="-",
        linewidth=0.8,
        alpha=0.30,
        color="blue",
        label="Unsmoothed rate grid",
    )


def plot_rate_series(
    rate_times: Sequence[datetime],
    rate_vals: Sequence[float],
    raw_bin_times: Sequence[datetime],
    raw_bin_counts: Sequence[int],
    start_dt: datetime,
    end_dt: datetime,
    total_gas_m3: float,
    total_pulses: int,
    output_file: str,
) -> None:
    fig, ax_rate = plt.subplots(figsize=(14, 5))

    mode = UNDERLAY_MODE.lower().strip()
    if mode not in {"pulses", "raw_rate"}:
        raise ValueError('UNDERLAY_MODE must be "pulses" or "raw_rate"')

    ax_secondary: Optional[plt.Axes] = None

    if mode == "pulses":
        ax_secondary = plot_pulse_count_underlay(
            ax_rate=ax_rate,
            raw_bin_times=raw_bin_times,
            raw_bin_counts=raw_bin_counts,
            start_dt=start_dt,
            end_dt=end_dt,
        )

    if rate_times and rate_vals:
        grid_times, grid_vals = resample_rate_series(rate_times, rate_vals)
        smooth_times, smooth_vals = centered_exponential_average(grid_times, grid_vals)

        if mode == "raw_rate":
            plot_unsmoothed_rate_underlay(ax_rate, grid_times, grid_vals)

        # Main visual line: centered exponential average on the regular-grid rate series.
        ax_rate.plot(
            smooth_times,
            smooth_vals,
            linewidth=1.5,
            alpha=0.95,
            color="red",
            label=f"Centered EMA, ±{CENTERED_EMA_RADIUS} samples",
        )

        max_y = max(max(rate_vals), max(grid_vals), max(smooth_vals))
        ymax = max_y * 1.10 if max_y > 0 else 1.0
        ax_rate.set_ylim(bottom=0, top=ymax)
        ax_rate.set_xlim(left=start_dt, right=end_dt)
    else:
        ax_rate.text(
            0.5,
            0.5,
            "Not enough pulses to derive a flow-rate series",
            ha="center",
            va="center",
            transform=ax_rate.transAxes,
        )
        ax_rate.set_ylim(bottom=0, top=1)
        ax_rate.set_xlim(left=start_dt, right=end_dt)

    total_days = max((end_dt.timestamp() - start_dt.timestamp()) / 86400.0, 1e-9)
    configure_time_axis(ax_rate, total_days)

    ax_rate.axhline(0, linewidth=0.8)
    ax_rate.set_ylabel("Flow rate (m³/hour)")
    ax_rate.set_title(
        "Gas Flow Rate – inferred from 0.01 m³ pulses\n"
        f"{start_dt.strftime('%Y-%m-%d %H:%M')} → {end_dt.strftime('%Y-%m-%d %H:%M')} (GMT+8)\n"
        f"Total usage: {total_gas_m3:.2f} m³  |  Pulses: {total_pulses}"
    )

    handles_rate, labels_rate = ax_rate.get_legend_handles_labels()
    if ax_secondary is not None:
        handles_secondary, labels_secondary = ax_secondary.get_legend_handles_labels()
        handles = handles_rate + handles_secondary
        labels = labels_rate + labels_secondary
    else:
        handles = handles_rate
        labels = labels_rate

    if handles:
        ax_rate.legend(handles, labels, loc="upper right")

    style_xaxis_like_old_plot(fig, ax_rate, ax_secondary)
    plt.sca(ax_rate)
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
    raw_bin_times, raw_bin_counts = build_raw_bin_count_series(rows)
    total_pulses = len(pulse_epochs)
    total_gas_m3 = total_pulses * PULSE_M3

    rate_times, rate_vals = build_rate_series(pulse_epochs)

    plot_rate_series(
        rate_times=rate_times,
        rate_vals=rate_vals,
        raw_bin_times=raw_bin_times,
        raw_bin_counts=raw_bin_counts,
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
        f"\nCentered EMA radius: ±{CENTERED_EMA_RADIUS} samples"
        f"\nCentered EMA decay: {CENTERED_EMA_DECAY:.1f}"
        f"\nUnderlay: {underlay_description()}"
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
