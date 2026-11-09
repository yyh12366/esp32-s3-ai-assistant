"""TTS interface. Every adapter streams PCM for one sentence."""
from __future__ import annotations

from abc import ABC, abstractmethod
from collections.abc import AsyncIterator


class TTSError(Exception):
    """Raised when synthesis fails."""


class TTS(ABC):
    """Speech synthesis adapter.

    Constructed with the adapter's own options section from config.yaml
    (``tts.<provider>``) as keyword arguments.
    """

    @abstractmethod
    def synthesize(self, text: str, sample_rate: int = 16000) -> AsyncIterator[bytes]:
        """Stream audio for ``text`` (one already-cleaned sentence).

        Implement as an ``async def`` generator. Each yielded chunk is raw
        16-bit little-endian signed mono PCM at ``sample_rate``; chunks may
        be any size (the server re-frames and paces them). Yield the first
        chunk as early as possible: its arrival time is logged as the TTS
        first-packet latency. Raises TTSError on failure.
        """

    async def aclose(self) -> None:
        """Release network clients etc. Called once on shutdown."""
