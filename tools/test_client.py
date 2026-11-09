#!/usr/bin/env python3
"""Simulated voice device for astrbot-relay.

Streams 40 ms PCM frames to the relay at real-time speed, prints every control
message, and reports client-side latency, audio pacing and (optionally) an
abort check. Exit code is non-zero on timeout, relay error or failed abort.
"""
from __future__ import annotations

import argparse
import asyncio
import contextlib
import json
import math
import queue
import struct
import sys
import threading
import time
import wave
from array import array
from dataclasses import dataclass, field
from typing import Any, Optional

from websockets.asyncio.client import connect
from websockets.exceptions import ConnectionClosed

SAMPLE_RATE = 16000
BYTES_PER_SEC = SAMPLE_RATE * 2          # 16-bit mono
FRAME_BYTES = 1280                       # 40 ms of PCM
FRAME_S = FRAME_BYTES / BYTES_PER_SEC
DEFAULT_TONE_S = 1.5
ABORT_AUDIO_LIMIT_S = 0.3   # audio may trail the abort by at most this long
ABORT_IDLE_LIMIT_S = 1.0    # state must be idle within this time of the abort
ABORT_WATCH_S = 1.5         # keep listening this long after sending the abort
KNOWN_TYPES = {"asr", "reply", "state", "tts_start", "tts_end", "error"}


class Marker:
    """Queue marker for events generated locally."""

    def __init__(self, name: str) -> None:
        self.name = name

    def __repr__(self) -> str:
        return self.name


LISTEN_STOP = Marker("LISTEN_STOP")
CLOSED = Marker("CLOSED")


# ---------------------------------------------------------------- audio input

def decode_wav(data: bytes) -> tuple[int, list[float]]:
    """Parse a RIFF/WAVE file (PCM 8/16/24/32-bit or float 32/64-bit).

    Returns (sample_rate, mono samples scaled to the int16 range).
    """
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE file")
    fmt: Optional[bytes] = None
    raw: Optional[bytes] = None
    pos = 12
    while pos + 8 <= len(data) and (fmt is None or raw is None):
        chunk_id = data[pos:pos + 4]
        size = struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if chunk_id == b"fmt ":
            fmt = body
        elif chunk_id == b"data":
            raw = body
        pos += 8 + size + (size & 1)  # chunks are word-aligned
    if fmt is None or raw is None:
        raise ValueError("missing fmt or data chunk")

    tag, channels, rate, _, _, bits = struct.unpack("<HHIIHH", fmt[:16])
    if tag == 0xFFFE and len(fmt) >= 26:  # WAVE_FORMAT_EXTENSIBLE: real tag in sub-format GUID
        tag = struct.unpack("<H", fmt[24:26])[0]
    if channels < 1:
        raise ValueError("invalid channel count")

    samples = unpack_samples(raw, tag, bits)
    if channels > 1:  # average channels to mono
        samples = [sum(frame) / channels for frame in zip(*(samples[c::channels] for c in range(channels)))]
    return rate, samples


def unpack_samples(raw: bytes, tag: int, bits: int) -> list[float]:
    """Decode little-endian samples and scale them to the int16 range."""
    if tag == 1:  # integer PCM
        if bits == 8:
            return [(b - 128) * 256.0 for b in raw]
        if bits == 16:
            return [float(v) for (v,) in struct.iter_unpack("<h", raw[:len(raw) // 2 * 2])]
        if bits == 24:
            n = len(raw) // 3 * 3
            return [int.from_bytes(raw[i:i + 3], "little", signed=True) / 256.0 for i in range(0, n, 3)]
        if bits == 32:
            return [v / 65536.0 for (v,) in struct.iter_unpack("<i", raw[:len(raw) // 4 * 4])]
    elif tag == 3:  # IEEE float, nominal range [-1, 1]
        if bits == 32:
            return [v * 32767.0 for (v,) in struct.iter_unpack("<f", raw[:len(raw) // 4 * 4])]
        if bits == 64:
            return [v * 32767.0 for (v,) in struct.iter_unpack("<d", raw[:len(raw) // 8 * 8])]
    raise ValueError(f"unsupported WAV format tag={tag} bits={bits}")


def resample_linear(x: list[float], src_rate: int, dst_rate: int) -> list[float]:
    """Linear-interpolation resampler, adequate for speech tests."""
    if src_rate == dst_rate or not x:
        return list(x)
    step = src_rate / dst_rate
    n_out = int(len(x) * dst_rate / src_rate)
    last = len(x) - 1
    out: list[float] = []
    for j in range(n_out):
        pos = j * step
        i = int(pos)
        if i >= last:
            out.append(x[last])
        else:
            out.append(x[i] + (x[i + 1] - x[i]) * (pos - i))
    return out


def to_pcm16le(samples: list[float]) -> bytes:
    """Round and clamp to int16, little-endian."""
    arr = array("h", (max(-32768, min(32767, round(v))) for v in samples))
    if sys.byteorder == "big":
        arr.byteswap()
    return arr.tobytes()


def load_wav_pcm(path: str) -> bytes:
    """Load any PCM/float WAV and convert it to 16 kHz mono int16 PCM bytes."""
    with open(path, "rb") as f:
        rate, mono = decode_wav(f.read())
    return to_pcm16le(resample_linear(mono, rate, SAMPLE_RATE))


def make_tone_pcm(seconds: float, freq: float = 440.0, amp: float = 0.3) -> bytes:
    """Synthetic sine tone as 16 kHz mono int16 PCM."""
    step = 2 * math.pi * freq / SAMPLE_RATE
    return to_pcm16le([amp * 32767 * math.sin(step * i) for i in range(int(seconds * SAMPLE_RATE))])


def write_wav(path: str, pcm: bytes) -> None:
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(pcm)


def import_sounddevice():
    """Import sounddevice lazily; only --mic and --play need it."""
    try:
        import sounddevice
    except (ImportError, OSError):
        sys.exit("sounddevice is not available: run `pip install sounddevice` (needed for --mic / --play)")
    return sounddevice


def record_from_mic(number: int, total: int) -> bytes:
    """Blocking: Enter to start, Enter to stop. Run via asyncio.to_thread."""
    sd = import_sounddevice()
    input(f"[turn {number}/{total}] Press Enter to start recording...")
    chunks: list[bytes] = []

    def callback(indata, frames, time_info, status) -> None:
        chunks.append(bytes(indata))

    with sd.InputStream(samplerate=SAMPLE_RATE, channels=1, dtype="int16", callback=callback):
        input("Recording... press Enter to stop.")
    return b"".join(chunks)


class LivePlayer:
    """Plays 16 kHz mono int16 PCM on a background thread."""

    def __init__(self) -> None:
        sd = import_sounddevice()
        self._q: queue.Queue = queue.Queue()
        self._carry = b""
        self._thread = threading.Thread(target=self._run, args=(sd,), daemon=True)
        self._thread.start()

    def _run(self, sd) -> None:
        with sd.RawOutputStream(samplerate=SAMPLE_RATE, channels=1, dtype="int16") as stream:
            while (chunk := self._q.get()) is not None:
                stream.write(chunk)

    def write(self, data: bytes) -> None:
        data = self._carry + data
        cut = len(data) // 2 * 2  # whole int16 samples only
        self._carry = data[cut:]
        if cut:
            self._q.put(data[:cut])

    def close(self) -> None:
        """Blocking: waits until the queued audio has played."""
        self._q.put(None)
        self._thread.join()


# ------------------------------------------------------------------ metrics

@dataclass
class Event:
    t: float                 # monotonic seconds
    kind: str                # listen_stop, abort, audio, asr, reply, state, tts_start, tts_end, error, other
    msg: dict = field(default_factory=dict)
    nbytes: int = 0


@dataclass
class Metrics:
    asr_ms: Optional[float]
    reply_ms: Optional[float]
    audio_ms: Optional[float]
    audio_s: float
    span_s: float
    pacing: Optional[float]
    burst_ms: float
    sentences: int
    tts_starts: int
    tts_ends: int
    frames: int


def max_burst_ms(audio: list[tuple[float, int]], window: float = 0.1) -> float:
    """Largest amount of audio (ms) received within any `window` seconds.

    `audio` is a time-ordered list of (arrival time, byte count).
    """
    best = total = 0
    lo = 0
    for hi, (t_hi, n_hi) in enumerate(audio):
        total += n_hi
        while t_hi - audio[lo][0] >= window:
            total -= audio[lo][1]
            lo += 1
        best = max(best, total)
    return best / BYTES_PER_SEC * 1000.0


def compute_metrics(events: list[Event]) -> Metrics:
    stop = next((e.t for e in events if e.kind == "listen_stop"), None)

    def since_stop(t: Optional[float]) -> Optional[float]:
        return None if t is None or stop is None else (t - stop) * 1000.0

    def first(kind: str, pred=lambda e: True) -> Optional[float]:
        return next((e.t for e in events if e.kind == kind and pred(e)), None)

    audio = [(e.t, e.nbytes) for e in events if e.kind == "audio"]
    audio_s = sum(n for _, n in audio) / BYTES_PER_SEC
    span = audio[-1][0] - audio[0][0] if len(audio) > 1 else 0.0
    return Metrics(
        asr_ms=since_stop(first("asr")),
        reply_ms=since_stop(first("reply", lambda e: bool(e.msg.get("text")))),
        audio_ms=since_stop(audio[0][0] if audio else None),
        audio_s=audio_s,
        span_s=span,
        pacing=audio_s / span if span > 0 else None,
        burst_ms=max_burst_ms(audio),
        sentences=sum(1 for e in events if e.kind == "reply" and e.msg.get("text")),
        tts_starts=sum(1 for e in events if e.kind == "tts_start"),
        tts_ends=sum(1 for e in events if e.kind == "tts_end"),
        frames=len(audio),
    )


def abort_check(events: list[Event], abort_t: float) -> tuple[bool, str]:
    """Audio must stop within ABORT_AUDIO_LIMIT_S and state must return to idle."""
    after = [e.t for e in events if e.kind == "audio" and e.t > abort_t]
    tail = (max(after) - abort_t) if after else 0.0
    idle_t = next((e.t for e in events
                   if e.kind == "state" and e.msg.get("value") == "idle" and e.t > abort_t), None)
    idle_s = (idle_t - abort_t) if idle_t is not None else None
    ok = tail <= ABORT_AUDIO_LIMIT_S and idle_s is not None and idle_s <= ABORT_IDLE_LIMIT_S
    idle_txt = f"{idle_s * 1000:.0f} ms" if idle_s is not None else "never"
    return ok, f"last audio {tail * 1000:.0f} ms after abort, idle after {idle_txt}"


# ------------------------------------------------------------------- network

@dataclass
class TurnResult:
    number: int
    reasons: list[str] = field(default_factory=list)
    events: list[Event] = field(default_factory=list)
    audio: bytes = b""
    abort_detail: Optional[str] = None

    @property
    def ok(self) -> bool:
        return not self.reasons


async def pump(ws, q: asyncio.Queue) -> None:
    """Reader task: timestamps every incoming frame as it arrives."""
    try:
        async for msg in ws:
            q.put_nowait((time.monotonic(), msg))
    except ConnectionClosed:
        pass
    finally:
        q.put_nowait((time.monotonic(), CLOSED))


async def send_audio(ws, pcm: bytes, q: asyncio.Queue) -> None:
    """Send listen_start, the PCM paced at real time, then listen_stop."""
    try:
        await ws.send(json.dumps({"type": "listen_start"}))
        t0 = time.monotonic()
        frames = (len(pcm) + FRAME_BYTES - 1) // FRAME_BYTES
        for i in range(frames):
            chunk = pcm[i * FRAME_BYTES:(i + 1) * FRAME_BYTES]
            await ws.send(chunk.ljust(FRAME_BYTES, b"\0"))
            await asyncio.sleep(max(0.0, t0 + (i + 1) * FRAME_S - time.monotonic()))
        await ws.send(json.dumps({"type": "listen_stop"}))
        q.put_nowait((time.monotonic(), LISTEN_STOP))
    except ConnectionClosed:
        q.put_nowait((time.monotonic(), CLOSED))


def stamp(t: float, ref: float) -> str:
    ms = (t - ref) * 1000.0
    return f"[{'+' if ms >= 0 else '-'}{abs(ms):5.0f}ms]"


def describe(msg: dict) -> str:
    mtype = msg.get("type")
    if mtype == "asr":
        return f"asr: {msg.get('text', '')}"
    if mtype == "reply":
        final = " (final)" if msg.get("final") else ""
        return f"reply{final}: {msg.get('text', '')}"
    if mtype == "state":
        return f"state: {msg.get('value')}"
    if mtype == "error":
        return f"ERROR: {msg.get('msg')}"
    rest = {k: v for k, v in msg.items() if k != "type"}
    return f"{mtype}" + (f" {json.dumps(rest, ensure_ascii=False)}" if rest else "")


async def run_turn(ws, q: asyncio.Queue, number: int, total: int, pcm: bytes,
                   args: argparse.Namespace, abort_after: Optional[float],
                   player: Optional[LivePlayer]) -> TurnResult:
    result = TurnResult(number)
    events: list[Event] = []
    audio = bytearray()
    print(f"\n=== turn {number}/{total}: {len(pcm) / BYTES_PER_SEC:.2f} s of audio ===", flush=True)

    while not q.empty():  # drop leftovers from the previous turn
        q.get_nowait()

    start = time.monotonic()
    deadline = start + args.timeout
    sender = asyncio.create_task(send_audio(ws, pcm, q))
    stop_t: Optional[float] = None
    abort_at: Optional[float] = None   # when to send abort (abort mode only)
    abort_t: Optional[float] = None    # when abort was actually sent
    open_tts = 0
    final_seen = False
    last_state: Optional[str] = None
    timed_out = closed = early_end = False

    def normal_done() -> bool:
        return stop_t is not None and final_seen and open_tts == 0 and last_state == "idle"

    try:
        while True:
            now = time.monotonic()
            ref = stop_t if stop_t is not None else start

            # Abort mode: send abort once due, then watch for a fixed window.
            if abort_after is not None:
                if abort_t is None and abort_at is not None and now >= abort_at:
                    try:
                        await ws.send(json.dumps({"type": "abort"}))
                    except ConnectionClosed:
                        closed = True
                        break
                    abort_t = time.monotonic()
                    events.append(Event(abort_t, "abort"))
                    print(f"{stamp(abort_t, ref)} >> abort sent", flush=True)
                    continue
                if abort_t is None and normal_done():
                    early_end = True  # reply finished before the abort point
                    break
                if abort_t is not None and now >= abort_t + ABORT_WATCH_S:
                    break
            elif normal_done():
                break

            if now >= deadline:
                timed_out = True
                break

            wake = deadline
            if abort_t is None and abort_at is not None:
                wake = min(wake, abort_at)
            if abort_t is not None:
                wake = min(wake, abort_t + ABORT_WATCH_S)
            try:
                t, item = await asyncio.wait_for(q.get(), timeout=max(0.0, wake - now))
            except asyncio.TimeoutError:
                continue

            if item is CLOSED:
                closed = True
                break
            if item is LISTEN_STOP:
                stop_t = t
                events.append(Event(t, "listen_stop"))
                print(f"{stamp(t, t)} >> listen_stop sent", flush=True)
                continue
            if isinstance(item, (bytes, bytearray)):
                events.append(Event(t, "audio", nbytes=len(item)))
                audio += item
                if player is not None:
                    player.write(bytes(item))
                continue

            try:
                msg = json.loads(item)
            except ValueError:
                msg = {"type": "raw", "text": item}
            if not isinstance(msg, dict):
                msg = {"type": "raw", "value": msg}
            mtype = msg.get("type")
            kind = mtype if mtype in KNOWN_TYPES else "other"
            events.append(Event(t, kind, msg=msg))
            print(f"{stamp(t, ref)} {describe(msg)}", flush=True)

            if kind == "tts_start":
                open_tts += 1
                if abort_after is not None and abort_at is None and abort_t is None:
                    abort_at = t + abort_after
            elif kind == "tts_end":
                open_tts -= 1
            elif kind == "reply" and msg.get("final"):
                final_seen = True
            elif kind == "state":
                last_state = msg.get("value")
            elif kind == "error":
                result.reasons.append(f"relay error: {msg.get('msg')}")
                break
    finally:
        sender.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await sender

    if timed_out:
        result.reasons.append(f"timed out after {args.timeout:g} s")
    if closed:
        result.reasons.append("connection closed by relay")
    if abort_after is not None:
        if abort_t is None:
            if not result.reasons:
                reason = "reply finished" if early_end else "no tts_start arrived"
                result.reasons.append(f"abort not sent ({reason} before --abort-after)")
        else:
            ok, detail = abort_check(events, abort_t)
            result.abort_detail = detail
            if not ok:
                result.reasons.append(f"abort check failed ({detail})")
            print(f"ABORT CHECK: {'PASS' if ok else 'FAIL'} - {detail}", flush=True)
    elif not result.reasons and not early_end and not normal_done():
        result.reasons.append("turn ended unexpectedly")

    result.events = events
    result.audio = bytes(audio)
    print_summary(compute_metrics(events))
    return result


def fmt_ms(v: Optional[float]) -> str:
    return "n/a" if v is None else f"{v:.0f} ms"


def print_summary(m: Metrics) -> None:
    pacing = f"{m.pacing:.2f}" if m.pacing is not None else "n/a"
    matched = "matched" if m.tts_starts == m.tts_ends else "MISMATCH"
    print("--- turn summary (client side) ---")
    print(f"  listen_stop -> asr:          {fmt_ms(m.asr_ms)}")
    print(f"  listen_stop -> first reply:  {fmt_ms(m.reply_ms)}")
    print(f"  listen_stop -> first audio:  {fmt_ms(m.audio_ms)}")
    print(f"  audio {m.audio_s:.2f} s over {m.span_s:.2f} s wall, pacing ratio {pacing} (~1.0 = throttled)")
    print(f"  max burst in any 100 ms window: {m.burst_ms:.0f} ms of audio")
    print(f"  sentences (reply msgs): {m.sentences}, tts start/end: {m.tts_starts}/{m.tts_ends} ({matched})")
    print(f"  binary audio frames: {m.frames}")


# ----------------------------------------------------------------------- CLI

async def run(args: argparse.Namespace, pcm: Optional[bytes]) -> int:
    try:
        ws = await connect(args.url, max_size=None, open_timeout=10)
    except Exception as exc:
        print(f"connect to {args.url} failed: {exc}", file=sys.stderr)
        return 1

    q: asyncio.Queue = asyncio.Queue()
    pump_task = asyncio.create_task(pump(ws, q))
    results: list[TurnResult] = []
    try:
        await ws.send(json.dumps({"type": "hello", "device_id": args.device_id}))
        print(f"connected to {args.url} as {args.device_id}", flush=True)
        for n in range(1, args.turns + 1):
            if pump_task.done():
                print("connection already closed", file=sys.stderr)
                break
            turn_pcm = pcm if pcm is not None else await asyncio.to_thread(record_from_mic, n, args.turns)
            if not turn_pcm:
                print("no audio recorded", file=sys.stderr)
                break
            player = LivePlayer() if args.play else None
            try:
                abort_after = args.abort_after if n == 1 else None
                res = await run_turn(ws, q, n, args.turns, turn_pcm, args, abort_after, player)
            finally:
                if player is not None:
                    await asyncio.to_thread(player.close)
            results.append(res)
            verdict = "PASS" if res.ok else "FAIL - " + "; ".join(res.reasons)
            print(f"turn {n}: {verdict}", flush=True)
            if not res.ok:
                break
    finally:
        pump_task.cancel()
        with contextlib.suppress(Exception):
            await ws.close()

    if results and args.out:
        last = results[-1]
        write_wav(args.out, last.audio)
        print(f"\nwrote {len(last.audio) / BYTES_PER_SEC:.2f} s of TTS audio (turn {last.number}) to {args.out}")
    ok = len(results) == args.turns and all(r.ok for r in results)
    print(f"\nOVERALL: {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


def parse_args(argv: Optional[list[str]] = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Simulated voice device for astrbot-relay.")
    p.add_argument("--url", default="ws://127.0.0.1:8765/ws", help="relay WebSocket URL (may include ?token=)")
    p.add_argument("--device-id", default="test-device-01")
    src = p.add_mutually_exclusive_group()
    src.add_argument("--wav", metavar="PATH", help="send a WAV file (any rate, width or channel count)")
    src.add_argument("--mic", action="store_true", help="record each turn from the default mic (needs sounddevice)")
    src.add_argument("--tone", metavar="SECONDS", type=float,
                     help=f"send a synthetic tone (default when no input is given: {DEFAULT_TONE_S} s)")
    p.add_argument("--turns", type=int, default=1, help="turns on the same connection (default 1)")
    p.add_argument("--abort-after", type=float, metavar="SECONDS",
                   help="turn 1 only: send abort this long after its first tts_start, then check the abort")
    p.add_argument("--out", default="reply.wav", help="write the last turn's TTS audio here (default reply.wav)")
    p.add_argument("--play", action="store_true", help="play received audio live (needs sounddevice)")
    p.add_argument("--timeout", type=float, default=60.0, help="per-turn timeout in seconds (default 60)")
    args = p.parse_args(argv)
    if args.turns < 1:
        p.error("--turns must be at least 1")
    if args.tone is not None and args.tone <= 0:
        p.error("--tone must be positive")
    if args.abort_after is not None and args.abort_after < 0:
        p.error("--abort-after must not be negative")
    return args


def main(argv: Optional[list[str]] = None) -> int:
    args = parse_args(argv)
    if args.mic or args.play:
        import_sounddevice()  # fail early with a clear message

    if args.wav:
        try:
            pcm: Optional[bytes] = load_wav_pcm(args.wav)
        except (OSError, ValueError, struct.error) as exc:
            print(f"cannot load {args.wav}: {exc}", file=sys.stderr)
            return 2
        print(f"input: {args.wav} -> {len(pcm) / BYTES_PER_SEC:.2f} s, 16 kHz mono")
    elif args.mic:
        pcm = None
        print("input: microphone")
    else:
        seconds = args.tone if args.tone is not None else DEFAULT_TONE_S
        pcm = make_tone_pcm(seconds)
        print(f"input: {seconds:g} s synthetic tone")

    try:
        return asyncio.run(run(args, pcm))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
