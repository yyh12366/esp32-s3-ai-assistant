"""Small PCM helpers (16-bit little-endian mono), stdlib only."""
from __future__ import annotations

import sys
from array import array


class Resampler:
    """Streaming sample-rate converter for int16 mono PCM.

    Linear interpolation; when downsampling, a [1, 2, 1]/4 pre-filter damps the
    band that would alias (good enough for speech on a small speaker). State is
    kept across ``process`` calls, so chunk boundaries don't click.
    """

    def __init__(self, src_rate: int, dst_rate: int):
        self.src_rate = src_rate
        self.dst_rate = dst_rate
        self._step = src_rate / dst_rate
        self._lowpass = src_rate > dst_rate
        self._buf = array("h")
        self._pos = 0.0  # read position in _buf, in source samples
        self._odd = b""  # dangling byte from the previous chunk
        self._hist = (0, 0)  # last two raw input samples, for the pre-filter

    def process(self, data: bytes) -> bytes:
        data = self._odd + data
        cut = len(data) - (len(data) % 2)
        data, self._odd = data[:cut], data[cut:]
        if self.src_rate == self.dst_rate or not data:
            return data

        x = array("h")
        x.frombytes(data)
        if sys.byteorder == "big":
            x.byteswap()

        if self._lowpass:
            a, b = self._hist
            filtered = array("h", bytes(2 * len(x)))
            for i, c in enumerate(x):
                filtered[i] = (a + 2 * b + c) >> 2
                a, b = b, c
            self._hist = (a, b)
            x = filtered

        buf = self._buf
        buf.extend(x)
        out = array("h")
        pos, step, n = self._pos, self._step, len(buf)
        while pos + 1 < n:
            i = int(pos)
            s0 = buf[i]
            out.append(int(s0 + (buf[i + 1] - s0) * (pos - i)))
            pos += step
        drop = min(int(pos), n)
        del buf[:drop]
        self._pos = pos - drop

        if sys.byteorder == "big":
            out.byteswap()
        return out.tobytes()
