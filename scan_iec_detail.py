
import struct
from pathlib import Path
from collections import Counter
from extract_mat import read_wave

PA = 0xF872
PB = 0x4E1F
PC = 0x0016
PD = 61424
BURST_SIZE = 61440

FILES = [
    r"D:\sp\captures\ring_truehd20s.wav",
    r"D:\sp\captures\ring_truehd30s.wav",
]

for filename in FILES:
    path = Path(filename)

    if not path.exists():
        print(f"\nFile not found: {path}")
        continue

    print(f"\n{'=' * 60}")
    print(path.name)
    print("=" * 60)

    data = read_wave(path)
    print(f"Data size: {len(data)}")

    pa_pb = []
    valid = []

    # 搜索 Pa/Pb，暂时不要求 Pc/Pd 正确
    for offset in range(0, len(data) - 7, 2):
        a, b, c, d = struct.unpack_from("<HHHH", data, offset)

        if a == PA and b == PB:
            pa_pb.append((offset, c, d))

            if c == PC and d == PD:
                valid.append(offset)

    print(f"\nPa/Pb candidates: {len(pa_pb)}")
    print(f"Fully valid headers: {len(valid)}")

    print("\nFirst 30 Pa/Pb candidates:")
    for offset, pc, pd in pa_pb[:30]:
        print(
            f"offset={offset:10d} "
            f"0x{offset:08X} "
            f"Pc=0x{pc:04X} "
            f"Pd={pd}"
        )

    print("\nPc distribution:")
    print(Counter(f"0x{pc:04X}" for _, pc, _ in pa_pb).most_common(20))

    print("\nPd distribution:")
    print(Counter(pd for _, _, pd in pa_pb).most_common(20))

    if valid:
        print("\nFirst 10 valid offsets:")
        print(valid[:10])

        print("\nLast 10 valid offsets:")
        print(valid[-10:])

        gaps = [
            (a, b, b - a)
            for a, b in zip(valid, valid[1:])
            if b - a != BURST_SIZE
        ]

        print(f"\nNon-standard gaps: {len(gaps)}")
        for item in gaps[:30]:
            print(item)
