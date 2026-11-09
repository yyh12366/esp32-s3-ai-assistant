"""Tests for the Breeze TTS adapter, the resampler and ${ENV} config expansion."""
from __future__ import annotations

import json
import math
from array import array

import httpx
import pytest

from relay.audio import Resampler
from relay.config import load_config
from relay.tts.base import TTSError
from relay.tts.breeze import BreezeTTS


def tone(rate: int, seconds: float, hz: float = 440.0) -> bytes:
    n = int(rate * seconds)
    return array("h", (int(8000 * math.sin(2 * math.pi * hz * i / rate)) for i in range(n))).tobytes()


def crossings(pcm: bytes) -> int:
    s = array("h")
    s.frombytes(pcm)
    return sum(1 for a, b in zip(s, s[1:]) if a < 0 <= b)


@pytest.mark.parametrize("chunk", [1, 7, 480, 100000])
def test_resampler_24k_to_16k_length_and_pitch(chunk):
    src = tone(24000, 1.0)
    r = Resampler(24000, 16000)
    out = b"".join(r.process(src[i : i + chunk]) for i in range(0, len(src), chunk))
    assert len(out) % 2 == 0
    assert abs(len(out) / 2 - 16000) <= 2
    assert abs(crossings(out) - 440) <= 2  # same pitch after conversion


def test_resampler_passthrough():
    r = Resampler(16000, 16000)
    assert r.process(b"\x01\x02\x03") == b"\x01\x02"
    assert r.process(b"\x04") == b"\x03\x04"


def make_tts(handler, **kw) -> BreezeTTS:
    tts = BreezeTTS(api_key="brz_test", voice_id="voc_x", **kw)
    tts._client = httpx.AsyncClient(
        transport=httpx.MockTransport(handler), headers={"xi-api-key": "brz_test"}
    )
    return tts


async def test_breeze_request_and_resampled_stream():
    seen = {}

    def handler(req: httpx.Request) -> httpx.Response:
        seen["url"] = str(req.url)
        seen["key"] = req.headers.get("xi-api-key")
        seen["body"] = json.loads(req.content)
        return httpx.Response(200, headers={"content-type": "audio/pcm"}, content=tone(24000, 0.5))

    tts = make_tts(handler, instructions="温柔", speed=1.1)
    out = b"".join([c async for c in tts.synthesize(" 你好。 ", 16000)])
    assert seen["url"] == "https://api.breeze.blue/v1/text-to-speech/voc_x/stream?output_format=pcm"
    assert seen["key"] == "brz_test"
    assert seen["body"] == {
        "text": "你好。",
        "language_code": "zh",
        "instructions": "温柔",
        "voice_settings": {"speed": 1.1},
    }
    assert abs(len(out) / 2 - 8000) <= 2
    await tts.aclose()


async def test_breeze_error_and_retry():
    calls = []

    def handler(req):
        calls.append(1)
        if len(calls) == 1:
            return httpx.Response(429, headers={"retry-after": "0"}, json={"detail": "slow down"})
        return httpx.Response(422, json={"detail": "bad language_code"})

    tts = make_tts(handler)
    with pytest.raises(TTSError, match="422"):
        [c async for c in tts.synthesize("你好")]
    assert len(calls) == 2  # retried once after 429
    await tts.aclose()


async def test_breeze_blank_text_makes_no_request():
    def handler(req):
        raise AssertionError("no request expected")

    tts = make_tts(handler)
    assert [c async for c in tts.synthesize("   ")] == []
    await tts.aclose()


def test_breeze_requires_key_and_voice(monkeypatch):
    monkeypatch.delenv("BREEZE_API_KEY", raising=False)
    with pytest.raises(ValueError):
        BreezeTTS(voice_id="voc_x")
    with pytest.raises(ValueError):
        BreezeTTS(api_key="k")


def test_config_env_expansion(tmp_path, monkeypatch):
    monkeypatch.setenv("ASTRBOT_API_KEY", "abk_from_env")
    p = tmp_path / "c.yaml"
    p.write_text(
        "astrbot:\n  api_key: ${ASTRBOT_API_KEY}\n"
        "tts:\n  provider: breeze\n  breeze:\n    api_key: ${MISSING_VAR}\n    voice_id: voc_x\n",
        encoding="utf-8",
    )
    cfg = load_config(p)
    assert cfg.astrbot.api_key == "abk_from_env"
    assert cfg.tts.provider == "breeze"
    assert cfg.tts.options == {"api_key": "", "voice_id": "voc_x"}
