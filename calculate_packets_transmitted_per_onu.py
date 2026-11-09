"""Compare created/dropped packets with packet capacity in ONU grants."""

import argparse
import csv
import re
import shlex
from pathlib import Path


ONU_FILENAME = re.compile(r"report_turnaround_ONU_(\d+)\.csv$")
ONU_SCALAR_PATH = re.compile(r"(?:^|\.)onu\[(\d+)\]$")
GRANT_COLUMN = "receivedGrantBytes"
SCALAR_NAMES = {"Packets Created", "Packets Dropped"}


def latest_sca_file(results_dir: Path) -> Path:
    files = list(results_dir.glob("*.sca"))
    if not files:
        raise FileNotFoundError(
            f"No .sca result files found in {results_dir}. "
            "Pass a result file with --sca."
        )
    return max(files, key=lambda path: path.stat().st_mtime)


def read_onu_scalars(sca_path: Path) -> dict[int, dict[str, int]]:
    stats: dict[int, dict[str, int]] = {}

    with sca_path.open(encoding="utf-8-sig") as result_file:
        for line_number, line in enumerate(result_file, start=1):
            if not line.startswith("scalar "):
                continue

            fields = shlex.split(line)
            if len(fields) < 4 or fields[2] not in SCALAR_NAMES:
                continue

            match = ONU_SCALAR_PATH.search(fields[1])
            if match is None:
                continue

            try:
                value = int(fields[3])
            except ValueError as error:
                raise ValueError(
                    f"Invalid {fields[2]} scalar in {sca_path}, "
                    f"line {line_number}: {fields[3]!r}"
                ) from error

            onu_id = int(match.group(1))
            onu_stats = stats.setdefault(onu_id, {})
            # Add values if the .sca file contains multiple run records.
            onu_stats[fields[2]] = onu_stats.get(fields[2], 0) + value

    if not stats:
        raise ValueError(f"No ONU packet scalars found in {sca_path}.")

    for onu_id, values in stats.items():
        missing = SCALAR_NAMES.difference(values)
        if missing:
            missing_names = ", ".join(sorted(missing))
            raise ValueError(
                f"ONU {onu_id} is missing these scalars in {sca_path}: "
                f"{missing_names}"
            )

    return stats


def read_expected_packets(
    csv_directory: Path, packet_size: int
) -> tuple[dict[int, int], dict[int, int], int]:
    packets_by_onu: dict[int, int] = {}
    grants_by_onu: dict[int, int] = {}
    remainder_bytes = 0

    csv_files = sorted(csv_directory.glob("report_turnaround_ONU_*.csv"))
    if not csv_files:
        raise FileNotFoundError(
            f"No report_turnaround_ONU_*.csv files found in {csv_directory}"
        )

    for csv_path in csv_files:
        match = ONU_FILENAME.fullmatch(csv_path.name)
        if match is None:
            continue
        onu_id = int(match.group(1))

        with csv_path.open(newline="", encoding="utf-8-sig") as file:
            reader = csv.DictReader(file)
            if reader.fieldnames is None or GRANT_COLUMN not in reader.fieldnames:
                raise ValueError(
                    f"{csv_path} does not contain a {GRANT_COLUMN!r} column."
                )

            for row_number, row in enumerate(reader, start=2):
                raw_grant = row.get(GRANT_COLUMN, "")
                try:
                    grant_bytes = int(raw_grant)
                except (TypeError, ValueError) as error:
                    raise ValueError(
                        f"Invalid {GRANT_COLUMN} value in "
                        f"{csv_path.name}, row {row_number}: {raw_grant!r}"
                    ) from error

                if grant_bytes < 0:
                    raise ValueError(
                        f"Negative {GRANT_COLUMN} value in "
                        f"{csv_path.name}, row {row_number}: {grant_bytes}"
                    )

                packets_by_onu[onu_id] = (
                    packets_by_onu.get(onu_id, 0)
                    + grant_bytes // packet_size
                )
                grants_by_onu[onu_id] = grants_by_onu.get(onu_id, 0) + 1
                remainder_bytes += grant_bytes % packet_size

    return packets_by_onu, grants_by_onu, remainder_bytes


def main() -> None:
    script_dir = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(
        description=(
            "Calculate Created - (Dropped + expected transmitted packets) "
            "for each ONU. Expected transmissions are estimated from grants."
        )
    )
    parser.add_argument(
        "--sca",
        type=Path,
        help="OMNeT++ .sca file (defaults to the newest file in ./results).",
    )
    parser.add_argument(
        "--results-directory",
        type=Path,
        default=script_dir / "results",
        help="Directory searched for the default .sca file.",
    )
    parser.add_argument(
        "--csv-directory",
        type=Path,
        default=script_dir,
        help="Directory containing report_turnaround_ONU_*.csv files.",
    )
    parser.add_argument(
        "--packet-size",
        type=int,
        default=1500,
        help="Packet size in bytes (default: 1500).",
    )
    args = parser.parse_args()

    if args.packet_size <= 0:
        parser.error("--packet-size must be greater than zero")

    sca_path = (
        args.sca.resolve()
        if args.sca
        else latest_sca_file(args.results_directory)
    )
    created_dropped = read_onu_scalars(sca_path)
    expected_tx, grants_by_onu, remainder_bytes = read_expected_packets(
        args.csv_directory, args.packet_size
    )

    all_onu_ids = sorted(set(created_dropped) | set(expected_tx))
    total_created = 0
    total_dropped = 0
    total_expected_tx = 0
    total_residual = 0

    print(f"Scalar file: {sca_path}")
    print(f"Grant CSV directory: {args.csv_directory.resolve()}")
    print(f"Packet size: {args.packet_size} bytes")
    print(
        "\nONU    Created    Dropped    Expected TX    Created-(Dropped+TX)    Grants"
    )

    for onu_id in all_onu_ids:
        scalar_values = created_dropped.get(onu_id, {})
        created = scalar_values.get("Packets Created", 0)
        dropped = scalar_values.get("Packets Dropped", 0)
        expected_tx_count = expected_tx.get(onu_id, 0)
        residual = created - (dropped + expected_tx_count)
        grant_count = grants_by_onu.get(onu_id, 0)

        print(
            f"{onu_id:>3} {created:>11} {dropped:>10} "
            f"{expected_tx_count:>14} {residual:>22} {grant_count:>8}"
        )

        total_created += created
        total_dropped += dropped
        total_expected_tx += expected_tx_count
        total_residual += residual

    print(
        f"TOT {total_created:>11} {total_dropped:>10} "
        f"{total_expected_tx:>14} {total_residual:>22}"
    )

    if remainder_bytes:
        print(
            "\nGrant bytes that did not form a whole packet were excluded "
            f"(sum of per-grant remainders: {remainder_bytes} bytes)."
        )

    missing_csv_onus = sorted(set(created_dropped) - set(expected_tx))
    if missing_csv_onus:
        print(
            "\nWarning: no grant CSV rows were found for ONU IDs: "
            + ", ".join(map(str, missing_csv_onus))
        )

    csv_only_onus = sorted(set(expected_tx) - set(created_dropped))
    if csv_only_onus:
        print(
            "\nWarning: grant CSVs contain ONU IDs absent from the .sca file: "
            + ", ".join(map(str, csv_only_onus))
        )


if __name__ == "__main__":
    main()
