"""Independent IMA ADPCM blocks for the negotiated device audio downlink.

Each message has a little-endian <hBBH header (first sample, initial step
index, reserved zero, sample count), then high-nibble-first ADPCM codes.
"""
from __future__ import annotations

import struct

STEPS = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130,
    143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
    494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
    4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
    10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385,
    24623, 27086, 29794, 32767,
)
INDEX = (-1, -1, -1, -1, 2, 4, 6, 8)


def encode_block(pcm: bytes) -> bytes:
    """Encode up to 640 mono int16 samples (40 ms at 16 kHz)."""
    if not pcm or len(pcm) % 2 or len(pcm) > 1280:
        raise ValueError("ADPCM blocks require 1..640 complete int16 samples")
    samples = struct.unpack(f"<{len(pcm) // 2}h", pcm)
    predictor = samples[0]
    initial_step = abs(samples[1] - predictor) if len(samples) > 1 else 0
    index = min(range(len(STEPS)), key=lambda i: abs(STEPS[i] - initial_step))
    block = bytearray(struct.pack("<hBBH", predictor, index, 0, len(samples)))
    for position, sample in enumerate(samples[1:]):
        step = STEPS[index]
        difference = sample - predictor
        code = 8 if difference < 0 else 0
        difference = abs(difference)
        delta = step >> 3
        for bit in (4, 2, 1):
            if difference >= step:
                code |= bit
                difference -= step
                delta += step
            step >>= 1
        predictor += -delta if code & 8 else delta
        predictor = max(-32768, min(32767, predictor))
        index = max(0, min(88, index + INDEX[code & 7]))
        if position % 2 == 0:
            block.append(code << 4)
        else:
            block[-1] |= code
    return bytes(block)
