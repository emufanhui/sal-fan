
from pathlib import Path
from extract_mat import read_wave

path = Path(r"D:\sp\captures\ring_truehd20s.wav")
data = read_wave(path)

print("First 128 bytes:")
print(data[:128].hex(" "))

print("\nData at 2048-byte intervals:")

for offset in [0, 2048, 4096, 6144, 8192, 10240]:
    print(f"\nOffset {offset}:")
    print(data[offset:offset + 64].hex(" "))

print("\nNon-zero bytes in first 10 blocks:")

for i in range(10):
    block = data[i * 2048:(i + 1) * 2048]
    count = sum(b != 0 for b in block)
    print(f"Block {i}: {count}/2048")

print("\nFirst 32 bytes at 30s valid burst:")

path30 = Path(r"D:\sp\captures\ring_truehd30s.wav")
data30 = read_wave(path30)

print(data30[30720:30752].hex(" "))
