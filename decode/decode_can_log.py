#!/usr/bin/env python3
"""
decode_can_log.py

Decodes a CAN log CSV file (format: timestamp_ms;id;extended;rtr;dlc;data)
using a DBC file, and exports the decoded signals to a new CSV file.

Expected input CSV columns (semicolon-separated), matching the CanSniffer
firmware output:
    timestamp_ms;id;extended;rtr;dlc;data
    1523;0x1A0;0;0;8;12 3A FF 00 00 00 07 E1

Lines starting with '#' (comments/status messages) are ignored.

Usage:
    python3 decode_can_log.py --dbc mycar.dbc --csv can_log.csv --out decoded.csv

Output CSV (long format, one row per decoded signal — safer than a wide
table since different messages have different signals):
    timestamp_ms;can_id;message;signal;value;unit

Requires: pip install cantools
"""

import argparse
import csv
import sys

import cantools


def parse_data_bytes(data_str):
    """Converts a 'AA BB CC' hex string into a bytes object."""
    data_str = (data_str or "").strip()
    if not data_str:
        return b""
    return bytes(int(b, 16) for b in data_str.split())


def parse_can_id(id_str):
    """Parses an id field like '0x1A0' or '1A0' or '424' into an int."""
    id_str = id_str.strip()
    if id_str.lower().startswith("0x"):
        return int(id_str, 16)
    return int(id_str, 16)  # assume hex even without 0x prefix (matches our log format)


def main():
    parser = argparse.ArgumentParser(description="Decode a CAN CSV log using a DBC file (cantools).")
    parser.add_argument("--dbc", required=True, help="Path to the .dbc file")
    parser.add_argument("--csv", required=True, help="Path to the input CSV log (timestamp_ms;id;extended;rtr;dlc;data)")
    parser.add_argument("--out", default="decoded.csv", help="Path to the output CSV file (default: decoded.csv)")
    parser.add_argument("--delimiter", default=";", help="Delimiter used in the input CSV (default: ';')")
    parser.add_argument("--strict", action="store_true", help="Use cantools strict mode (fails on ambiguous/overlapping signals)")
    args = parser.parse_args()

    try:
        db = cantools.database.load_file(args.dbc, strict=args.strict)
    except Exception as e:
        print(f"ERROR: could not load DBC file '{args.dbc}': {e}", file=sys.stderr)
        sys.exit(1)

    decoded_rows = []
    total_frames = 0
    decoded_frames = 0
    unknown_ids = set()
    decode_errors = 0

    with open(args.csv, "r", encoding="utf-8", errors="replace") as f:
        reader = csv.reader(f, delimiter=args.delimiter)
        header_skipped = False

        for row in reader:
            if not row or not row[0].strip():
                continue
            if row[0].strip().startswith("#"):
                continue  # skip status/comment lines
            if not header_skipped and row[0].strip().lower() in ("timestamp_ms", "timestamp"):
                header_skipped = True
                continue

            if len(row) < 6:
                continue  # malformed row, skip

            timestamp_ms, id_str, extended, rtr, dlc, data_str = row[:6]
            total_frames += 1

            if rtr.strip() == "1":
                continue  # RTR frames carry no data to decode

            try:
                frame_id = parse_can_id(id_str)
                data = parse_data_bytes(data_str)
            except Exception:
                decode_errors += 1
                continue

            try:
                message = db.get_message_by_frame_id(frame_id)
            except KeyError:
                unknown_ids.add(id_str.strip())
                continue

            try:
                signals = message.decode(data, decode_choices=True, allow_truncated=True)
            except Exception:
                decode_errors += 1
                continue

            decoded_frames += 1
            for signal_name, value in signals.items():
                sig_def = message.get_signal_by_name(signal_name)
                unit = sig_def.unit or ""
                decoded_rows.append((timestamp_ms, id_str.strip(), message.name, signal_name, value, unit))

    with open(args.out, "w", encoding="utf-8", newline="") as f:
        writer = csv.writer(f, delimiter=";")
        writer.writerow(["timestamp_ms", "can_id", "message", "signal", "value", "unit"])
        writer.writerows(decoded_rows)

    print(f"Total frames read       : {total_frames}")
    print(f"Frames decoded          : {decoded_frames}")
    print(f"Decode errors           : {decode_errors}")
    print(f"Unknown IDs (not in DBC): {len(unknown_ids)}")
    if unknown_ids:
        sample = sorted(list(unknown_ids))[:15]
        print(f"  e.g. {', '.join(sample)}")
    print(f"Decoded signal rows written to: {args.out}")


if __name__ == "__main__":
    main()
