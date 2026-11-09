"""Tests for the ASR / TTS adapters and registry wiring."""
from __future__ import annotations

import array
import io
import math
import time
import wave

import httpx
import pytest

from relay.asr.base import ASRError
from relay.asr.openai_compat import OpenAICompatASR
from relay.config import ProviderConfig
from relay.registry import create_asr, create_tts
from relay.tts.placeholder import PlaceholderTTS

SR = 16000


def _pcm(seconds: float, sr: int = SR) -> bytes:
    """Non-silent 16-bit LE mono PCM of the given length."""
    n = int(seconds * sr)
    samples = array.array("h", (int(8000 * math.sin(i * 0.1)) for i in range(n)))
    return samples.tobytes()


def _asr_with(handler, **kwargs) -> OpenAICompatASR:
    asr = OpenAICompatASR(**kwargs)
    asr._client = httpx.AsyncClient(transport=httpx.MockTransport(handler))
    return asr


def _extract_wav(body: bytes) -> bytes:
    return body[body.index(b"RIFF"):]


# --- OpenAICompatASR -------------------------------------------------------

async def test_asr_posts_multipart_wav_and_returns_stripped_text():
    seen = {}

    def handler(request: httpx.Request) -> httpx.Response:
        seen["url"] = str(request.url)
        seen["body"] = request.read()
        seen["ctype"] = request.headers["content-type"]
        return httpx.Response(200, json={"text": "  你好世界  "})

    prompt = "请使用简体中文转写。"
    asr = _asr_with(handler, base_url="http://asr.local:8000/v1/", model="sense-voice",
                    language="zh", prompt=prompt)
    text = await asr.transcribe(_pcm(0.5), sample_rate=SR)
    await asr.aclose()

    assert text == "你好世界"
    assert seen["url"] == "http://asr.local:8000/v1/audio/transcriptions"
    assert seen["ctype"].startswith("multipart/form-data")
    body = seen["body"]
    assert b'name="model"' in body and b"sense-voice" in body
    assert b'name="language"' in body and b"zh" in body
    assert b'name="prompt"' in body and prompt.encode("utf-8") in body
    assert b'name="response_format"' in body
    with wave.open(io.BytesIO(_extract_wav(body))) as wav:
        assert wav.getframerate() == SR
        assert wav.getnchannels() == 1
        assert wav.getsampwidth() == 2
        assert wav.getnframes() == SR // 2


async def test_asr_accepts_plain_text_response():
    asr = _asr_with(lambda r: httpx.Response(200, text=" hello \n",
                                             headers={"content-type": "text/plain"}))
    assert await asr.transcribe(_pcm(0.2)) == "hello"
    await asr.aclose()


@pytest.mark.parametrize("plain_text", [False, True])
@pytest.mark.parametrize("simplified_chinese", [False, True])
async def test_asr_simplifies_traditional_chinese_when_enabled(plain_text, simplified_chinese):
    original = "肥魚肥魚大肥魚，臺灣天氣很好。ESP32 OK 123"

    def handler(request):
        if plain_text:
            return httpx.Response(200, text=f"  {original}\n",
                                  headers={"content-type": "text/plain"})
        return httpx.Response(200, json={"text": f"  {original}\n"})

    asr = _asr_with(handler, simplified_chinese=simplified_chinese)
    try:
        text = await asr.transcribe(_pcm(0.2))
    finally:
        await asr.aclose()

    expected = "肥鱼肥鱼大肥鱼，台湾天气很好。ESP32 OK 123" if simplified_chinese else original
    assert text == expected


async def test_asr_error_status_raises_with_status_and_truncated_body():
    asr = _asr_with(lambda r: httpx.Response(500, text="x" * 500))
    with pytest.raises(ASRError) as exc:
        await asr.transcribe(_pcm(0.5))
    assert "500" in str(exc.value)
    assert "x" * 201 not in str(exc.value)
    await asr.aclose()


async def test_asr_bad_json_raises_asr_error():
    asr = _asr_with(lambda r: httpx.Response(200, json={"oops": 1}))
    with pytest.raises(ASRError):
        await asr.transcribe(_pcm(0.5))
    await asr.aclose()


async def test_asr_transport_error_raises_asr_error():
    def handler(request):
        raise httpx.ConnectError("refused", request=request)

    asr = _asr_with(handler)
    with pytest.raises(ASRError):
        await asr.transcribe(_pcm(0.5))
    await asr.aclose()


async def test_asr_empty_or_short_pcm_makes_no_request():
    calls = []

    def handler(request):
        calls.append(request)
        return httpx.Response(200, json={"text": "x"})

    asr = _asr_with(handler)
    assert await asr.transcribe(b"") == ""
    assert await asr.transcribe(_pcm(0.05)) == ""  # below 0.1 s
    assert calls == []
    await asr.aclose()


def test_asr_auth_header_only_when_api_key_set():
    with_key = OpenAICompatASR(api_key="secret")
    without_key = OpenAICompatASR()
    assert with_key._client.headers.get("authorization") == "Bearer secret"
    assert "authorization" not in without_key._client.headers


# --- PlaceholderTTS --------------------------------------------------------

async def _collect(tts: PlaceholderTTS, text: str, sr: int = SR):
    chunks = []
    async for chunk in tts.synthesize(text, sample_rate=sr):
        chunks.append(chunk)
    return chunks


async def test_placeholder_length_tracks_text_and_is_int16_aligned():
    tts = PlaceholderTTS(chars_per_second=6.0, first_packet_delay_ms=0)
    text = "今天天气很好啊"  # 7 chars -> ~1.167 s
    chunks = await _collect(tts, text)
    data = b"".join(chunks)
    assert len(data) % 2 == 0
    expected = len(text) / 6.0
    duration = len(data) / 2 / SR
    assert abs(duration - expected) <= expected * 0.1
    samples = array.array("h")
    samples.frombytes(data)
    assert max(abs(s) for s in samples) > 1000  # audible, not silent


async def test_placeholder_minimum_duration_for_short_text():
    tts = PlaceholderTTS(first_packet_delay_ms=0)
    data = b"".join(await _collect(tts, "嗯"))
    duration = len(data) / 2 / SR
    assert abs(duration - 0.3) <= 0.03


async def test_placeholder_chunks_follow_chunk_ms():
    tts = PlaceholderTTS(chunk_ms=100, first_packet_delay_ms=0)
    chunks = await _collect(tts, "一二三四五六七八")  # ~1.33 s
    assert all(len(c) == 2 * SR // 10 for c in chunks[:-1])
    assert len(chunks) >= 13


async def test_placeholder_blank_text_yields_nothing():
    tts = PlaceholderTTS(first_packet_delay_ms=0)
    assert await _collect(tts, "") == []
    assert await _collect(tts, "   \n") == []


async def test_placeholder_first_chunk_respects_delay():
    tts = PlaceholderTTS(first_packet_delay_ms=200)
    start = time.perf_counter()
    gen = tts.synthesize("你好", sample_rate=SR)
    await gen.__anext__()
    elapsed_ms = (time.perf_counter() - start) * 1000
    await gen.aclose()
    assert 150 <= elapsed_ms <= 600


# --- Registry --------------------------------------------------------------

def test_registry_builds_openai_compat_asr():
    asr = create_asr(ProviderConfig("openai_compat", {
        "base_url": "http://127.0.0.1:9/v1", "model": "m", "language": "en",
        "timeout": 5, "prompt": "p", "api_key": "k",
    }))
    assert isinstance(asr, OpenAICompatASR)
    assert asr._model == "m" and asr._language == "en"


def test_registry_builds_placeholder_tts():
    tts = create_tts(ProviderConfig("placeholder", {}))
    assert isinstance(tts, PlaceholderTTS)


def test_registry_rejects_unknown_option_keys():
    with pytest.raises(TypeError):
        create_asr(ProviderConfig("openai_compat", {"bogus": 1}))
