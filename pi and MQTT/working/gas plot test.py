import matplotlib.pyplot as plt
import matplotlib.dates as mdates
from matplotlib.ticker import NullLocator

import csv
import json
from datetime import datetime, timedelta, timezone

path = 'C:\\Users\\natha\\Downloads'
# Input file (your raw data)
INPUT_FILE = path + '\\' + "rawdata.csv"

# Output file (expanded bins)
OUTPUT_FILE = path + '\\' + "expanded_gas_bins.csv"

# Your timezone
LOCAL_TZ = timezone(timedelta(hours=8))

def expand_gas_data():
    rows_out = []

    with open(INPUT_FILE, "r") as f:
        reader = csv.DictReader(f)

        for row in reader:
            # Parse the timestamp as local time (+08:00)
            ts_local = datetime.fromisoformat(row["timestamp_local"])

            # Parse the JSON message
            msg = json.loads(row["message"])
            bin_s = msg["bin_s"]
            counts = msg["counts"]
            N = len(counts)

            # Expand each bin
            for i, c in enumerate(counts):
                # midpoint timestamp logic (your definition)
                midpoint = ts_local - timedelta(seconds=bin_s * (N - i)) + timedelta(seconds=bin_s / 2)

                rows_out.append({
                    'dt_orig': ts_local.isoformat(),
                    "dt_meas": midpoint.isoformat(),
                    "count": c
                })

    # Write output CSV
    with open(OUTPUT_FILE, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=['dt_orig', 'dt_meas', 'count'])
        w.writeheader()
        w.writerows(rows_out)

    print(f"Done. Wrote {len(rows_out)} rows to {OUTPUT_FILE}")



def plot_data():
    times = []
    vals = []

    # Load expanded CSV
    with open(OUTPUT_FILE, "r") as f:
        reader = csv.DictReader(f)
        for row in reader:
            dt = datetime.fromisoformat(row["dt_meas"])
            c = int(row["count"])
            if c > 0:    # only plot non-zero bins
                times.append(dt)
                vals.append(c)

    fig, ax = plt.subplots(figsize=(15,5))

    # ---- VERTICAL BAR LINES ----
    ax.vlines(times, ymin=0, ymax=vals, colors="blue", alpha=0.6, linewidth=2)

    # ---- FORCE X-AXIS TO SIT AT EXACTLY y = 0 ----
    ax.set_ylim(bottom=0)
    
    # ---- X-AXIS TICKS AND LABELS: HOURLY ----
    ax.xaxis.set_major_locator(mdates.HourLocator(interval=1))
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%Y-%m-%d %H:%M"))

    # Remove minor ticks entirely
    ax.xaxis.set_minor_locator(NullLocator())

    # Smaller font on tick labels
    plt.xticks(rotation=90, fontsize=6)

    plt.ylabel("Pulse Count (0.01 m³)")
    plt.title("Gas Usage – Raw Pulse Events (vertical bars)")

    plt.tight_layout()
    plt.show()




if __name__ == "__main__":
    expand_gas_data()
    plot_data()
