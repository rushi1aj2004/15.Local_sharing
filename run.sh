
#!/usr/bin/env bash
set -Eeuo pipefail

# ============================================================
# Local Sharing sweep: 10 loads x 4 UpmaxGrantBytes = 40 runs
# ============================================================

BASE_INI="omnetpp.ini"
EXE="${EXE:-./15.Local_sharing.exe}"
OUT_ROOT="local_sharing_sweep_results"
MAX_PARALLEL="${1:-4}"

NUM_ONUS=300
#GRANTS=(6250 12500 25000 50000)
GRANTS=(100000 200000 400000)
#LOADS=(0.1 0.2 0.3 0.4 0.5 0.6 0.7 0.8 0.9 1.0)
LOADS=(1.0)
if [[ ! -f "$BASE_INI" ]]; then
    echo "ERROR: $BASE_INI not found in $(pwd)"
    exit 1
fi

if [[ ! -x "$EXE" ]]; then
    echo "ERROR: Executable not found or not executable: $EXE"
    exit 1
fi

if ! [[ "$MAX_PARALLEL" =~ ^[1-9][0-9]*$ ]]; then
    echo "Usage: $0 [number_of_parallel_simulations]"
    exit 1
fi

mkdir -p "$OUT_ROOT"
OUT_ROOT="$(cd "$OUT_ROOT" && pwd)"
BASE_INI="$(cd "$(dirname "$BASE_INI")" && pwd)/$(basename "$BASE_INI")"
EXE="$(cd "$(dirname "$EXE")" && pwd)/$(basename "$EXE")"

# Create one independent INI per simulation.
make_ini() {
    local ini="$1"
    local run_dir="$2"
    local load="$3"
    local grant="$4"

    local per_onu_load
    per_onu_load=$(awk -v l="$load" \
        'BEGIN { printf "%.12g", l/300 }')

    cp "$BASE_INI" "$ini"

    python3 - "$ini" "$run_dir" "$per_onu_load" "$grant" <<'PY'
import sys

path, run_dir, load, grant = sys.argv[1:]

updates = {
    "*.numONUs": "300",
    "*.onu[*].load": load,
    "*.olt.UpmaxGrantBytes": grant,
    "result-dir": run_dir,
    "output-scalar-file": "results.sca",
    "**.scalar-recording": "true",
}

with open(path, encoding="utf-8") as f:
    lines = f.readlines()

seen = set()
output = []

for line in lines:
    stripped = line.strip()

    if not stripped or stripped.startswith("#") or "=" not in line:
        output.append(line)
        continue

    lhs = line.split("=", 1)[0].strip()

    if lhs in updates:
        output.append(f"{lhs} = {updates[lhs]}\n")
        seen.add(lhs)
    else:
        output.append(line)

missing = [key for key in updates if key not in seen]

if missing:
    if output and not output[-1].endswith("\n"):
        output[-1] += "\n"

    output.append("\n# ---- Local Sharing sweep overrides ----\n")
    for key in missing:
        output.append(f"{key} = {updates[key]}\n")

with open(path, "w", encoding="utf-8") as f:
    f.writelines(output)
PY
}

# Extract the final OLT statistics from the .sca file.
extract_metrics() {
    local sca="$1"

    python3 - "$sca" <<'PY'
import sys
import shlex

path = sys.argv[1]
total_packets = ""
mean_delay = ""

try:
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if not line.startswith("scalar "):
                continue

            parts = shlex.split(line)
            if len(parts) < 4:
                continue

            module, name, value = parts[1], parts[2], parts[3]

            if module != "EPONNetwork.olt":
                continue

            if name == "Total DATA Packets Received":
                total_packets = value
            elif name == "Mean DATA Packet Delay (s)":
                mean_delay = value

except OSError as e:
    print(f"Error reading {path}: {e}", file=sys.stderr)

print(f"{mean_delay},{total_packets}")
PY
}

run_one() {
    local load="$1"
    local grant="$2"
    local tag="load_${load}_grant_${grant}"
    local run_dir="$OUT_ROOT/$tag"
    local ini="$run_dir/run.ini"
    local log="$run_dir/run.log"
    local metrics delay packets

    mkdir -p "$run_dir"
    echo "START $tag"

    make_ini "$ini" "$run_dir" "$load" "$grant"

    if "$EXE" -u Cmdenv -n ned -f "$ini" -c General \
        > "$log" 2>&1; then

        metrics=$(extract_metrics "$run_dir/results.sca")
        IFS=',' read -r delay packets <<< "$metrics"

        if [[ -n "$delay" && -n "$packets" ]]; then
            printf '%s,%s,%s,%s,SUCCESS\n' \
                "$load" "$grant" "$delay" "$packets" \
                >> "$OUT_ROOT/summary.csv"
            echo "DONE  $tag"
        else
            printf '%s,%s,,,MISSING_METRICS\n' \
                "$load" "$grant" >> "$OUT_ROOT/summary.csv"
            echo "MISSING METRICS: $tag -- inspect $log and results.sca" >&2
        fi
    else
        printf '%s,%s,,,FAILED\n' "$load" "$grant" \
            >> "$OUT_ROOT/summary.csv"
        echo "FAILED $tag -- inspect $log" >&2
    fi
}

export -f make_ini extract_metrics run_one
export BASE_INI EXE OUT_ROOT NUM_ONUS

printf 'aggregate_load,UpmaxGrantBytes,mean_delay,packets_received,status\n' \
    > "$OUT_ROOT/summary.csv"

for load in "${LOADS[@]}"; do
    for grant in "${GRANTS[@]}"; do
        printf '%s %s\n' "$load" "$grant"
    done
done |
    xargs -n 2 -P "$MAX_PARALLEL" \
        bash -c 'run_one "$0" "$1"'

echo
echo "Local Sharing sweep completed."
echo "Summary: $OUT_ROOT/summary.csv"
echo "Individual results: $OUT_ROOT/"
