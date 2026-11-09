"""Codec interoperability and negotiation through the normal WebSocket entry."""
from __future__ import annotations

import json
import struct

import pytest
from websockets.asyncio.client import connect
from websockets.asyncio.server import serve

from relay.adpcm import encode_block
from relay.astrbot import ChatEvent
from relay.config import load_config
from relay.server import Relay


def test_encoder_matches_independent_ima_golden_vector():
    samples = (1000, 1100, 1300, 900, -1000, -32768, 32767, 0, 500)
    pcm = struct.pack("<9h", *samples)
    # Codes independently verified with audioop.lin2adpcm, state=(1000, 27).
    assert encode_block(pcm) == bytes.fromhex("e8031b00090046fff790")
    assert encode_block(struct.pack("<h", -32768)) == bytes.fromhex("008000000100")
    assert len(encode_block(bytes(1280))) == 326


@pytest.mark.parametrize("pcm", [b"", b"\x00", bytes(1282)])
def test_encoder_rejects_invalid_block_sizes(pcm):
    with pytest.raises(ValueError):
        encode_block(pcm)


@pytest.mark.parametrize("codec", [None, "pcm_s16le", "ima_adpcm", "unknown"])
@pytest.mark.parametrize("server_codec", ["ima_adpcm", "pcm_s16le"])
async def test_websocket_negotiates_downlink_preserves_pcm_upload_and_short_tail(tmp_path, codec, server_codec):
    pcm = bytes(1280) + struct.pack("<3h", 100, 120, 140)

    class ASR:
        async def transcribe(self, audio, sample_rate):
            assert audio == b"recorded PCM"
            return "你好"

    class AstrBot:
        async def chat(self, username, text, session_id):
            for character in "测试语音。&&sigh&&":
                yield ChatEvent("delta", text=character)
            yield ChatEvent("done")

    class TTS:
        async def synthesize(self, text, sample_rate):
            assert text == "测试语音。"
            yield pcm[:100]
            yield pcm[100:]

    cfg_path = tmp_path / "config.yaml"
    cfg_path.write_text("astrbot:\n  session_store: " + str(tmp_path / "sessions.json"))
    cfg = load_config(cfg_path)
    cfg.audio.prebuffer_ms = 0
    cfg.audio.downlink_codec = server_codec
    relay = Relay(cfg, ASR(), TTS(), AstrBot())
    async with serve(relay.handler, "127.0.0.1", 0) as server:
        port = server.sockets[0].getsockname()[1]
        async with connect(f"ws://127.0.0.1:{port}/ws", compression=None) as ws:
            hello = {"type": "hello", "device_id": "codec-test"}
            if codec is not None:
                hello["audio_codec"] = codec
            await ws.send(json.dumps(hello))
            state = json.loads(await ws.recv())
            expected_codec = "ima_adpcm" if codec == server_codec == "ima_adpcm" else "pcm_s16le"
            assert state["audio_codec"] == expected_codec
            await ws.send(json.dumps({"type": "listen_start"}))
            await ws.send(b"recorded PCM")
            await ws.send(json.dumps({"type": "listen_stop"}))
            blocks = []
            events = []
            replies = []
            async for message in ws:
                if isinstance(message, bytes):
                    blocks.append(message)
                    continue
                event = json.loads(message)
                events.append(event["type"])
                if event["type"] == "reply" and event.get("text"):
                    replies.append(event["text"])
                assert event["type"] != "error"
                if event["type"] == "state" and event["value"] == "idle":
                    break
            if expected_codec == "ima_adpcm":
                assert blocks == [encode_block(pcm[:1280]), encode_block(pcm[1280:])]
            else:
                assert b"".join(blocks) == pcm
            assert events.count("tts_start") == events.count("tts_end") == 1
            assert replies == ["测试语音。"]
