
import struct
from pathlib import Path
from extract_mat import read_wave

PA = 0xF872
PB = 0x4E1F
PC = 0x0016
PD = 61424
BURST_SIZE = 61440

for filename in [
    r"D:\sp\captures\ring_truehd20s.wav",
    r"D:\sp\captures\ring_truehd30s.wav",
]:
    path = Path(filename)

    if not path.exists():
        print(f"\nFile not found: {path}")
        continue

    print(f"\n========== {path.name} ==========")

    data = read_wave(path)
    print(f"WAV data bytes: {len(data)}")

    valid = []

    # IEC61937 burst 起始位置按 16-bit word 搜索
    for offset in range(0, len(data) - 7, 2):
        pa, pb, pc, pd = struct.unpack_from("<HHHH", data, offset)

        if (pa, pb, pc, pd) == (PA, PB, PC, PD):
            valid.append(offset)

    print(f"Valid IEC61937 headers: {len(valid)}")

    print("First 20 valid offsets:")
    for offset in valid[:20]:
        print(f"  0x{offset:08X} ({offset})")

    if len(valid) >= 2:
        print("First 20 offset differences:")
        for a, b in zip(valid[:20], valid[1:21]):
            print(f"  {b - a}")

    if valid:
        first = valid[0]
        expected = [
            first + i * BURST_SIZE
            for i in range(min(10, (len(data) - first) // BURST_SIZE))
        ]

        print("Sequential burst verification:")
        for offset in expected:
            ok = offset in valid
            print(f"  {offset}: {'OK' if ok else 'MISSING'}")
