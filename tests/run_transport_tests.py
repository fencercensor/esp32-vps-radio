#!/usr/bin/env python3
"""Cross-check the PHP encoder, Python zlib and the shared C++ decoder."""
from pathlib import Path
import random
import struct
import subprocess
import tempfile
import zlib

root = Path(__file__).resolve().parents[1]
rng = random.Random(12345)
with tempfile.TemporaryDirectory(prefix='radio-transport-') as directory:
    temp = Path(directory)
    binary = temp / 'transport-test'
    subprocess.run(['c++', '-std=c++17', '-O2', str(root / 'tests/transport_test.cpp'),
                    '-lz', '-o', str(binary)], check=True)
    vectors = [bytes(1024 * n) for n in range(1, 9)]
    vectors += [bytes(range(256)) * 36, bytes(rng.getrandbits(8) for _ in range(8192)),
                bytes(rng.getrandbits(8) for _ in range(1024)) + bytes(8192)]
    kinds = set()
    counts = set()
    for index, source in enumerate(vectors):
        raw = temp / f'{index}.raw'
        wire_path = temp / f'{index}.rz1'
        raw.write_bytes(source)
        subprocess.run(['php', str(root / 'tests/lossless_encode.php'), str(raw), str(wire_path)], check=True)
        wire = wire_path.read_bytes()
        offset = 0
        decoded = bytearray()
        while offset < len(wire):
            magic, kind, count, raw_size, wire_size = struct.unpack_from('<2sBBHH', wire, offset)
            assert magic == b'RZ' and kind in (0, 1) and 1 <= count <= 8
            assert raw_size == count * 1024 and 0 < wire_size <= raw_size
            offset += 8
            payload = wire[offset:offset + wire_size]
            assert len(payload) == wire_size
            block = zlib.decompress(payload) if kind else payload
            assert len(block) == raw_size
            decoded.extend(block)
            kinds.add(kind)
            counts.add(count)
            offset += wire_size
        assert decoded == source
        subprocess.run([str(binary), str(wire_path), str(raw)], check=True, stdout=subprocess.DEVNULL)
    assert kinds == {0, 1} and counts == set(range(1, 9))
    print(f'Passed {len(vectors)} PHP/Python/C++ vectors; raw fallback, all group sizes and malformed-frame rejection verified.')
