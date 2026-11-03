
#!/usr/bin/env python3
# Experimental Dolby MAT / TrueHD AU scanner V0.1
#
# Input: byte-swapped MAT stream, IEC headers already removed.
# Current assumptions:
#   MAT physical frame: 61424 bytes
#   MAT logical frame:  61440 bytes
#   Substreams:         4
#   Samples per frame:  40
#   Nominal AU spacing: 2560
#
# Default operation is report-only.
# This is an experimental parser, not a general-purpose TrueHD parser.

import argparse
import csv
import sys
from dataclasses import dataclass
from pathlib import Path

MAT_FRAME = 61424
LOGICAL_FRAME = 61440
SUBSTREAMS = 4
SAMPLES_PER_FRAME = 40
NOMINAL_SPACING = 2560
BYTES_PER_TIMING_TICK = NOMINAL_SPACING // SAMPLES_PER_FRAME

MAJOR_SYNC = (b"\xF8\x72\x6F\xBA", b"\xF8\x72\x6F\xBB")


@dataclass
class AU:
    pos: int
    header: int
    length: int
    timing: int
    directory_bytes: int
    major: bool

    @property
    def logical_pos(self):
        frame, offset = divmod(self.pos, MAT_FRAME)
        return frame * LOGICAL_FRAME + offset


def logical_to_raw(pos):
    frame, offset = divmod(pos, LOGICAL_FRAME)
    if offset >= MAT_FRAME:
        return None
    return frame * MAT_FRAME + offset


def nibble_xor(data, start, count):
    result = 0
    for b in data[start:start + count]:
        result ^= (b >> 4) & 0x0F
        result ^= b & 0x0F
    return result


def parse_au(data, pos, major_required=None):
    if pos < 0 or pos + 12 > len(data):
        return None

    header = (data[pos] << 8) | data[pos + 1]
    length = (header & 0x0FFF) * 2
    timing = (data[pos + 2] << 8) | data[pos + 3]

    if length < 12 or pos + length > len(data):
        return None

    sync = data[pos + 4:pos + 8]
    is_major = sync in MAJOR_SYNC

    if major_required is True and not is_major:
        return None
    if major_required is False and is_major:
        return None

    # Parse the four Substream Directory entries.
    directory_bytes = 0

    for _ in range(SUBSTREAMS):
        entry = pos + 4 + directory_bytes

        if entry + 1 >= pos + length:
            return None

        first_byte = data[entry]
        directory_bytes += 2

        if first_byte & 0x80:
            directory_bytes += 2

        if 4 + directory_bytes > length:
            return None

    # Current experimental Check Nibble rule:
    # AU header + Substream Directory nibble XOR must equal 0xF.
    checksum = nibble_xor(data, pos, 4)
    checksum ^= nibble_xor(data, pos + 4, directory_bytes)

    if checksum != 0x0F:
        return None

    return AU(
        pos=pos,
        header=header,
        length=length,
        timing=timing,
        directory_bytes=directory_bytes,
        major=is_major,
    )



def find_major_aus(data):
    """
    Locate Major Sync markers independently of Minor AU directory parsing.

    Major Sync has its own information structure after the sync word.
    Therefore, do not apply the Minor AU directory parser here.

    This function identifies candidates only. Timing continuity and
    subsequent structural checks are performed separately.
    """
    found = {}

    for sync in MAJOR_SYNC:
        search = 0

        while True:
            sync_pos = data.find(sync, search)

            if sync_pos < 0:
                break

            au_pos = sync_pos - 4

            if au_pos >= 0 and au_pos + 8 <= len(data):
                header = (
                    (data[au_pos] << 8) |
                    data[au_pos + 1]
                )

                length = (header & 0x0FFF) * 2

                timing = (
                    (data[au_pos + 2] << 8) |
                    data[au_pos + 3]
                )

                # Basic AU boundary checks only.
                if (
                    length >= 12
                    and au_pos + length <= len(data)
                ):
                    found[au_pos] = AU(
                        pos=au_pos,
                        header=header,
                        length=length,
                        timing=timing,
                        directory_bytes=0,
                        major=True,
                    )

            search = sync_pos + 1

    return [
        found[p]
        for p in sorted(found)
    ]



def timing_delta(previous, current):
    return (current.timing - previous.timing) & 0xFFFF


def spacing_valid(previous, current):
    delta = timing_delta(previous, current)

    if delta == 0:
        return False

    expected = delta * BYTES_PER_TIMING_TICK
    actual = current.logical_pos - previous.logical_pos

    return actual == expected


def find_next_minor(data, previous, end_pos, max_delta):
    """
    Instead of scanning every byte, predict the next AU position from
    Input Timing. Test each possible timing delta against the checksum.
    """

    for delta in range(1, max_delta + 1):
        target_logical = (
            previous.logical_pos +
            delta * BYTES_PER_TIMING_TICK
        )

        raw_pos = logical_to_raw(target_logical)

        if raw_pos is None or raw_pos >= end_pos:
            continue

        candidate = parse_au(
            data,
            raw_pos,
            major_required=False
        )

        if candidate is None:
            continue

        if timing_delta(previous, candidate) != delta:
            continue

        if candidate.pos + candidate.length > end_pos:
            continue

        return candidate

    return None


def scan_interval(data, start, end, max_delta):
    minors = []
    previous = start

    while True:
        candidate = find_next_minor(
            data,
            previous,
            end.pos,
            max_delta
        )

        if candidate is None:
            break

        minors.append(candidate)
        previous = candidate

    anchor_ok = spacing_valid(previous, end)

    remaining_delta = timing_delta(previous, end)

    warning = None

    if not anchor_ok:
        warning = (
            f"Anchor mismatch: last AU={previous.pos}, "
            f"next Major AU={end.pos}"
        )
    elif remaining_delta > max_delta:
        warning = (
            f"Anchor spacing is valid, but remaining timing delta "
            f"{remaining_delta} exceeds search limit {max_delta}"
        )

    return minors, anchor_ok, warning


def main():
    parser = argparse.ArgumentParser(
        description="Experimental Dolby MAT TrueHD AU scanner"
    )

    parser.add_argument(
        "input",
        nargs="?",
        default=r"D:\sp\captures\ring_truehd.mat"
    )

    parser.add_argument(
        "--csv",
        default="truehd_au_report.csv"
    )

    parser.add_argument(
        "--max-delta",
        type=int,
        default=256
    )

    parser.add_argument(
        "--write-thd",
        action="store_true",
        help="Write experimental concatenated AU output"
    )

    parser.add_argument(
        "--output",
        default="experimental_output.thd"
    )

    args = parser.parse_args()

    input_path = Path(args.input)

    if not input_path.is_file():
        print(f"ERROR: File not found: {input_path}")
        return 2

    if args.max_delta < 1:
        print("ERROR: --max-delta must be positive")
        return 2

    print("Reading MAT file...")
    data = input_path.read_bytes()

    print(f"Input size: {len(data):,} bytes")
    print(f"Complete MAT frames: {len(data) // MAT_FRAME}")
    print()

    majors = find_major_aus(data)

    print(f"Validated Major Sync anchors: {len(majors)}")

    for au in majors:
        print(
            f"MAJOR pos={au.pos:>10} "
            f"logical={au.logical_pos:>10} "
            f"header={au.header:04X} "
            f"length={au.length:>4} "
            f"timing={au.timing:04X}"
        )

    if len(majors) < 2:
        print("ERROR: Not enough valid Major Sync anchors.")
        return 1

    all_aus = []
    warnings = []

    for index in range(len(majors) - 1):
        start = majors[index]
        end = majors[index + 1]

        minors, anchor_ok, warning = scan_interval(
            data,
            start,
            end,
            args.max_delta
        )

        all_aus.append(start)
        all_aus.extend(minors)

        status = "PASS" if anchor_ok and warning is None else "CHECK"

        print(
            f"Interval {index + 1}: "
            f"{start.pos} -> {end.pos} | "
            f"{len(minors)} Minor AU | {status}"
        )

        if warning:
            warnings.append(warning)
            print("  WARNING:", warning)

    all_aus.append(majors[-1])

    # Remove duplicate positions and sort.
    unique = {au.pos: au for au in all_aus}
    all_aus = [unique[p] for p in sorted(unique)]

    minor_count = sum(not au.major for au in all_aus)
    major_count = sum(au.major for au in all_aus)
    total_bytes = sum(au.length for au in all_aus)

    print()
    print("========== SUMMARY ==========")
    print(f"Major AU count: {major_count}")
    print(f"Minor AU count: {minor_count}")
    print(f"Total AU count: {len(all_aus)}")
    print(f"Sum of AU lengths: {total_bytes:,} bytes")
    print(f"Warnings: {len(warnings)}")

    # Write CSV report.
    csv_path = Path(args.csv)

    with csv_path.open(
        "w",
        newline="",
        encoding="utf-8-sig"
    ) as f:
        writer = csv.writer(f)

        writer.writerow([
            "raw_position",
            "logical_position",
            "type",
            "header",
            "length",
            "timing",
            "directory_bytes"
        ])

        for au in all_aus:
            writer.writerow([
                au.pos,
                au.logical_pos,
                "Major" if au.major else "Minor",
                f"{au.header:04X}",
                au.length,
                f"{au.timing:04X}",
                au.directory_bytes
            ])

    print(f"CSV report: {csv_path.resolve()}")

    # Optional experimental extraction.
    if args.write_thd:
        output = bytearray()

        for au in all_aus:
            output.extend(data[au.pos:au.pos + au.length])

        output_path = Path(args.output)
        output_path.write_bytes(output)

        print(f"Experimental THD: {output_path.resolve()}")
        print(f"Output size: {len(output):,} bytes")
        print("WARNING: Output still requires independent decoder validation.")

    print()

    if warnings:
        print("RESULT: CHECK REQUIRED")
        return 1

    print("RESULT: SCAN COMPLETE")
    print("This is not yet a general-purpose TrueHD parser.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
