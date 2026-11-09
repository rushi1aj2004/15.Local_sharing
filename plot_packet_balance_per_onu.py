"""Plot Created - (Dropped + grant-based Expected TX) for every ONU."""

import argparse
from pathlib import Path

import matplotlib.pyplot as plt

from calculate_packets_transmitted_per_onu import (
    latest_sca_file,
    read_expected_packets,
    read_onu_scalars,
)


def main() -> None:
    script_dir = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(
        description=(
            "Plot per-ONU Created - (Dropped + expected transmitted packets)."
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
    parser.add_argument(
        "--output",
        type=Path,
        help="PNG path (defaults to packet_balance_per_onu.png beside the .sca file).",
    )
    parser.add_argument(
        "--show",
        action="store_true",
        help="Open the plot window after saving the PNG.",
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
    expected_tx, _, _ = read_expected_packets(
        args.csv_directory, args.packet_size
    )

    onu_ids = sorted(set(created_dropped) | set(expected_tx))
    residuals = [
        created_dropped.get(onu_id, {}).get("Packets Created", 0)
        - created_dropped.get(onu_id, {}).get("Packets Dropped", 0)
        - expected_tx.get(onu_id, 0)
        for onu_id in onu_ids
    ]

    output_path = (
        args.output.resolve()
        if args.output
        else sca_path.with_name("packet_balance_per_onu.png")
    )

    figure, axis = plt.subplots(figsize=(12, 6))
    axis.axhline(0, color="black", linewidth=1, linestyle="--")
    axis.scatter(onu_ids, residuals, s=20, alpha=0.75)
    axis.set_xlabel("ONU ID")
    axis.set_ylabel("Created − (Dropped + Expected TX), packets")
    axis.set_title("Per-ONU Packet Balance")
    axis.grid(True, alpha=0.3)
    figure.tight_layout()
    figure.savefig(output_path, dpi=300)
    if args.show:
        plt.show()

    print(f"Scalar file: {sca_path}")
    print(f"Grant CSV directory: {args.csv_directory.resolve()}")
    print(f"ONUs plotted: {len(onu_ids)}")
    print(f"Plot saved to: {output_path}")


if __name__ == "__main__":
    main()
