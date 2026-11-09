"""Media HTTP service for the device: TXT books and MP3 tracks.

Deliberately dependency-free: asyncio streams speak just enough HTTP/1.1 for the
firmware's tiny client, and ffmpeg does MP3 -> 16 kHz mono PCM on the relay so the
ESP32-C3 never has to decode audio (no PSRAM, tight internal RAM).
"""
from __future__ import annotations

import asyncio
import hmac
import logging
import shutil
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlsplit

from .adpcm import encode_block
from .config import MediaConfig

log = logging.getLogger(__name__)

PCM_RATE = 16000
BOOK_SUFFIXES = {".txt"}
MUSIC_SUFFIXES = {".mp3", ".m4a", ".flac", ".wav", ".ogg"}
PCM_CHUNK = 4096
OTA_CHUNK = 16384                    # 固件流式输出的读取块大小
# 固件 OTA：只允许这两个固定文件名，不接受任何路径片段（防目录穿越）。
OTA_FILES = {
    "/ota/version.json": ("version.json", "application/json; charset=utf-8"),
    "/ota/firmware.bin": ("firmware.bin", "application/octet-stream"),
}
ADPCM_SAMPLES = 640                  # 每个 ADPCM 块 40 ms，与设备端 AV_ADPCM_MAX_SAMPLES 一致
ADPCM_FRAME_BYTES = ADPCM_SAMPLES * 2

def _is_wide(ch: str) -> bool:
    code = ord(ch)
    return (
        0x1100 <= code <= 0x115F
        or 0x2E80 <= code <= 0xA4CF
        or 0xAC00 <= code <= 0xD7A3
        or 0xF900 <= code <= 0xFAFF
        or 0xFE30 <= code <= 0xFE6F
        or 0xFF00 <= code <= 0xFF60
        or 0xFFE0 <= code <= 0xFFE6
    )


def wrap_lines(text: str, cols: int, lines: int) -> list[list[str]]:
    """Wrap text into pages of `lines` rows, each at most `cols` columns wide.

    Width is terminal-style: CJK glyphs count 2 so the device's 12px font lines up.
    """
    cols = max(4, cols)
    lines = max(1, lines)
    rows: list[str] = []
    for raw in text.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        if not raw.strip():
            rows.append("")
            continue
        current = ""
        width = 0
        for ch in raw:
            w = 2 if _is_wide(ch) else 1
            if width + w > cols:
                rows.append(current)
                current, width = "", 0
            current += ch
            width += w
        rows.append(current)
    if not rows:
        rows = [""]
    return [rows[i : i + lines] for i in range(0, len(rows), lines)]


def _read_text(path: Path) -> str:
    data = path.read_bytes()
    for encoding in ("utf-8-sig", "gb18030"):
        try:
            return data.decode(encoding)
        except UnicodeDecodeError:
            continue
    return data.decode("utf-8", errors="replace")


def _title(path: Path) -> str:
    return path.stem.strip() or path.name


class MediaLibrary:
    def __init__(self, root: str | Path):
        self.root = Path(root)

    def _files(self, sub: str, suffixes: set[str]) -> list[Path]:
        folder = self.root / sub
        if not folder.is_dir():
            return []
        return sorted(
            (p for p in folder.iterdir() if p.is_file() and p.suffix.lower() in suffixes),
            key=lambda p: p.name,
        )

    def books(self) -> list[Path]:
        return self._files("books", BOOK_SUFFIXES)

    def tracks(self) -> list[Path]:
        return self._files("music", MUSIC_SUFFIXES)

    def book_text(self, index: int) -> str | None:
        books = self.books()
        if not 0 <= index < len(books):
            return None
        return _read_text(books[index])

    def track(self, index: int) -> Path | None:
        tracks = self.tracks()
        if not 0 <= index < len(tracks):
            return None
        return tracks[index]


def take_pcm_frames(pending: bytearray, frame_bytes: int = ADPCM_FRAME_BYTES) -> list[bytes]:
    """从 pending 头部切出完整的 PCM 帧（就地删除已取出的字节），剩余不足一帧的留给下次。"""
    frames: list[bytes] = []
    while len(pending) >= frame_bytes:
        frames.append(bytes(pending[:frame_bytes]))
        del pending[:frame_bytes]
    return frames


async def _send(writer: asyncio.StreamWriter, status: str, body: bytes,
                headers: dict[str, str] | None = None) -> None:
    lines = [f"HTTP/1.1 {status}", f"Content-Length: {len(body)}", "Connection: close"]
    for key, value in (headers or {}).items():
        lines.append(f"{key}: {value}")
    writer.write(("\r\n".join(lines) + "\r\n\r\n").encode("latin-1") + body)
    await writer.drain()


async def _send_text(writer: asyncio.StreamWriter, body: str,
                     headers: dict[str, str] | None = None) -> None:
    merged = {"Content-Type": "text/plain; charset=utf-8"}
    merged.update(headers or {})
    await _send(writer, "200 OK", body.encode("utf-8"), merged)


class MediaServer:
    def __init__(self, cfg: MediaConfig, library: MediaLibrary, auth_token: str = ""):
        self.cfg = cfg
        self.library = library
        self.auth_token = auth_token
        self._server: asyncio.AbstractServer | None = None

    async def start(self) -> None:
        self._server = await asyncio.start_server(self._handle, self.cfg.host, self.cfg.port)
        log.info("media listening on http://%s:%d (root=%s)", self.cfg.host, self.cfg.port,
                 self.library.root)

    async def stop(self) -> None:
        if self._server is not None:
            self._server.close()
            await self._server.wait_closed()
            self._server = None

    async def _handle(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            raw = await asyncio.wait_for(reader.readline(), timeout=10)
            parts = raw.decode("latin-1").split()
            if len(parts) < 2:
                await _send(writer, "400 Bad Request", b"")
                return
            url = urlsplit(parts[1])
            while True:
                line = await asyncio.wait_for(reader.readline(), timeout=10)
                if line in (b"\r\n", b"\n", b""):
                    break
            await self._route(url.path, parse_qs(url.query), writer)
        except (asyncio.TimeoutError, ConnectionError):
            pass
        except Exception:
            log.exception("media request failed")
        finally:
            try:
                await writer.drain()
            except (ConnectionError, RuntimeError):
                pass
            writer.close()

    async def _route(self, path: str, query: dict[str, list[str]],
                     writer: asyncio.StreamWriter) -> None:
        if self.auth_token and not self._authorized(query):
            await _send(writer, "401 Unauthorized", b"")
            return
        path = unquote(path).rstrip("/") or "/"
        if path == "/books":
            await _send_text(writer, "".join(f"{_title(p)}\n" for p in self.library.books()))
            return
        if path == "/music":
            await _send_text(writer, "".join(f"{_title(p)}\n" for p in self.library.tracks()))
            return
        if path in OTA_FILES:
            await self._ota_file(path, writer)
            return
        if path.startswith("/books/"):
            await self._book_page(path[len("/books/"):], query, writer)
            return
        if path.startswith("/music/") and path.endswith(".pcm"):
            await self._track_stream(path[len("/music/"):-len(".pcm")], writer, "pcm")
            return
        if path.startswith("/music/") and path.endswith(".adpcm"):
            await self._track_stream(path[len("/music/"):-len(".adpcm")], writer, "adpcm")
            return
        await _send(writer, "404 Not Found", b"")

    def _authorized(self, query: dict[str, list[str]]) -> bool:
        supplied = query.get("token", [""])[0]
        return hmac.compare_digest(supplied.encode("utf-8"), self.auth_token.encode("utf-8"))

    async def _ota_file(self, path: str, writer: asyncio.StreamWriter) -> None:
        """输出 <media>/ota/ 下的固定文件；文件不存在返回 404。

        发布脚本用 mv 原子替换文件：先传 firmware.bin，最后替换 version.json。
        这里按 stat 得到的长度写 Content-Length，之后按块流式输出。
        """
        name, content_type = OTA_FILES[path]
        target = self.library.root / "ota" / name
        if not target.is_file():
            await _send(writer, "404 Not Found", b"")
            return
        size = target.stat().st_size
        writer.write(
            (
                "HTTP/1.1 200 OK\r\n"
                f"Content-Type: {content_type}\r\n"
                f"Content-Length: {size}\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: close\r\n\r\n"
            ).encode("latin-1")
        )
        with target.open("rb") as handle:
            while True:
                chunk = handle.read(OTA_CHUNK)
                if not chunk:
                    break
                writer.write(chunk)
                await writer.drain()
        log.info("served %s (%d bytes)", name, size)

    async def _book_page(self, index_raw: str, query: dict[str, list[str]],
                         writer: asyncio.StreamWriter) -> None:
        try:
            index = int(index_raw)
            page = max(0, int(query.get("page", ["0"])[0]))
            cols = int(query.get("cols", ["20"])[0])
            lines = int(query.get("lines", ["14"])[0])
        except ValueError:
            await _send(writer, "400 Bad Request", b"")
            return
        text = self.library.book_text(index)
        if text is None:
            await _send(writer, "404 Not Found", b"")
            return
        pages = wrap_lines(text, cols, lines)
        if page >= len(pages):
            page = len(pages) - 1
        body = "\n".join(pages[page])
        await _send_text(writer, body, {"X-Page-Count": str(len(pages))})

    async def _track_stream(self, index_raw: str, writer: asyncio.StreamWriter,
                            codec: str) -> None:
        """按 codec 流式输出曲目：pcm 原样转发，adpcm 每 640 采样编成一个 IMA 块。"""
        try:
            index = int(index_raw)
        except ValueError:
            await _send(writer, "400 Bad Request", b"")
            return
        track = self.library.track(index)
        if track is None:
            await _send(writer, "404 Not Found", b"")
            return
        ffmpeg = shutil.which("ffmpeg")
        if not ffmpeg:
            await _send(writer, "503 Service Unavailable", b"ffmpeg missing on relay")
            return

        writer.write(
            b"HTTP/1.1 200 OK\r\n"
            b"Content-Type: application/octet-stream\r\n"
            b"Connection: close\r\n\r\n"
        )
        await writer.drain()
        proc = await asyncio.create_subprocess_exec(
            ffmpeg, "-v", "error", "-i", str(track),
            "-f", "s16le", "-acodec", "pcm_s16le",
            "-ac", "1", "-ar", str(PCM_RATE), "-",
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL,
        )
        assert proc.stdout is not None
        pending = bytearray()
        try:
            while True:
                chunk = await proc.stdout.read(PCM_CHUNK)
                if not chunk:
                    break
                if codec == "pcm":
                    writer.write(chunk)
                else:
                    pending += chunk
                    for frame in take_pcm_frames(pending):
                        writer.write(encode_block(frame))
                await writer.drain()
            if codec == "adpcm":
                # 文件末尾不足一帧的部分也要编码；奇数的悬空字节无法构成采样，丢弃。
                tail = len(pending) - len(pending) % 2
                if tail:
                    writer.write(encode_block(bytes(pending[:tail])))
                    await writer.drain()
        except ConnectionError:
            log.info("client left during %s", track.name)
        finally:
            if proc.returncode is None:
                proc.kill()
            await proc.wait()
        log.info("streamed %s (%s)", track.name, codec)
