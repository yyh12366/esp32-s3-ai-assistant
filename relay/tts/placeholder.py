"""Placeholder TTS: synthetic beeps whose length tracks the text length."""
from __future__ import annotations

import asyncio
import math
import sys
from array import array
from collections.abc import AsyncIterator

from .base import TTS

_MIN_SECONDS = 0.3
_FADE_SECONDS = 0.005
_TONE_FRACTION = 0.6  # share of each character slot occupied by the beep


class PlaceholderTTS(TTS):
    """Stand-in TTS: one pitched beep per character, generated lazily.

    Output is obviously synthetic, but its duration follows the text so the
    pipeline's pacing can be exercised end to end. Does not sleep in real
    time; the server paces playback.
    """

    def __init__(
        self,
        chars_per_second: float = 6.0,
        tone_hz: float = 440.0,
        first_packet_delay_ms: int = 30,
        chunk_ms: int = 100,
        volume: float = 0.3,
    ) -> None:
        if chars_per_second <= 0:
            raise ValueError("chars_per_second must be positive")
        self._cps = float(chars_per_second)
        self._tone_hz = float(tone_hz)
        self._first_delay_s = first_packet_delay_ms / 1000.0
        self._chunk_ms = chunk_ms
        self._amplitude = max(0.0, min(1.0, volume)) * 32767

    async def synthesize(self, text: str, sample_rate: int = 16000) -> AsyncIterator[bytes]:
        text = text.strip()
        if not text:
            return

        await asyncio.sleep(self._first_delay_s)  # simulated latency

        slot = sample_rate / self._cps  # samples per character slot
        tone_len = slot * _TONE_FRACTION
        fade = _FADE_SECONDS * sample_rate
        freqs = [self._tone_hz * (1 + (ord(ch) % 5) * 0.1) for ch in text]
        total = max(_MIN_SECONDS, len(text) / self._cps)
        total_n = round(total * sample_rate)
        chunk_n = max(1, int(sample_rate * self._chunk_ms / 1000))
        amp = self._amplitude
        two_pi_over_sr = 2 * math.pi / sample_rate

        for start in range(0, total_n, chunk_n):
            end = min(total_n, start + chunk_n)
            samples = array("h", bytes(2 * (end - start)))  # silence
            for i in range(end - start):
                n = start + i
                idx = int(n // slot)
                if idx >= len(text):
                    break  # tail after the text is silence
                p = n - idx * slot  # position inside this character's slot
                if p >= tone_len:
                    continue
                env = min(1.0, p / fade, (tone_len - p) / fade)
                value = math.sin(two_pi_over_sr * freqs[idx] * p)
                samples[i] = int(amp * env * value)
            if sys.byteorder == "big":
                samples.byteswap()
            yield samples.tobytes()
            await asyncio.sleep(0)
