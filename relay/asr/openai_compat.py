"""ASR adapter for any OpenAI-compatible /v1/audio/transcriptions endpoint."""
from __future__ import annotations

import io
import wave

import httpx
from opencc import OpenCC

from .base import ASR, ASRError

_MIN_SECONDS = 0.1
_MAX_ERROR_BODY = 200


class OpenAICompatASR(ASR):
    """Transcribe one utterance per request via a Whisper-style multipart API."""

    def __init__(
        self,
        base_url: str = "http://127.0.0.1:8000/v1",
        api_key: str = "",
        model: str = "whisper-1",
        language: str = "zh",
        timeout: float = 30.0,
        prompt: str = "",
        proxy: str | None = None,
        simplified_chinese: bool = False,
    ) -> None:
        self._url = f"{base_url.rstrip('/')}/audio/transcriptions"
        self._model = model
        self._language = language
        self._prompt = prompt
        self._simplifier = OpenCC("t2s") if simplified_chinese else None
        headers = {"Authorization": f"Bearer {api_key}"} if api_key else {}
        self._client = httpx.AsyncClient(timeout=timeout, headers=headers, proxy=proxy)

    async def transcribe(self, pcm: bytes, sample_rate: int = 16000) -> str:
        """Send PCM (16-bit LE mono) as WAV and return the recognised text."""
        pcm = pcm[: len(pcm) // 2 * 2]  # drop a dangling odd byte
        if len(pcm) / 2 / sample_rate < _MIN_SECONDS:
            return ""

        buf = io.BytesIO()
        with wave.open(buf, "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(sample_rate)
            wav.writeframes(pcm)

        data = {"model": self._model, "response_format": "json"}
        if self._language:
            data["language"] = self._language
        if self._prompt:
            data["prompt"] = self._prompt
        files = {"file": ("audio.wav", buf.getvalue(), "audio/wav")}

        try:
            resp = await self._client.post(self._url, data=data, files=files)
        except httpx.HTTPError as exc:
            raise ASRError(f"ASR request failed: {exc}") from exc

        if not resp.is_success:
            body = resp.text[:_MAX_ERROR_BODY]
            raise ASRError(f"ASR HTTP {resp.status_code}: {body}")

        if resp.headers.get("content-type", "").startswith("text/plain"):
            text = resp.text.strip()
        else:
            try:
                text = resp.json()["text"].strip()
            except (ValueError, KeyError, TypeError, AttributeError) as exc:
                raise ASRError(f"ASR returned unparseable response: {exc}") from exc
        return self._simplifier.convert(text) if self._simplifier else text

    async def aclose(self) -> None:
        await self._client.aclose()
