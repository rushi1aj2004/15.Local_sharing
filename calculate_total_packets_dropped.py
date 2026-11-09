"""Sum the per-ONU dropped-packet scalars in an OMNeT++ .sca result file."""

import argparse
import re
import shlex
from pathlib import Path


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
        description="Calculate total dropped packets across ONUs from an OMNeT++ .sca file."
    )
    parser.add_argument(
        "--input",
        type=Path,
        help="OMNeT++ .sca file (defaults to the newest file in ./results).",
    )
    args = parser.parse_args()

    sca_path = args.input.resolve() if args.input else latest_sca_file()
    dropped_by_onu = read_dropped_packets(sca_path)
    total_dropped = sum(dropped_by_onu.values())

    print(f"Result file: {sca_path}")
    print(f"ONUs counted: {len(dropped_by_onu)}")
    print(f"Total packets dropped: {total_dropped}")


if __name__ == "__main__":
    main()
