
#!/usr/bin/env python3
# Experimental Dolby MAT -> TrueHD AU extractor v0.3
#
# Features:
# - Remove MAT Start/Middle/End codes.
# - Global AU candidate scan.
# - AU check-nibble validation.
# - Substream directory parsing.
# - Restart flag validation.
# - Substream parity and CRC8 validation.
# - Non-overlapping AU selection.
# - Output starts at first validated Major Sync.
#
# This version targets the observed 4-substream MAT/FBA capture.
# It is not a general-purpose MAT/TrueHD demuxer.

import argparse
import csv
import sys
from collections import Counter
from pathlib import Path

import numpy as np


MAT_FRAME_SIZE = 61424
MAT_PAYLOAD_SIZE = 61376

MAT_MIDDLE_POS = 30708
MAT_END_POS = 61408

MAT_START_CODE = bytes.fromhex(
    "07 9E 00 03 84 01 01 01 80 00 "
    "56 A5 3B F4 81 83 49 80 77 E0"
)

MAT_MIDDLE_CODE = bytes.fromhex(
    "C3 C1 42 49 3B FA 82 83 49 80 77 E0"
)

MAT_END_CODE = bytes.fromhex(
    "C3 C2 C0 C4 00 00 00 00 "
    "00 00 00 00 00 00 97 11"
)

MAJOR_SYNC = (
    bytes.fromhex("F8 72 6F BA"),
    bytes.fromhex("F8 72 6F BB"),
)


# ------------------------------------------------------------
# TrueHD CRC8
# ------------------------------------------------------------

def update_crc8_byte(crc, value):
    for bit in range(7, -1, -1):
        crc = (crc << 1) | ((value >> bit) & 1)

        if crc & 0x100:
            crc ^= 0x163

    return crc & 0xFF


CRC8_ZERO_TABLE = [
    update_crc8_byte(i, 0)
    for i in range(256)
]

CRC8_BYTE_TABLE = [
    update_crc8_byte(0, i)
    for i in range(256)
]


def crc8_truehd(data):
    crc = 0xA2

    for value in data:
        crc = (
            CRC8_ZERO_TABLE[crc]
            ^ CRC8_BYTE_TABLE[value]
        )

    return crc


def parity_truehd(data):
    value = 0xA9

    for byte in data:
        value ^= byte

    return value


def nibble_xor(data):
    value = 0

    for byte in data:
        value ^= (byte >> 4) ^ (byte & 0x0F)

    return value


# ------------------------------------------------------------
# MAT handling
# ------------------------------------------------------------

def raw_offset_from_dematted(pos):
    frame, within = divmod(pos, MAT_PAYLOAD_SIZE)

    if within < (MAT_MIDDLE_POS - 20):
        raw_within = 20 + within
    else:
        raw_within = 32 + within

    return frame * MAT_FRAME_SIZE + raw_within


def demat_mat(raw):

    if len(raw) % MAT_FRAME_SIZE:
        raise ValueError(
            f"MAT file size {len(raw)} is not a multiple "
            f"of {MAT_FRAME_SIZE}"
        )

    frame_count = len(raw) // MAT_FRAME_SIZE

    output = bytearray(
        frame_count * MAT_PAYLOAD_SIZE
    )

    dst = 0
    bad_markers = []

    for frame_index in range(frame_count):

        base = frame_index * MAT_FRAME_SIZE

        frame = raw[
            base:base + MAT_FRAME_SIZE
        ]

        if frame[0:20] != MAT_START_CODE:
            bad_markers.append(
                (frame_index, "START")
            )

        if frame[
            MAT_MIDDLE_POS:MAT_MIDDLE_POS + 12
        ] != MAT_MIDDLE_CODE:
            bad_markers.append(
                (frame_index, "MIDDLE")
            )

        if frame[
            MAT_END_POS:MAT_END_POS + 16
        ] != MAT_END_CODE:
            bad_markers.append(
                (frame_index, "END")
            )

        # Remove Start Code: 20 bytes.
        n1 = MAT_MIDDLE_POS - 20

        output[dst:dst + n1] = frame[
            20:MAT_MIDDLE_POS
        ]

        dst += n1

        # Remove Middle Code: 12 bytes.
        # Remove End Code: 16 bytes.
        n2 = MAT_END_POS - (
            MAT_MIDDLE_POS + 12
        )

        output[dst:dst + n2] = frame[
            MAT_MIDDLE_POS + 12:MAT_END_POS
        ]

        dst += n2

    if bad_markers:

        preview = "\n".join(
            f"Frame {i}: {kind} marker mismatch"
            for i, kind in bad_markers[:12]
        )

        raise ValueError(
            f"Found {len(bad_markers)} MAT marker errors:\n"
            + preview
        )

    if dst != len(output):
        raise RuntimeError(
            "MAT de-framing size mismatch"
        )

    return bytes(output), frame_count


# ------------------------------------------------------------
# AU parser
# ------------------------------------------------------------

def parse_au(data, pos, minor_substreams=None):

    size = len(data)

    if pos < 0 or (pos & 1):
        return None

    if pos + 8 > size:
        return None

    header = (
        (data[pos] << 8)
        | data[pos + 1]
    )

    au_length = (
        header & 0x0FFF
    ) * 2

    if au_length < 16:
        return None

    if pos + au_length > size:
        return None

    timing = (
        (data[pos + 2] << 8)
        | data[pos + 3]
    )

    is_major = (
        data[pos + 4:pos + 8]
        in MAJOR_SYNC
    )

    # --------------------------------------------------------
    # Major Sync
    # --------------------------------------------------------

    if is_major:

        major = data[
            pos + 4:pos + au_length
        ]

        if len(major) < 28:
            return None

        # Major Sync signature.
        if major[8:10] != b"\xB7\x52":
            return None

        substreams = major[16] >> 4

        if not 1 <= substreams <= 4:
            return None

        major_size = 28

        # extra_channel_meaning_present
        if major[25] & 0x01:

            if len(major) < 27:
                return None

            extra_words = major[26] >> 4

            major_size += (
                2 + extra_words * 2
            )

        if major_size > len(major):
            return None

        directory_pos = (
            pos + 4 + major_size
        )

    # --------------------------------------------------------
    # Minor Sync
    # --------------------------------------------------------

    else:

        if minor_substreams is None:
            return None

        if not 1 <= minor_substreams <= 4:
            return None

        substreams = minor_substreams

        directory_pos = pos + 4

    # --------------------------------------------------------
    # Substream Directory
    # --------------------------------------------------------

    directory_bytes = bytearray()
    entries = []

    cursor = directory_pos

    for _ in range(substreams):

        if cursor + 2 > pos + au_length:
            return None

        word = (
            (data[cursor] << 8)
            | data[cursor + 1]
        )

        directory_bytes.extend(
            data[cursor:cursor + 2]
        )

        cursor += 2

        extra_word = (word >> 15) & 1

        restart_nonexistent = (
            word >> 14
        ) & 1

        crc_present = (
            word >> 13
        ) & 1

        end_ptr_words = (
            word & 0x0FFF
        )

        if extra_word:

            if cursor + 2 > pos + au_length:
                return None

            directory_bytes.extend(
                data[cursor:cursor + 2]
            )

            cursor += 2

        # Major Sync must contain restart headers.
        # Minor Sync must not contain restart headers.
        if is_major and restart_nonexistent != 0:
            return None

        if not is_major and restart_nonexistent != 1:
            return None

        entries.append(
            (
                restart_nonexistent,
                crc_present,
                end_ptr_words
            )
        )

    # --------------------------------------------------------
    # AU check nibble
    # --------------------------------------------------------

    if nibble_xor(
        data[pos:pos + 4] + directory_bytes
    ) != 0x0F:

        return None

    # Minor Sync must not have a Major Sync word
    # immediately following its directory.
    if not is_major:

        if cursor + 2 <= pos + au_length:

            if data[cursor:cursor + 2] == b"\xF8\x72":
                return None

    # --------------------------------------------------------
    # Substream parity and CRC
    # --------------------------------------------------------

    substream_start = cursor

    previous_end = 0
    crc_streams = 0

    for (
        restart_nonexistent,
        crc_present,
        end_ptr_words
    ) in entries:

        end_rel = end_ptr_words * 2

        available = (
            au_length
            - (substream_start - pos)
        )

        if end_rel < previous_end:
            return None

        if end_rel > available:
            return None

        segment_start = (
            substream_start + previous_end
        )

        segment_end = (
            substream_start + end_rel
        )

        if crc_present:

            if segment_end - segment_start < 2:
                return None

            payload_end = segment_end - 2

            payload = data[
                segment_start:payload_end
            ]

            stored_parity = data[payload_end]
            stored_crc = data[payload_end + 1]

            if parity_truehd(payload) != stored_parity:
                return None

            if crc8_truehd(payload) != stored_crc:
                return None

            crc_streams += 1

        previous_end = end_rel

    return {
        "pos": pos,
        "length": au_length,
        "timing": timing,
        "major": is_major,
        "substreams": substreams,
        "directory_pos": directory_pos,
        "substream_start": substream_start,
        "crc_streams": crc_streams,
    }


# ------------------------------------------------------------
# Major Sync discovery
# ------------------------------------------------------------

def find_major_aus(data):

    found = {}

    for sync in MAJOR_SYNC:

        search = 0

        while True:

            sync_pos = data.find(
                sync,
                search
            )

            if sync_pos < 0:
                break

            au_pos = sync_pos - 4

            if au_pos >= 0 and (au_pos & 1) == 0:

                au = parse_au(
                    data,
                    au_pos,
                    None
                )

                if au is not None and au["major"]:
                    found[au_pos] = au

            search = sync_pos + 1

    return [
        found[p]
        for p in sorted(found)
    ]


# ------------------------------------------------------------
# NumPy global scan prefilter
# ------------------------------------------------------------

def vector_prefilter_candidates(
    data,
    chunk_positions=500_000
):

    arr = np.frombuffer(
        data,
        dtype=np.uint8
    )

    total = len(arr)

    found = []

    # Four substreams:
    # 16 possible extra_substream_word combinations.
    layouts = []

    for pattern in range(16):

        cursor = 4

        word_offsets = []
        included_offsets = []

        for i in range(4):

            word_offsets.append(cursor)

            included_offsets.extend(
                (cursor, cursor + 1)
            )

            cursor += 2

            if pattern & (1 << i):

                included_offsets.extend(
                    (cursor, cursor + 1)
                )

                cursor += 2

        layouts.append(
            (
                pattern,
                word_offsets,
                included_offsets,
                cursor
            )
        )

    for chunk_start in range(
        0,
        total - 16,
        chunk_positions * 2
    ):

        chunk_end = min(
            chunk_start + chunk_positions * 2,
            total - 16
        )

        positions = np.arange(
            chunk_start,
            chunk_end,
            2,
            dtype=np.int64
        )

        if positions.size == 0:
            continue

        h0 = arr[positions]
        h1 = arr[positions + 1]

        lengths = (
            (
                (h0 & 0x0F).astype(np.int32) << 8
            )
            | h1.astype(np.int32)
        ) * 2

        base = (
            (lengths >= 16)
            & ((positions + lengths) <= total)
        )

        # Major Syncs are scanned separately.
        base &= ~(
            (arr[positions + 4] == 0xF8)
            & (arr[positions + 5] == 0x72)
            & (arr[positions + 6] == 0x6F)
            & (
                (arr[positions + 7] == 0xBA)
                | (arr[positions + 7] == 0xBB)
            )
        )

        if not np.any(base):
            continue

        for (
            pattern,
            word_offsets,
            included_offsets,
            directory_end
        ) in layouts:

            mask = (
                base
                & (lengths >= directory_end)
            )

            if not np.any(mask):
                continue

            for i, offset in enumerate(word_offsets):

                extra_set = (
                    arr[positions + offset] & 0x80
                ) != 0

                if pattern & (1 << i):
                    mask &= extra_set
                else:
                    mask &= ~extra_set

                if not np.any(mask):
                    break

            if not np.any(mask):
                continue

            candidate_positions = positions[mask]

            checksum = np.zeros(
                candidate_positions.size,
                dtype=np.uint8
            )

            for offset in range(4):

                value = arr[
                    candidate_positions + offset
                ]

                checksum ^= (
                    (value >> 4)
                    ^ (value & 0x0F)
                )

            for offset in included_offsets:

                value = arr[
                    candidate_positions + offset
                ]

                checksum ^= (
                    (value >> 4)
                    ^ (value & 0x0F)
                )

            good = candidate_positions[
                checksum == 0x0F
            ]

            if good.size:
                found.extend(good.tolist())

    return sorted(set(found))


def find_all_candidates(data, substreams):

    candidates = []

    possible_positions = vector_prefilter_candidates(
        data
    )

    print(
        f"Check-nibble prefilter candidates: "
        f"{len(possible_positions)}",
        flush=True
    )

    for index, pos in enumerate(
        possible_positions,
        1
    ):

        au = parse_au(
            data,
            pos,
            substreams
        )

        if au is not None and not au["major"]:
            candidates.append(au)

        if index % 50000 == 0:

            print(
                f"  CRC validation: "
                f"{index:,}/{len(possible_positions):,}",
                flush=True
            )

    return candidates


# ------------------------------------------------------------
# Select non-overlapping AU sequence
# ------------------------------------------------------------

def choose_non_overlapping(candidates):

    candidates.sort(
        key=lambda item: item["pos"]
    )

    selected = []

    end = -1

    for au in candidates:

        if au["pos"] >= end:

            selected.append(au)

            end = (
                au["pos"] + au["length"]
            )

    return selected


# ------------------------------------------------------------
# CSV report
# ------------------------------------------------------------

def write_csv(
    path,
    candidates,
    selected,
    output_positions
):

    selected_positions = {
        a["pos"] for a in selected
    }

    fields = [
        "demat_pos",
        "raw_mat_pos",
        "au_length",
        "timing_hex",
        "major_sync",
        "substreams",
        "crc_streams",
        "nonoverlap_selected",
        "included_in_thd",
    ]

    with open(
        path,
        "w",
        newline="",
        encoding="utf-8-sig"
    ) as f:

        writer = csv.DictWriter(
            f,
            fieldnames=fields
        )

        writer.writeheader()

        for au in candidates:

            pos = au["pos"]

            writer.writerow({

                "demat_pos": pos,

                "raw_mat_pos":
                    raw_offset_from_dematted(pos),

                "au_length": au["length"],

                "timing_hex":
                    f"{au['timing']:04X}",

                "major_sync":
                    int(au["major"]),

                "substreams":
                    au["substreams"],

                "crc_streams":
                    au["crc_streams"],

                "nonoverlap_selected":
                    int(pos in selected_positions),

                "included_in_thd":
                    int(pos in output_positions),
            })


# ------------------------------------------------------------
# Main
# ------------------------------------------------------------

def main():

    parser = argparse.ArgumentParser(
        description="Experimental Dolby MAT TrueHD AU extractor v0.3"
    )

    parser.add_argument(
        "input",
        nargs="?",
        help="Input raw MAT payload file"
    )

    parser.add_argument(
        "--csv",
        help="Write AU candidate report CSV"
    )

    parser.add_argument(
        "--write-thd",
        action="store_true",
        help="Write concatenated AU output"
    )

    parser.add_argument(
        "--output",
        help="Output THD path"
    )

    args = parser.parse_args()

    if not args.input:
        parser.error("input MAT file is required")

    if args.write_thd and not args.output:
        parser.error("--write-thd requires --output")

    input_path = Path(args.input)

    print("Reading MAT file...", flush=True)

    raw = input_path.read_bytes()

    print(
        f"Input size: {len(raw):,} bytes",
        flush=True
    )

    try:

        dematted, frame_count = demat_mat(raw)

    except Exception as exc:

        print(
            f"ERROR: MAT de-framing failed: {exc}",
            file=sys.stderr
        )

        return 2

    print(
        f"Complete MAT frames: {frame_count}",
        flush=True
    )

    print(
        f"MAT framing bytes removed: "
        f"{frame_count * 48:,}",
        flush=True
    )

    print(
        f"De-MAT stream size: "
        f"{len(dematted):,} bytes",
        flush=True
    )

    print(
        "Locating and validating Major Sync AUs...",
        flush=True
    )

    majors = find_major_aus(dematted)

    if not majors:

        print(
            "ERROR: no validated Major Sync AU found",
            file=sys.stderr
        )

        return 3

    counts = Counter(
        a["substreams"] for a in majors
    )

    substreams, count = counts.most_common(1)[0]

    if len(counts) > 1:

        print(
            "WARNING: Major Sync candidates disagree "
            f"on substream count: {dict(counts)}"
        )

    if substreams != 4:

        print(
            "ERROR: this v0.3 scanner currently "
            "supports the observed 4-substream layout; "
            f"detected {substreams}.",
            file=sys.stderr
        )

        return 8

    print(
        f"Validated Major Sync anchors: {len(majors)}",
        flush=True
    )

    print(
        f"Detected substreams: {substreams} "
        f"(from {count}/{len(majors)} Major Syncs)",
        flush=True
    )

    print(
        "Scanning all even-byte positions...",
        flush=True
    )

    candidates = find_all_candidates(
        dematted,
        substreams
    )

    candidates.extend(majors)

    candidates.sort(
        key=lambda item: item["pos"]
    )

    print(
        f"Fully validated AU candidates: "
        f"{len(candidates)}",
        flush=True
    )

    selected = choose_non_overlapping(
        candidates
    )

    if not selected:

        print(
            "ERROR: no non-overlapping AU sequence found",
            file=sys.stderr
        )

        return 4

    first_major_index = next(
        (
            i
            for i, a in enumerate(selected)
            if a["major"]
        ),
        None
    )

    if first_major_index is None:

        print(
            "ERROR: no Major Sync in selected sequence",
            file=sys.stderr
        )

        return 5

    before_major = selected[
        :first_major_index
    ]

    output_aus = selected[
        first_major_index:
    ]

    if not output_aus or not output_aus[0]["major"]:

        print(
            "ERROR: output does not begin with Major Sync",
            file=sys.stderr
        )

        return 6

    major_count = sum(
        1 for a in output_aus if a["major"]
    )

    minor_count = (
        len(output_aus) - major_count
    )

    output_size = sum(
        a["length"] for a in output_aus
    )

    output_positions = {
        a["pos"] for a in output_aus
    }

    print("\n========== SUMMARY ==========")

    print(
        f"Major AU count: {major_count}"
    )

    print(
        f"Minor AU count: {minor_count}"
    )

    print(
        f"Total AU count: {len(output_aus)}"
    )

    print(
        f"Valid pre-Major candidates skipped: "
        f"{len(before_major)}"
    )

    print(
        f"Overlapping valid candidates not selected: "
        f"{len(candidates) - len(selected)}"
    )

    print(
        f"Sum of AU lengths: {output_size:,} bytes"
    )

    print(
        f"First Major Sync de-MAT offset: "
        f"{output_aus[0]['pos']:,}"
    )

    print(
        f"First Major Sync raw MAT offset: "
        f"{raw_offset_from_dematted(output_aus[0]['pos']):,}"
    )

    if args.csv:

        write_csv(
            args.csv,
            candidates,
            selected,
            output_positions
        )

        print(
            f"CSV report: {args.csv}"
        )

    if args.write_thd:

        output_path = Path(args.output)

        with output_path.open("wb") as f:

            for au in output_aus:

                p = au["pos"]

                f.write(
                    dematted[
                        p:p + au["length"]
                    ]
                )

        actual_size = output_path.stat().st_size

        if actual_size != output_size:

            print(
                "ERROR: output size mismatch",
                file=sys.stderr
            )

            return 7

        print(
            f"Experimental THD: {output_path}"
        )

        print(
            f"Output size: {actual_size:,} bytes"
        )

    print("\nRESULT: SCAN COMPLETE")

    print(
        "Experimental parser; verify output with FFmpeg."
    )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
