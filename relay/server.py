"""WebSocket relay: device <-> ASR -> AstrBot -> TTS <-> device."""
from __future__ import annotations

import asyncio
import json
import logging
import os
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Literal
from urllib.parse import parse_qs, urlsplit

from websockets.asyncio.server import ServerConnection
from websockets.exceptions import ConnectionClosed

from .asr.base import ASR, ASRError
from .adpcm import encode_block
from .astrbot import AstrBotClient, AstrBotError
from .config import Config
from .textproc import SentenceSplitter, clean_for_tts
from .tts.base import TTS, TTSError

log = logging.getLogger(__name__)
latency_log = logging.getLogger("relay.latency")

BYTES_PER_SAMPLE = 2  # 16-bit mono


class SessionStore:
    """device_id -> AstrBot session_id, persisted as JSON so context survives restarts."""

    def __init__(self, path: str):
        self.path = Path(path)
        self._data: dict[str, str] = {}
        if self.path.exists():
            try:
                self._data = json.loads(self.path.read_text(encoding="utf-8"))
            except (OSError, ValueError) as exc:
                log.warning("session store %s unreadable (%s), starting empty", self.path, exc)

    def get(self, device_id: str) -> str | None:
        return self._data.get(device_id)

    def set(self, device_id: str, session_id: str) -> None:
        if self._data.get(device_id) == session_id:
            return
        self._data[device_id] = session_id
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text(json.dumps(self._data, ensure_ascii=False, indent=2), encoding="utf-8")
        os.replace(tmp, self.path)


class Pacer:
    """Throttle PCM to real-time playback speed, keeping at most `prebuffer` seconds
    queued on the device. After an underrun (we had nothing to send) the virtual
    playback clock restarts from now instead of bursting to catch up."""

    def __init__(self, prebuffer_s: float):
        self.prebuffer = prebuffer_s
        self.play_end: float | None = None  # when the device finishes what we've sent

    async def before_send(self, duration_s: float) -> None:
        now = time.monotonic()
        if self.play_end is None or self.play_end < now:
            self.play_end = now
        wait = self.play_end - self.prebuffer - now
        if wait > 0:
            await asyncio.sleep(wait)
        self.play_end += duration_s

    async def drain(self) -> None:
        """Wait until the device should have played everything sent."""
        if self.play_end is not None:
            wait = self.play_end - time.monotonic()
            if wait > 0:
                await asyncio.sleep(wait)


@dataclass
class TurnTimings:
    t_stop: float = field(default_factory=time.monotonic)
    asr_ms: float | None = None
    astrbot_first_token_ms: float | None = None
    first_sentence_ms: float | None = None
    tts_first_packet_ms: list[float] = field(default_factory=list)
    first_audio_ms: float | None = None
    sentences: int = 0
    audio_s: float = 0.0

    def since_stop(self) -> float:
        return (time.monotonic() - self.t_stop) * 1000


def _only_connection_closed(exc: BaseException) -> bool:
    if isinstance(exc, BaseExceptionGroup):
        return all(_only_connection_closed(e) for e in exc.exceptions)
    return isinstance(exc, ConnectionClosed)


def _ms(v: float | None) -> str:
    return "-" if v is None else f"{v:.0f}ms"


class DeviceConnection:
    def __init__(self, relay: "Relay", ws: ServerConnection, device_id: str,
                 audio_codec: str = "pcm_s16le"):
        self.relay = relay
        self.cfg = relay.cfg
        self.ws = ws
        self.device_id = device_id
        self.audio_codec = audio_codec
        self.listening = False
        self.pcm = bytearray()
        self.turn_task: asyncio.Task | None = None
        self.state = ""
        sr = self.cfg.audio.sample_rate
        self.max_listen_bytes = self.cfg.audio.max_listen_seconds * sr * BYTES_PER_SAMPLE
        self.frame_bytes = sr * BYTES_PER_SAMPLE * self.cfg.audio.frame_ms // 1000

    # ---- sending helpers -------------------------------------------------
    async def send_json(self, **msg) -> None:
        await self.ws.send(json.dumps(msg, ensure_ascii=False))

    async def set_state(self, value: str) -> None:
        if value != self.state:
            self.state = value
            await self.send_json(type="state", value=value, audio_codec=self.audio_codec)

    # ---- main loop ---------------------------------------------------------
    async def run(self) -> None:
        await self.set_state("idle")
        try:
            async for message in self.ws:
                if isinstance(message, bytes):
                    self.on_audio(message)
                else:
                    await self.on_control(message)
        finally:
            await self.cancel_turn()

    def on_audio(self, data: bytes) -> None:
        if not self.listening:
            return
        if len(self.pcm) + len(data) > self.max_listen_bytes:
            if len(self.pcm) < self.max_listen_bytes:
                log.warning("[%s] utterance exceeds %ss, truncating",
                            self.device_id, self.cfg.audio.max_listen_seconds)
            self.pcm += data[: max(0, self.max_listen_bytes - len(self.pcm))]
            return
        self.pcm += data

    async def on_control(self, raw: str) -> None:
        try:
            msg = json.loads(raw)
            mtype = msg["type"]
        except (ValueError, KeyError, TypeError):
            await self.send_json(type="error", msg="invalid control message")
            return

        if mtype == "listen_start":
            await self.cancel_turn()  # barge-in: new utterance interrupts any reply
            self.pcm.clear()
            self.listening = True
            await self.set_state("listening")
        elif mtype == "listen_stop":
            if not self.listening:
                return
            self.listening = False
            pcm = bytes(self.pcm)
            self.pcm.clear()
            self.turn_task = asyncio.create_task(self.turn(pcm), name=f"turn-{self.device_id}")
        elif mtype == "abort":
            log.info("[%s] abort", self.device_id)
            self.listening = False
            self.pcm.clear()
            await self.cancel_turn()
            await self.set_state("idle")
        elif mtype == "hello":
            pass  # already handled; tolerate repeats
        else:
            log.debug("[%s] ignoring control message %r", self.device_id, mtype)

    async def cancel_turn(self) -> None:
        task, self.turn_task = self.turn_task, None
        if task and not task.done():
            task.cancel()
            try:
                await task
            except asyncio.CancelledError:
                pass
            except Exception:
                log.exception("[%s] turn failed while cancelling", self.device_id)

    # ---- one conversational turn ----------------------------------------
    async def turn(self, pcm: bytes) -> None:
        tm = TurnTimings()
        speaking = {"started": False}
        try:
            await self.set_state("thinking")
            audio_s = len(pcm) / (self.cfg.audio.sample_rate * BYTES_PER_SAMPLE)
            try:
                text = await self.relay.asr.transcribe(pcm, self.cfg.audio.sample_rate)
            except ASRError as exc:
                log.error("[%s] ASR failed: %s", self.device_id, exc)
                await self.send_json(type="error", msg=f"ASR failed: {exc}")
                await self.set_state("idle")
                return
            tm.asr_ms = tm.since_stop()
            latency_log.info("[%s] ASR %s for %.2fs audio: %r",
                             self.device_id, _ms(tm.asr_ms), audio_s, text)
            await self.send_json(type="asr", text=text)
            if not text:
                await self.set_state("idle")
                return

            await self.reply(text, tm, speaking)
        except asyncio.CancelledError:
            log.info("[%s] turn interrupted", self.device_id)
            if speaking["started"]:
                await self._safe(self.send_json(type="tts_end"))
            raise
        except Exception as exc:  # includes ExceptionGroup from the TaskGroup
            if _only_connection_closed(exc):
                return
            log.exception("[%s] turn failed", self.device_id)
            await self._safe(self.send_json(type="error", msg=f"internal error: {exc}"))
            if speaking["started"]:
                await self._safe(self.send_json(type="tts_end"))
            await self._safe(self.set_state("idle"))
        finally:
            self._log_summary(tm)

    async def reply(self, text: str, tm: TurnTimings, speaking: dict) -> None:
        sentences: asyncio.Queue[str | None] = asyncio.Queue()
        # bounded: TTS may run ahead of playback, but not unboundedly
        audio: asyncio.Queue[
            tuple[Literal["text"], str]
            | tuple[Literal["pcm"], bytes]
            | tuple[Literal["end"], None]
        ] = asyncio.Queue(maxsize=64)
        errors: list[str] = []

        async def produce() -> None:
            """AstrBot SSE -> sentences."""
            splitter = SentenceSplitter(self.cfg.text.min_chars, self.cfg.text.max_chars)
            client: AstrBotClient = self.relay.astrbot
            username = self.cfg.astrbot.username_prefix + self.device_id
            t_req = time.monotonic()

            def emit(parts: list[str]) -> None:
                for s in parts:
                    if tm.first_sentence_ms is None:
                        tm.first_sentence_ms = tm.since_stop()
                    sentences.put_nowait(s)

            try:
                async for ev in client.chat(username, text, self.relay.sessions.get(self.device_id)):
                    if ev.kind == "session" and ev.session_id:
                        self.relay.sessions.set(self.device_id, ev.session_id)
                    elif ev.kind == "delta":
                        if tm.astrbot_first_token_ms is None:
                            tm.astrbot_first_token_ms = (time.monotonic() - t_req) * 1000
                            latency_log.info("[%s] AstrBot first token %s",
                                             self.device_id, _ms(tm.astrbot_first_token_ms))
                        emit(splitter.feed(ev.text))
                    elif ev.kind == "error":
                        errors.append(f"AstrBot: {ev.text}")
                        break
                    elif ev.kind == "done":
                        break
                emit(splitter.flush())
            except AstrBotError as exc:
                errors.append(f"AstrBot request failed: {exc}")
            finally:
                sentences.put_nowait(None)

        async def synthesize() -> None:
            """sentences -> (reply text marker, PCM chunks) in playback order."""
            tts: TTS = self.relay.tts
            while (raw := await sentences.get()) is not None:
                # the device shows the same cleaned text it speaks (no Markdown/emoji/URLs)
                spoken = clean_for_tts(raw)
                if not spoken:
                    continue
                await audio.put(("text", spoken))
                t0 = time.monotonic()
                first = True
                try:
                    async for chunk in tts.synthesize(spoken, self.cfg.audio.sample_rate):
                        if first:
                            first = False
                            ms = (time.monotonic() - t0) * 1000
                            tm.tts_first_packet_ms.append(ms)
                            latency_log.info("[%s] TTS first packet %s: %r",
                                             self.device_id, _ms(ms), spoken)
                        if chunk:
                            await audio.put(("pcm", chunk))
                except TTSError as exc:
                    log.error("[%s] TTS failed for %r: %s", self.device_id, spoken, exc)
                    errors.append(f"TTS failed: {exc}")
            await audio.put(("end", None))

        async def play() -> None:
            """Send reply text + paced PCM frames to the device."""
            pacer = Pacer(self.cfg.audio.prebuffer_ms / 1000)
            frame_s = self.cfg.audio.frame_ms / 1000
            pending = bytearray()

            async def send_frame(frame: bytes) -> None:
                if not speaking["started"]:
                    speaking["started"] = True
                    await self.set_state("speaking")
                    await self.send_json(type="tts_start")
                await pacer.before_send(len(frame) / self.frame_bytes * frame_s)
                if tm.first_audio_ms is None:
                    tm.first_audio_ms = tm.since_stop()
                if self.audio_codec == "ima_adpcm":
                    # Blocks carry independent predictor state and fit the device decoder.
                    for offset in range(0, len(frame), 1280):
                        await self.ws.send(encode_block(frame[offset:offset + 1280]))
                else:
                    await self.ws.send(frame)
                tm.audio_s += len(frame) / (self.cfg.audio.sample_rate * BYTES_PER_SAMPLE)

            while True:
                item = await audio.get()
                if item[0] == "text":
                    tm.sentences += 1
                    await self.send_json(type="reply", text=item[1], final=False)
                elif item[0] == "pcm":
                    pending += item[1]
                    while len(pending) >= self.frame_bytes:
                        await send_frame(bytes(pending[: self.frame_bytes]))
                        del pending[: self.frame_bytes]
                else:  # end
                    if pending:
                        if len(pending) % 2:
                            pending = pending[:-1]
                        await send_frame(bytes(pending))
                    break

            for err in errors:
                await self.send_json(type="error", msg=err)
            await self.send_json(type="reply", text="", final=True)
            if speaking["started"]:
                await self.send_json(type="tts_end")
                speaking["started"] = False
                await pacer.drain()  # stay "speaking" until the device has played it
            await self.set_state("idle")

        async with asyncio.TaskGroup() as tg:
            tg.create_task(produce())
            tg.create_task(synthesize())
            tg.create_task(play())

    async def _safe(self, coro) -> None:
        try:
            await coro
        except ConnectionClosed:
            pass

    def _log_summary(self, tm: TurnTimings) -> None:
        tts = tm.tts_first_packet_ms
        latency_log.info(
            "[%s] turn summary: asr=%s astrbot_first_token=%s first_sentence=%s "
            "tts_first_packet=%s (avg %s over %d) first_audio=%s sentences=%d audio=%.1fs total=%s",
            self.device_id, _ms(tm.asr_ms), _ms(tm.astrbot_first_token_ms),
            _ms(tm.first_sentence_ms), _ms(tts[0] if tts else None),
            _ms(sum(tts) / len(tts) if tts else None), len(tts), _ms(tm.first_audio_ms),
            tm.sentences, tm.audio_s, _ms(tm.since_stop()),
        )


class Relay:
    def __init__(self, cfg: Config, asr: ASR, tts: TTS, astrbot: AstrBotClient):
        self.cfg = cfg
        self.asr = asr
        self.tts = tts
        self.astrbot = astrbot
        self.sessions = SessionStore(cfg.astrbot.session_store)
        self.devices: dict[str, DeviceConnection] = {}

    async def handler(self, ws: ServerConnection) -> None:
        peer = ws.remote_address
        url = urlsplit(ws.request.path if ws.request else "/")
        if self.cfg.server.path and url.path.rstrip("/") != self.cfg.server.path.rstrip("/"):
            await ws.close(4004, "not found")
            return
        token = self.cfg.server.auth_token
        if token and parse_qs(url.query).get("token", [""])[0] != token:
            log.warning("rejecting %s: bad token", peer)
            await ws.close(4001, "unauthorized")
            return

        try:
            raw = await asyncio.wait_for(ws.recv(), timeout=10)
            hello = json.loads(raw)
            device_id = str(hello["device_id"]).strip()
            if hello.get("type") != "hello" or not device_id:
                raise ValueError("first message must be hello with device_id")
        except (asyncio.TimeoutError, ValueError, KeyError, TypeError) as exc:
            await self._reject(ws, f"expected hello: {exc}")
            return
        except ConnectionClosed:
            return

        old = self.devices.get(device_id)
        if old is not None:
            log.info("[%s] new connection replaces old one", device_id)
            await old.ws.close(4000, "replaced by new connection")

        audio_codec = "pcm_s16le"
        if self.cfg.audio.downlink_codec == "ima_adpcm" and hello.get("audio_codec") == "ima_adpcm":
            audio_codec = "ima_adpcm"
        conn = DeviceConnection(self, ws, device_id, audio_codec)
        self.devices[device_id] = conn
        log.info("[%s] connected from %s (session %s, audio %s)", device_id, peer,
                 self.sessions.get(device_id) or "new", audio_codec)
        try:
            await conn.run()
        except ConnectionClosed:
            pass
        finally:
            if self.devices.get(device_id) is conn:
                del self.devices[device_id]
            log.info("[%s] disconnected", device_id)

    async def _reject(self, ws: ServerConnection, reason: str) -> None:
        try:
            await ws.send(json.dumps({"type": "error", "msg": reason}))
            await ws.close(4002, "protocol error")
        except ConnectionClosed:
            pass
