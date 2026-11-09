from pathlib import Path

import matplotlib.pyplot as plt
import pandas as pd


CSV_FILE = Path("local_sharing_timing.csv")
OUTPUT_FILE = "local_sharing_metrics_vs_simTime.png"

REQUIRED_COLUMNS = [
    "simulationTime_s",
    "contributorsScheduled",
    "hostOwnDataTime_s",
    "deltaTLSMinusForwardingTime_s",
]

if not CSV_FILE.exists():
    raise FileNotFoundError(
        f"{CSV_FILE} not found. Run the simulation first to generate it."
    )

data = pd.read_csv(CSV_FILE)
missing_columns = [column for column in REQUIRED_COLUMNS if column not in data.columns]
if missing_columns:
    raise ValueError(
        f"{CSV_FILE} is missing required columns: {', '.join(missing_columns)}"
    )

for column in REQUIRED_COLUMNS:
    data[column] = pd.to_numeric(data[column], errors="coerce")

data = data.dropna(subset=REQUIRED_COLUMNS).sort_values("simulationTime_s")
if data.empty:
    raise ValueError(f"{CSV_FILE} contains no valid timing measurements to plot.")

simulation_time = data["simulationTime_s"]
plot_specs = [
    ("contributorsScheduled", "Contributors Scheduled", "Count"),
    ("hostOwnDataTime_s", "Host Own Data Time", "Time (s)"),
    (
        "deltaTLSMinusForwardingTime_s",
        "Delta TLS Before Allocation - Forwarding Time",
        "Time (s)",
    ),
]

fig, axes = plt.subplots(
    nrows=3,
    ncols=1,
    figsize=(11, 9),
    sharex=True,
)

for axis, (column, title, ylabel) in zip(axes, plot_specs):
    axis.plot(simulation_time, data[column], marker=".", linewidth=0.8)
    axis.set_title(title)
    axis.set_ylabel(ylabel)
    axis.grid(True, alpha=0.3)

axes[-1].set_xlabel("Simulation Time (s)")
fig.suptitle("Local Sharing Metrics vs Simulation Time")
fig.tight_layout()
fig.savefig(OUTPUT_FILE, dpi=300, bbox_inches="tight")
plt.show()

print(f"Measurements plotted: {len(data)}")
print(f"Plot saved as {OUTPUT_FILE}")
