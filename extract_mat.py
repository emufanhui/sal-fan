import argparse
import struct
import uuid
import sys
from pathlib import Path

# Dolby MLP / MAT IEC61937 SubFormat GUID
EXPECTED_GUID = uuid.UUID("0000000c-0cea-0010-8000-00aa00389b71")

BURST_SIZE = 61440
MAT_SIZE = 61424

PA = 0xF872
PB = 0x4E1F
PC = 0x0016

MAT_START = bytes.fromhex(
    "07 9e 00 03 84 01 01 01 80 00 56 a5 3b f4 81 83 49 80 77 e0"
)
MAT_MIDDLE = bytes.fromhex(
    "c3 c1 42 49 3b fa 82 83 49 80 77 e0"
)
MAT_END = bytes.fromhex(
    "c3 c2 c0 c4 00 00 00 00 00 00 00 00 00 00 97 11"
)

MIDDLE_OFFSET = 30708
END_OFFSET = MAT_SIZE - len(MAT_END)


def read_wave(path):
    raw = path.read_bytes()

    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise ValueError("不是有效的 RIFF/WAVE 文件")

    pos = 12
    fmt = None
    data = None

    while pos + 8 <= len(raw):
        chunk_id = raw[pos:pos + 4]
        size = struct.unpack_from("<I", raw, pos + 4)[0]
        start = pos + 8
        end = start + size

        if end > len(raw):
            raise ValueError("WAV chunk 超出文件范围")

        if chunk_id == b"fmt ":
            fmt = raw[start:end]
        elif chunk_id == b"data":
            data = raw[start:end]

        pos = end + (size & 1)

    if fmt is None or data is None:
        raise ValueError("缺少 fmt 或 data chunk")

    if len(fmt) < 40:
        raise ValueError("fmt chunk 不包含完整的 WAVEFORMATEXTENSIBLE")

    tag, channels, rate, avg, align, bits = struct.unpack_from(
        "<HHIIHH", fmt, 0
    )

    if tag != 0xFFFE:
        raise ValueError("不是 WAVE_FORMAT_EXTENSIBLE")

    channel_mask = struct.unpack_from("<I", fmt, 20)[0]
    subformat = uuid.UUID(bytes_le=fmt[24:40])

    print(f"Channels: {channels}")
    print(f"Sample rate: {rate}")
    print(f"Bits: {bits}")
    print(f"Block align: {align}")
    print(f"Channel mask: 0x{channel_mask:X}")
    print(f"SubFormat: {subformat}")

    if subformat != EXPECTED_GUID:
        raise ValueError("不是 Dolby MLP/MAT IEC61937 SubFormat")

    if (channels, rate, bits, align) != (8, 192000, 16, 16):
        raise ValueError("载波格式不符合预期的 8ch/192kHz/16bit")

    return data


def swap_words(data):
    if len(data) % 2:
        raise ValueError("MAT 数据长度必须为偶数")

    result = bytearray(len(data))
    result[0::2] = data[1::2]
    result[1::2] = data[0::2]
    return bytes(result)


def extract(input_path, output_path):
    data = read_wave(input_path)

    if len(data) % BURST_SIZE:
        raise ValueError(
            f"data 长度 {len(data)} 不是 {BURST_SIZE} 的整数倍"
        )

    count = len(data) // BURST_SIZE
    print(f"IEC61937 bursts: {count}")

    output = bytearray()

    for i in range(count):
        offset = i * BURST_SIZE
        burst = data[offset:offset + BURST_SIZE]

        pa, pb, pc, pd = struct.unpack_from("<HHHH", burst, 0)

        if (pa, pb) != (PA, PB):
            raise ValueError(f"Burst {i}: IEC preamble 错误")

        if pc != PC:
            raise ValueError(
                f"Burst {i}: Pc 错误，实际为 0x{pc:04X}"
            )

        if pd != MAT_SIZE:
            raise ValueError(
                f"Burst {i}: Pd 错误，实际为 {pd}"
            )

        # 去掉 8 字节 IEC61937 header
        payload = burst[8:8 + MAT_SIZE]

        # 对完整 MAT payload 的每个 16-bit word 交换字节
        mat = swap_words(payload)

        # 验证 MAT 三个固定标记
        if mat[:20] != MAT_START:
            raise ValueError(f"Burst {i}: MAT Start marker 错误")

        if mat[MIDDLE_OFFSET:MIDDLE_OFFSET + 12] != MAT_MIDDLE:
            raise ValueError(f"Burst {i}: MAT Middle marker 错误")

        if mat[END_OFFSET:END_OFFSET + len(MAT_END)] != MAT_END:
            raise ValueError(f"Burst {i}: MAT End marker 错误")

        output.extend(mat)

        if (i + 1) % 50 == 0:
            print(f"Validated {i + 1}/{count} frames")

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(output)

    print("\nExtraction successful")
    print(f"Frames: {count}")
    print(f"Output bytes: {len(output)}")
    print(f"Output file: {output_path}")
    print("All MAT markers validated.")
    print("注意：输出是 MAT 帧流，不是已经提取的 TrueHD elementary stream。")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("input", help="输入 WAV 文件")
    parser.add_argument("output", nargs="?", help="输出 MAT 文件")
    args = parser.parse_args()

    source = Path(args.input)
    destination = Path(args.output) if args.output else source.with_suffix(".mat")

    try:
        extract(source, destination)
    except Exception as e:
        print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(1)