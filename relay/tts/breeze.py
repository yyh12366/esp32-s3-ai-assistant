"""Breeze Blue TTS adapter (streaming PCM).

POST {api_base}/v1/text-to-speech/{voice_id}/stream?output_format=pcm
Header ``xi-api-key``. The response body is raw 24 kHz / 16-bit / mono PCM,
streamed as it is generated; we resample it to the device rate on the fly.
"""
from __future__ import annotations

import asyncio
import logging
import os
from collections.abc import AsyncIterator
from urllib.parse import quote

import httpx

from ..audio import Resampler
from .base import TTS, TTSError

log = logging.getLogger(__name__)

BREEZE_PCM_RATE = 24000
_RETRY_STATUSES = {429, 503, 504}


class BreezeTTS(TTS):
    def __init__(
        self,
        api_key: str = "",
        api_base: str = "https://api.breeze.blue",
        voice_id: str = "",
        model_id: str = "",
        language_code: str = "zh",
        instructions: str = "",
        speed: float | None = None,
        volume: float | None = None,
        timeout: float = 30.0,
        max_retry_wait: float = 3.0,
    ) -> None:
        api_key = api_key or os.environ.get("BREEZE_API_KEY", "")
        if not api_key:
            raise ValueError("tts.breeze.api_key is empty (or set BREEZE_API_KEY)")
        if not voice_id:
            raise ValueError("tts.breeze.voice_id is empty")
        self._url = f"{api_base.rstrip('/')}/v1/text-to-speech/{quote(voice_id, safe='')}/stream"
        self._model_id = model_id
        self._language_code = language_code
        self._instructions = instructions
        self._voice_settings = {
            k: v for k, v in (("speed", speed), ("volume", volume)) if v is not None
        }
        self._max_retry_wait = max_retry_wait
        self._client = httpx.AsyncClient(
            timeout=httpx.Timeout(timeout, connect=10.0),
            headers={"xi-api-key": api_key},
        )

    def _payload(self, text: str) -> dict:
        body: dict = {"text": text}
        if self._model_id:
            body["model_id"] = self._model_id
        if self._language_code:
            body["language_code"] = self._language_code
        if self._instructions:
            body["instructions"] = self._instructions
        if self._voice_settings:
            body["voice_settings"] = self._voice_settings
        return body

    async def synthesize(self, text: str, sample_rate: int = 16000) -> AsyncIterator[bytes]:
        text = text.strip()
        if not text:
            return
        for attempt in (1, 2):
            resampler = Resampler(BREEZE_PCM_RATE, sample_rate)
            try:
                async with self._client.stream(
                    "POST",
                    self._url,
                    params={"output_format": "pcm"},
                    json=self._payload(text),
                    headers={"accept": "audio/pcm"},
                ) as resp:
                    if resp.status_code in _RETRY_STATUSES and attempt == 1:
                        wait = _retry_after(resp.headers.get("retry-after"))
                        if wait <= self._max_retry_wait:
                            log.warning("Breeze HTTP %d, retrying in %.1fs", resp.status_code, wait)
                            await asyncio.sleep(wait)
                            continue
                    if not resp.is_success:
                        body = (await resp.aread()).decode("utf-8", "replace")[:200]
                        raise TTSError(f"Breeze HTTP {resp.status_code}: {body}")
                    async for chunk in resp.aiter_bytes():
                        out = resampler.process(chunk)
                        if out:
                            yield out
                    return
            except httpx.HTTPError as exc:
                raise TTSError(f"Breeze request failed: {exc}") from exc

    async def aclose(self) -> None:
        await self._client.aclose()


def _retry_after(value: str | None) -> float:
    try:
        return max(0.0, float(value)) if value else 1.0
    except ValueError:
        return 1.0
