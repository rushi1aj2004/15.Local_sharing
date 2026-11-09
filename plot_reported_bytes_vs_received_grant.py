import glob

import matplotlib.pyplot as plt
import pandas as pd


CSV_PATTERN = "report_turnaround_ONU_*.csv"
OUTPUT_FILE = "reportedBytes_vs_receivedGrant.png"
REQUIRED_COLUMNS = {"reportedBytes", "receivedGrantBytes"}

files = sorted(glob.glob(CSV_PATTERN))

if not files:
    raise FileNotFoundError(f"No {CSV_PATTERN} files found in the current directory.")

dataframes = []
files_without_grant_data = []

for file in files:
    df = pd.read_csv(file)

    if not REQUIRED_COLUMNS.issubset(df.columns):
        files_without_grant_data.append(file)
        continue

    df["reportedBytes"] = pd.to_numeric(df["reportedBytes"], errors="coerce")
    df["receivedGrantBytes"] = pd.to_numeric(
        df["receivedGrantBytes"], errors="coerce"
    )
    df = df.dropna(subset=["reportedBytes", "receivedGrantBytes"])

    if not df.empty:
        dataframes.append(df[["reportedBytes", "receivedGrantBytes"]])

if not dataframes:
    raise ValueError(
        "No usable grant measurements found. The CSV files must include "
        "reportedBytes and receivedGrantBytes columns; rerun the simulation "
        "with the updated ONU code to generate them."
    )

data = pd.concat(dataframes, ignore_index=True)

plt.figure(figsize=(10, 6))
plt.scatter(
    data["reportedBytes"],
    data["receivedGrantBytes"],
    s=12,
    alpha=0.5,
)
plt.xlabel("Reported Bytes")
plt.ylabel("Received Grant (bytes)")
plt.title("Reported Bytes vs Received Grant for All ONUs")
plt.grid(True, alpha=0.3)
plt.tight_layout()
plt.savefig(OUTPUT_FILE, dpi=300)
plt.show()

print(f"ONU CSV files found: {len(files)}")
print(f"Files without receivedGrantBytes data: {len(files_without_grant_data)}")
print(f"Measurements plotted: {len(data)}")
print(f"Plot saved as {OUTPUT_FILE}")
