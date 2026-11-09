import csv
from pathlib import Path


CSV_FILE = Path(__file__).resolve().parent / "local_sharing_timing.csv"
COLUMN = "contributorsScheduled"

if not CSV_FILE.exists():
    raise FileNotFoundError(
        f"{CSV_FILE} not found. Run the simulation first to generate it."
    )

with CSV_FILE.open(newline="", encoding="utf-8") as file:
    reader = csv.DictReader(file)

    if reader.fieldnames is None or COLUMN not in reader.fieldnames:
        raise ValueError(f"{CSV_FILE} does not contain a {COLUMN} column.")

    total_contributors = 0
    reservation_count = 0

    for row in reader:
        try:
            contributors = int(row[COLUMN])
        except (TypeError, ValueError):
            continue

        total_contributors += contributors
        reservation_count += 1

if reservation_count == 0:
    raise ValueError(f"No valid {COLUMN} values found in {CSV_FILE}.")

average = total_contributors / reservation_count

print(f"LS reservations included: {reservation_count}")
print(f"Average contributors scheduled: {average:.2f}")
