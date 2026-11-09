"""ASR interface. Every adapter turns one utterance of PCM into text."""
from __future__ import annotations

from abc import ABC, abstractmethod


class ASRError(Exception):
    """Raised when recognition fails (network, bad response, ...)."""


class ASR(ABC):
    """Speech recognition adapter.

    Constructed with the adapter's own options section from config.yaml
    (``asr.<provider>``) as keyword arguments.
    """

    @abstractmethod
    async def transcribe(self, pcm: bytes, sample_rate: int = 16000) -> str:
        """Recognise one complete utterance.

        ``pcm`` is raw 16-bit little-endian signed mono PCM. Returns the
        recognised text, stripped; empty string if nothing was recognised.
        Raises ASRError on failure.
        """

    async def aclose(self) -> None:
        """Release network clients etc. Called once on shutdown."""
