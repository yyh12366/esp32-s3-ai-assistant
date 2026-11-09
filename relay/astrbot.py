"""AstrBot HTTP API client: streaming chat over SSE plus config checks."""
from __future__ import annotations

import json
import logging
from collections.abc import AsyncGenerator, AsyncIterator
from contextlib import aclosing
from dataclasses import dataclass
from typing import Any, Literal

import httpx

from relay.config import AstrBotConfig

log = logging.getLogger(__name__)

# Segment chain types that are not meant for the user (reasoning, tool traces, stats).
_NON_USER_CHAINS = frozenset({"reasoning", "tool_call", "tool_call_result", "agent_stats"})


@dataclass
class ChatEvent:
    kind: Literal["session", "delta", "done", "error"]
    text: str = ""
    session_id: str | None = None


class AstrBotError(Exception):
    """Raised for HTTP errors, AstrBot-level errors and transport failures."""


class _SegState:
    """Per-segment bookkeeping: whether a delta was emitted since the last `complete`."""

    __slots__ = ("emitted",)

    def __init__(self) -> None:
        self.emitted = False


def _error_text(raw: bytes) -> str:
    text = raw.decode("utf-8", "replace").strip()
    try:
        body: Any = json.loads(text)
    except ValueError:
        body = None
    if isinstance(body, dict):
        for key in ("message", "msg", "detail", "data"):
            value = body.get(key)
            if value:
                return str(value)
    return text[:300] or "empty response"


def _translate(payload: dict, seg: _SegState) -> tuple[ChatEvent | None, bool]:
    """Map one AstrBot event payload to (event to yield, whether to stop)."""
    kind = payload.get("type")
    data = payload.get("data")
    chain = payload.get("chain_type")

    if kind == "session_id":
        sid = payload.get("session_id")
        if isinstance(sid, str) and sid:
            return ChatEvent("session", session_id=sid), False
        return None, False

    if kind == "plain":
        if chain in _NON_USER_CHAINS or not isinstance(data, str) or not data:
            return None, False
        seg.emitted = True
        return ChatEvent("delta", text=data), False

    if kind == "complete":
        # Full text of a streamed segment: only emit it if no delta was sent for it.
        if chain in _NON_USER_CHAINS:
            seg.emitted = False
            return None, False
        fallback = not seg.emitted
        seg.emitted = False
        if fallback and isinstance(data, str) and data:
            return ChatEvent("delta", text=data), False
        return None, False

    if kind == "end":
        return ChatEvent("done"), True

    if kind == "error":
        msg = data if isinstance(data, str) and data else str(payload.get("message") or "unknown error")
        return ChatEvent("error", text=msg), True

    return None, False


async def _sse_data(response: httpx.Response) -> AsyncGenerator[str, None]:
    """Yield the joined `data:` payload of each SSE event."""
    data_lines: list[str] = []
    async for line in response.aiter_lines():
        if line == "":
            if data_lines:
                yield "\n".join(data_lines)
                data_lines = []
            continue
        if line.startswith(":"):  # comment / heartbeat
            continue
        field, _, value = line.partition(":")
        if value.startswith(" "):
            value = value[1:]
        if field == "data":
            data_lines.append(value)
    if data_lines:
        yield "\n".join(data_lines)


class AstrBotClient:
    def __init__(self, cfg: AstrBotConfig, transport: httpx.AsyncBaseTransport | None = None):
        self._cfg = cfg
        self._client = httpx.AsyncClient(
            base_url=cfg.base_url,
            headers={"Authorization": f"Bearer {cfg.api_key}"},
            timeout=httpx.Timeout(cfg.request_timeout, connect=10.0),
            transport=transport,
        )

    async def chat(self, username: str, text: str, session_id: str | None) -> AsyncIterator[ChatEvent]:
        """Send one message and yield session/delta/done/error events as they stream in."""
        body: dict[str, Any] = {
            "username": username,
            "message": text,
            "enable_streaming": True,
            # voice: no inline GenUI widgets, no reasoning output (also lowers latency)
            "flags": {
                "enable_streaming": True,
                "enable_inline_genui": False,
                "enable_reasoning": False,
            },
        }
        if session_id is not None:
            body["session_id"] = session_id
        if self._cfg.config_id:
            body["config_id"] = self._cfg.config_id
        elif self._cfg.config_name:
            body["config_name"] = self._cfg.config_name

        seg = _SegState()
        try:
            async with self._client.stream(
                "POST",
                "/api/v1/chat",
                json=body,
                headers={"Accept": "text/event-stream"},
            ) as resp:
                ctype = resp.headers.get("content-type", "")
                if not (200 <= resp.status_code < 300) or "text/event-stream" not in ctype:
                    raw = await resp.aread()
                    raise AstrBotError(f"HTTP {resp.status_code}: {_error_text(raw)}")

                stopped = False
                async with aclosing(_sse_data(resp)) as events:
                    async for raw_data in events:
                        try:
                            payload = json.loads(raw_data)
                        except ValueError:
                            log.debug("skipping non-JSON SSE data: %r", raw_data[:200])
                            continue
                        if not isinstance(payload, dict):
                            continue
                        event, stop = _translate(payload, seg)
                        if event is not None:
                            yield event
                        if stop:
                            stopped = True
                            break
                if not stopped:
                    log.debug("AstrBot stream closed without end event")
                    yield ChatEvent("done")
        except httpx.HTTPError as exc:
            raise AstrBotError(f"request failed: {exc}") from exc

    async def list_configs(self) -> list[dict]:
        try:
            resp = await self._client.get("/api/v1/configs")
        except httpx.HTTPError as exc:
            raise AstrBotError(f"request failed: {exc}") from exc
        try:
            body: Any = resp.json()
        except ValueError:
            body = None
        if not (200 <= resp.status_code < 300) or not isinstance(body, dict) or body.get("status") != "ok":
            msg = _error_text(resp.content)
            raise AstrBotError(f"HTTP {resp.status_code}: {msg}")
        data = body.get("data")
        if not isinstance(data, list):
            return []
        return [item for item in data if isinstance(item, dict)]

    async def check_config(self) -> None:
        """Log whether the configured config_id/config_name resolves. Never raises."""
        cfg = self._cfg
        if not cfg.config_id and not cfg.config_name:
            log.info("AstrBot: no config_id/config_name set, using default config routing")
            return

        try:
            configs = await self.list_configs()
        except AstrBotError as exc:
            log.warning("AstrBot: cannot verify config (%s); continuing without check", exc)
            return

        available = ", ".join(f"{c.get('id')} ({c.get('name')})" for c in configs) or "none"
        ok = True

        if cfg.config_id:
            match = [c for c in configs if str(c.get("id")) == cfg.config_id]
            if not match:
                log.error("AstrBot: config_id %r not found; available: %s", cfg.config_id, available)
                ok = False

        if cfg.config_name:
            matches = [c for c in configs if c.get("name") == cfg.config_name]
            if len(matches) != 1:
                log.error(
                    "AstrBot: config_name %r matched %d entries (need exactly 1); available: %s",
                    cfg.config_name,
                    len(matches),
                    available,
                )
                ok = False

        if ok:
            if cfg.config_id:
                name = next(c.get("name") for c in configs if str(c.get("id")) == cfg.config_id)
                log.info("AstrBot: using config id=%s name=%s", cfg.config_id, name)
            else:
                cid = next(c.get("id") for c in configs if c.get("name") == cfg.config_name)
                log.info("AstrBot: using config name=%s id=%s", cfg.config_name, cid)

    async def aclose(self) -> None:
        await self._client.aclose()
