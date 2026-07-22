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
    python decode_can_log.py --dbc mycar.dbc --id-map can_frames_decoded_all_values_mcu3.json --csv can_log.csv --out decoded.csv

Output CSV (long format, one row per decoded signal — safer than a wide
table since different messages have different signals):
    timestamp_ms;can_id;message;signal;value;unit

Optional: --id-map lets you pass a JSON reference file (e.g. exported from a
vehicle's firmware binary strings) that only maps CAN ID -> frame/signal
NAMES, with no bit-position/scale/offset info. It is used as a fallback
identification layer for frames the DBC doesn't recognize: the frame gets
its real name instead of 'UNKNOWN', but signal VALUES still can't be
computed (no bit layout available), so they are kept as raw hex data.
Expected JSON shape:
{
  "frames": [
    {"address_dec": 1360, "frame_name": "ADSP_alertLog",
     "signals": [{"signal_name": "ADSP_alertID", ...}, ...]},
    ...
  ]
}

Requires: pip install cantools
"""

import argparse
import csv
import json
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


def load_id_map(path):
    """Loads a name-only ID reference JSON (frame/signal names, no bit layout).

    Returns a dict: {frame_id_int: {"name": str, "signals": [signal names]}}
    """
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)

    id_map = {}
    for frame in data.get("frames", []):
        frame_id = frame.get("address_dec")
        if frame_id is None:
            continue
        signal_names = [s.get("signal_name", "") for s in frame.get("signals", [])]
        id_map[frame_id] = {
            "name": frame.get("frame_name") or f"ID_{frame_id:X}",
            "signals": signal_names,
        }
    return id_map


def main():
    parser = argparse.ArgumentParser(description="Decode a CAN CSV log using a DBC file (cantools).")
    parser.add_argument("--dbc", required=True, help="Path to the .dbc file")
    parser.add_argument("--csv", required=True, help="Path to the input CSV log (timestamp_ms;id;extended;rtr;dlc;data)")
    parser.add_argument("--out", default="decoded.csv", help="Path to the output CSV file (default: decoded.csv)")
    parser.add_argument("--delimiter", default=";", help="Delimiter used in the input CSV (default: ';')")
    parser.add_argument("--strict", action="store_true", help="Use cantools strict mode (fails on ambiguous/overlapping signals)")
    parser.add_argument(
        "--id-map",
        default=None,
        help=(
            "Optional JSON reference file mapping CAN ID -> frame/signal names "
            "(no bit layout). Used as a fallback to name frames the DBC doesn't "
            "recognize; their signal values are still kept as raw hex."
        ),
    )
    args = parser.parse_args()

    try:
        db = cantools.database.load_file(args.dbc, strict=args.strict)
    except Exception as e:
        print(f"ERROR: could not load DBC file '{args.dbc}': {e}", file=sys.stderr)
        sys.exit(1)

    id_map = {}
    if args.id_map:
        try:
            id_map = load_id_map(args.id_map)
        except Exception as e:
            print(f"ERROR: could not load ID map file '{args.id_map}': {e}", file=sys.stderr)
            sys.exit(1)

    decoded_rows = []
    total_frames = 0
    decoded_frames = 0
    unknown_ids = set()
    unknown_frames = 0
    identified_via_map_ids = set()
    identified_via_map_frames = 0
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

            # Extended CAN IDs (29-bit, e.g. 0x7E10824) are stored separately from
            # standard IDs (11-bit, max 0x7FF) inside cantools. If we don't tell it
            # which kind of frame this is, a valid extended ID that IS in the DBC
            # will still raise KeyError and be wrongly marked as unknown.
            is_extended = extended.strip() == "1"
            message = None
            try:
                message = db.get_message_by_frame_id(frame_id, force_extended_id=is_extended)
            except KeyError:
                try:
                    # Fall back to the opposite frame type in case the 'extended'
                    # column in the log doesn't match how the DBC defines it.
                    message = db.get_message_by_frame_id(frame_id, force_extended_id=not is_extended)
                except KeyError:
                    pass

            if message is None:
                # ID not present in the DBC: keep the raw frame instead of dropping it,
                # so unidentified traffic is still visible in the output.
                raw_hex = data.hex(" ").upper()
                map_entry = id_map.get(frame_id)
                if map_entry is not None:
                    # Known name from the reference JSON, but no bit layout to
                    # compute real signal values from -> keep raw data, labeled
                    # with the real frame name instead of UNKNOWN.
                    identified_via_map_ids.add(id_str.strip())
                    identified_via_map_frames += 1
                    decoded_rows.append(
                        (timestamp_ms, id_str.strip(), map_entry["name"], "RAW_DATA", raw_hex, "hex")
                    )
                else:
                    unknown_ids.add(id_str.strip())
                    unknown_frames += 1
                    decoded_rows.append(
                        (timestamp_ms, id_str.strip(), "UNKNOWN", "RAW_DATA", raw_hex, "hex")
                    )
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

    print(f"Total frames read              : {total_frames}")
    print(f"Frames decoded (DBC)           : {decoded_frames}")
    print(f"Decode errors                  : {decode_errors}")
    if id_map:
        print(
            f"Named via ID map, raw data     : {len(identified_via_map_ids)} IDs "
            f"({identified_via_map_frames} frames; values not decoded, no bit layout)"
        )
    print(f"Still unknown (not in DBC/map) : {len(unknown_ids)} IDs ({unknown_frames} frames kept as raw data)")
    if unknown_ids:
        sample = sorted(list(unknown_ids))[:15]
        print(f"  e.g. {', '.join(sample)}")
    print(f"Decoded signal rows written to: {args.out}")


if __name__ == "__main__":
    main()
