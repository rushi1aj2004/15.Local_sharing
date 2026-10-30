
import pandas as pd
import matplotlib.pyplot as plt

CSV_FILE = "deltaTLS_vs_simTime.csv"
STRIDE = 100
CHUNK_SIZE = 200_000

sampled_time = []
sampled_delta = []
valid_rows = 0
bad_rows = 0

def valid_row(row):
    try:
        return (
            len(row) == 2
            and pd.notna(float(row[0]))
            and pd.notna(float(row[1]))
        )
    except (ValueError, TypeError):
        return False

for chunk in pd.read_csv(
    CSV_FILE,
    chunksize=CHUNK_SIZE,
    header=0,
    on_bad_lines="skip"
):
    # Also ensure both columns contain numeric values.
    chunk["simTime_s"] = pd.to_numeric(
        chunk["simTime_s"], errors="coerce"
    )
    chunk["deltaTLS_s"] = pd.to_numeric(
        chunk["deltaTLS_s"], errors="coerce"
    )
    chunk = chunk.dropna(subset=["simTime_s", "deltaTLS_s"])

    # Sample valid rows at the desired rate.
    positions = range(valid_rows, valid_rows + len(chunk))
    mask = [i % STRIDE == 0 for i in positions]
    sampled = chunk.loc[mask, ["simTime_s", "deltaTLS_s"]]

    sampled_time.extend(sampled["simTime_s"].tolist())
    sampled_delta.extend(sampled["deltaTLS_s"].tolist())

    valid_rows += len(chunk)

plt.figure(figsize=(12, 6))
plt.plot(sampled_time, sampled_delta, linewidth=0.8)
plt.xlabel("Simulation Time (s)")
plt.ylabel("Delta T_LS (s)")
plt.title("Local Sharing Window vs Simulation Time")
plt.grid(True, alpha=0.3)
plt.tight_layout()
plt.savefig("deltaTLS_vs_simTime.png", dpi=200)
plt.show()

print(f"Valid rows processed: {valid_rows}")
print(f"Points plotted: {len(sampled_time)}")
print("Saved plot as deltaTLS_vs_simTime.png")
