"""Plot dropped packets per ONU from an OMNeT++ .sca result file."""

import argparse
import re
import shlex
from pathlib import Path

import matplotlib.pyplot as plt


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_RESULTS_DIR = SCRIPT_DIR / "results"
ONU_PATH = re.compile(r"(?:^|\.)onu\[(\d+)\]$")
DROP_SCALAR_NAMES = {"Packets Dropped", "Number of Packets Dropped"}


def latest_sca_file() -> Path:
    files = list(DEFAULT_RESULTS_DIR.glob("*.sca"))
    if not files:
        raise FileNotFoundError(
            f"No .sca result files found in {DEFAULT_RESULTS_DIR}. "
            "Run the simulation first or pass a result file with --input."
        )
    return max(files, key=lambda path: path.stat().st_mtime)


def read_dropped_packets(sca_path: Path) -> dict[int, int]:
    dropped_by_onu: dict[int, int] = {}

    with sca_path.open(encoding="utf-8") as result_file:
        for line_number, line in enumerate(result_file, start=1):
            if not line.startswith("scalar "):
                continue

            fields = shlex.split(line)
            if len(fields) < 4 or fields[2] not in DROP_SCALAR_NAMES:
                continue

            match = ONU_PATH.search(fields[1])
            if match is None:
                continue

            try:
                dropped = int(fields[3])
            except ValueError as error:
                raise ValueError(
                    f"Invalid dropped-packet count at {sca_path}:{line_number}: "
                    f"{fields[3]!r}"
                ) from error

            onu_id = int(match.group(1))
            dropped_by_onu[onu_id] = dropped_by_onu.get(onu_id, 0) + dropped

    if not dropped_by_onu:
        raise ValueError(
            f"No ONU 'Packets Dropped' scalars found in {sca_path}."
        )

    return dropped_by_onu


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Plot dropped packets against ONU ID from an OMNeT++ .sca file."
    )
    parser.add_argument(
        "--input",
        type=Path,
        help="OMNeT++ .sca file (defaults to the newest file in ./results).",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="PNG output path (defaults to packets_dropped_vs_onu_id.png beside the .sca file).",
    )
    args = parser.parse_args()

    sca_path = args.input.resolve() if args.input else latest_sca_file()
    dropped_by_onu = read_dropped_packets(sca_path)
    onu_ids = sorted(dropped_by_onu)
    counts = [dropped_by_onu[onu_id] for onu_id in onu_ids]
    output_path = (
        args.output.resolve()
        if args.output
        else sca_path.with_name("packets_dropped_vs_onu_id.png")
    )

    figure, axis = plt.subplots(figsize=(10, 6))
    axis.plot(onu_ids, counts, marker=".", linestyle="none", markersize=6)
    axis.set_xlabel("ONU ID")
    axis.set_ylabel("Packets Dropped")
    axis.set_title("Packets Dropped vs ONU ID")
    axis.grid(True, alpha=0.3)
    figure.tight_layout()
    figure.savefig(output_path, dpi=300)
    plt.show()

    print(f"Result file: {sca_path}")
    print(f"Plot saved to: {output_path}")


if __name__ == "__main__":
    main()
