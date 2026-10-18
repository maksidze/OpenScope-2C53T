#!/usr/bin/env python3
"""Dependency-free golden checks for the draft OSLP/1 framing profile."""

import random
import struct


def crc32c(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for value in data:
        crc ^= value
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 & -(crc & 1))
    return (~crc) & 0xFFFFFFFF


def cobs_encode(data: bytes) -> bytes:
    output = bytearray(b"\x00")
    code_index = 0
    code = 1
    for value in data:
        if value == 0:
            output[code_index] = code
            code_index = len(output)
            output.append(0)
            code = 1
        else:
            output.append(value)
            code += 1
            if code == 0xFF:
                output[code_index] = code
                code_index = len(output)
                output.append(0)
                code = 1
    output[code_index] = code
    return bytes(output)


def cobs_decode(data: bytes) -> bytes:
    output = bytearray()
    offset = 0
    while offset < len(data):
        code = data[offset]
        assert code and offset + code <= len(data) + 1
        offset += 1
        output += data[offset : offset + code - 1]
        offset += code - 1
        if code != 0xFF and offset < len(data):
            output.append(0)
    return bytes(output)


def make_frame(payload: bytes) -> bytes:
    # HELLO request, session=0, transaction=1, sequence=1.
    header = struct.pack("<2sBBBBBBHHHH", b"OS", 1, 0, 1, 2, 0, 1, 0, 1, 1, len(payload))
    decoded = header + payload
    return cobs_encode(decoded + struct.pack("<I", crc32c(decoded))) + b"\x00"


def main() -> None:
    assert crc32c(b"123456789") == 0xE3069283
    vectors = [b"", b"\x00", b"abc", bytes(range(256)), b"\x00" * 1024]
    random.seed(0x2C53)
    vectors += [bytes(random.randrange(256) for _ in range(size)) for size in range(1, 1025, 17)]
    for vector in vectors:
        assert cobs_decode(cobs_encode(vector)) == vector

    payload = bytes([0, 1, 0, 0, 4, 0x78, 0x56, 0x34, 0x12, 6, 1, 4])
    frame = make_frame(payload)
    assert frame[-1] == 0
    decoded = cobs_decode(frame[:-1])
    assert decoded[:2] == b"OS"
    assert len(decoded) == 16 + len(payload) + 4
    assert crc32c(decoded[:-4]) == struct.unpack("<I", decoded[-4:])[0]
    print(f"protocol self-test passed; HELLO golden frame: {frame.hex()}")


if __name__ == "__main__":
    main()
