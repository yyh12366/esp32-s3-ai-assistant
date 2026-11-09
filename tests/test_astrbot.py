"""Tests for the AstrBot HTTP/SSE client."""
from __future__ import annotations

import json

import httpx
import pytest

from relay.astrbot import AstrBotClient, AstrBotError, ChatEvent
from relay.config import AstrBotConfig


def sse(*payloads: dict | str) -> bytes:
    """Build an SSE body; str items are emitted verbatim as comment/raw lines."""
    parts: list[str] = []
    for p in payloads:
        if isinstance(p, str):
            parts.append(p + "\n")
        else:
            parts.append(f"data: {json.dumps(p, ensure_ascii=False)}\n\n")
    return "".join(parts).encode("utf-8")


def sse_response(body: bytes, status: int = 200) -> httpx.Response:
    return httpx.Response(status, headers={"content-type": "text/event-stream"}, content=body)


def make_client(handler, **cfg_overrides) -> AstrBotClient:
    cfg = AstrBotConfig(base_url="http://astrbot.test", api_key="k", **cfg_overrides)
    return AstrBotClient(cfg, transport=httpx.MockTransport(handler))


async def collect(client: AstrBotClient, session_id: str | None = None) -> list[ChatEvent]:
    return [ev async for ev in client.chat("alice", "hi", session_id)]


def plain(data: str, chain: str | None = None, streaming: bool = True) -> dict:
    return {"type": "plain", "data": data, "streaming": streaming, "chain_type": chain, "message_id": "m"}


def complete(data: str) -> dict:
    return {"type": "complete", "data": data, "streaming": True, "chain_type": None, "message_id": "m"}


END = {"type": "end", "data": None}


async def test_session_and_deltas_then_done():
    body = sse(
        {"type": "session_id", "data": None, "session_id": "s-1"},
        plain("你好"),
        plain("世界"),
        END,
    )
    client = make_client(lambda req: sse_response(body))
    events = await collect(client)
    assert events == [
        ChatEvent("session", session_id="s-1"),
        ChatEvent("delta", text="你好"),
        ChatEvent("delta", text="世界"),
        ChatEvent("done"),
    ]
    await client.aclose()


async def test_heartbeat_comments_ignored():
    body = sse(": heartbeat", plain("a"), ": heartbeat", ": heartbeat", END)
    client = make_client(lambda req: sse_response(body))
    events = await collect(client)
    assert [e.kind for e in events] == ["delta", "done"]
    await client.aclose()


async def test_reasoning_and_tool_chains_ignored():
    body = sse(
        plain("thinking...", chain="reasoning"),
        plain("calling", chain="tool_call"),
        plain("result", chain="tool_call_result"),
        plain("stats", chain="agent_stats"),
        plain("answer"),
        END,
    )
    client = make_client(lambda req: sse_response(body))
    events = await collect(client)
    assert events == [ChatEvent("delta", text="answer"), ChatEvent("done")]
    await client.aclose()


async def test_complete_not_duplicated_after_deltas():
    body = sse(plain("你"), plain("好"), complete("你好"), END)
    client = make_client(lambda req: sse_response(body))
    events = await collect(client)
    assert [e.text for e in events if e.kind == "delta"] == ["你", "好"]
    await client.aclose()


async def test_complete_fallback_when_no_deltas():
    body = sse(complete("完整句子"), END)
    client = make_client(lambda req: sse_response(body))
    events = await collect(client)
    assert events == [ChatEvent("delta", text="完整句子"), ChatEvent("done")]
    await client.aclose()


async def test_multi_segment_no_duplication_per_segment():
    body = sse(
        plain("第一段"),
        complete("第一段"),
        plain("工具", chain="tool_call"),
        plain("第二"),
        plain("段"),
        complete("第二段"),
        END,
    )
    client = make_client(lambda req: sse_response(body))
    events = await collect(client)
    assert [e.text for e in events if e.kind == "delta"] == ["第一段", "第二", "段"]
    assert events[-1] == ChatEvent("done")
    await client.aclose()


async def test_multi_segment_fallback_for_second_segment_only():
    # First segment streamed with deltas, second has only a complete -> fallback emits it.
    body = sse(plain("A"), complete("A"), complete("B"), END)
    client = make_client(lambda req: sse_response(body))
    events = await collect(client)
    assert [e.text for e in events if e.kind == "delta"] == ["A", "B"]
    await client.aclose()


async def test_non_str_data_ignored():
    body = sse({"type": "plain", "data": {"x": 1}, "streaming": True}, END)
    client = make_client(lambda req: sse_response(body))
    events = await collect(client)
    assert events == [ChatEvent("done")]
    await client.aclose()


async def test_unknown_types_ignored():
    body = sse(
        {"type": "user_message_saved", "data": {}},
        {"type": "run_started", "data": None},
        {"type": "image", "data": "http://x/y.png"},
        {"type": "message_saved", "data": None},
        plain("ok"),
        END,
    )
    client = make_client(lambda req: sse_response(body))
    events = await collect(client)
    assert events == [ChatEvent("delta", text="ok"), ChatEvent("done")]
    await client.aclose()


async def test_error_event_stops_stream():
    body = sse(plain("partial"), {"type": "error", "data": "model overloaded"}, plain("never"))
    client = make_client(lambda req: sse_response(body))
    events = await collect(client)
    assert events == [ChatEvent("delta", text="partial"), ChatEvent("error", text="model overloaded")]
    await client.aclose()


async def test_json_error_body_with_http_200_raises():
    def handler(req):
        return httpx.Response(
            200,
            headers={"content-type": "application/json"},
            content=b'{"status":"error","message":"config_id not found: x"}',
        )

    client = make_client(handler)
    with pytest.raises(AstrBotError, match="config_id not found: x"):
        await collect(client)
    await client.aclose()


async def test_http_401_raises_with_message():
    def handler(req):
        return httpx.Response(
            401,
            headers={"content-type": "application/json"},
            content=b'{"status":"error","message":"invalid api key"}',
        )

    client = make_client(handler)
    with pytest.raises(AstrBotError, match=r"HTTP 401: invalid api key"):
        await collect(client)
    await client.aclose()


async def test_http_error_plain_text_body():
    client = make_client(lambda req: httpx.Response(502, content=b"bad gateway"))
    with pytest.raises(AstrBotError, match=r"HTTP 502: bad gateway"):
        await collect(client)
    await client.aclose()


async def test_transport_error_wrapped():
    def handler(req):
        raise httpx.ConnectError("refused", request=req)

    client = make_client(handler)
    with pytest.raises(AstrBotError):
        await collect(client)
    await client.aclose()


async def test_stream_closing_without_end_yields_done():
    body = sse(plain("hello"))
    client = make_client(lambda req: sse_response(body))
    events = await collect(client)
    assert events == [ChatEvent("delta", text="hello"), ChatEvent("done")]
    await client.aclose()


async def test_request_body_session_and_config_name():
    seen: dict = {}

    def handler(req: httpx.Request):
        seen["path"] = req.url.path
        seen["body"] = json.loads(req.content)
        seen["accept"] = req.headers.get("accept")
        seen["auth"] = req.headers.get("authorization")
        return sse_response(sse(END))

    client = make_client(handler, config_name="default-voice")
    await collect(client, session_id="sess-9")
    assert seen["path"] == "/api/v1/chat"
    assert seen["accept"] == "text/event-stream"
    assert seen["auth"] == "Bearer k"
    assert seen["body"] == {
        "username": "alice",
        "message": "hi",
        "enable_streaming": True,
        "session_id": "sess-9",
        "config_name": "default-voice",
        "flags": {
            "enable_streaming": True,
            "enable_inline_genui": False,
            "enable_reasoning": False,
        },
    }
    await client.aclose()


async def test_request_body_config_id_wins_and_no_session_key():
    seen: dict = {}

    def handler(req: httpx.Request):
        seen["body"] = json.loads(req.content)
        return sse_response(sse(END))

    client = make_client(handler, config_id="cfg-1", config_name="ignored")
    await collect(client, session_id=None)
    assert seen["body"]["config_id"] == "cfg-1"
    assert "config_name" not in seen["body"]
    assert "session_id" not in seen["body"]
    await client.aclose()


async def test_list_configs_returns_data():
    def handler(req: httpx.Request):
        assert req.url.path == "/api/v1/configs"
        return httpx.Response(
            200,
            headers={"content-type": "application/json"},
            content=json.dumps(
                {"status": "ok", "data": [{"id": "a", "name": "A", "path": "p", "is_default": True}]}
            ).encode(),
        )

    client = make_client(handler)
    assert await client.list_configs() == [{"id": "a", "name": "A", "path": "p", "is_default": True}]
    await client.aclose()


async def test_list_configs_error_raises():
    client = make_client(lambda req: httpx.Response(403, content=b'{"status":"error","message":"no scope"}'))
    with pytest.raises(AstrBotError, match="no scope"):
        await client.list_configs()
    await client.aclose()


async def test_check_config_default_routing_makes_no_request():
    calls: list[str] = []

    def handler(req):
        calls.append(req.url.path)
        return httpx.Response(500)

    client = make_client(handler)
    await client.check_config()
    assert calls == []
    await client.aclose()


async def test_check_config_unknown_id_does_not_raise(caplog):
    def handler(req):
        return httpx.Response(
            200,
            headers={"content-type": "application/json"},
            content=json.dumps({"status": "ok", "data": [{"id": "a", "name": "A"}]}).encode(),
        )

    client = make_client(handler, config_id="zzz")
    with caplog.at_level("ERROR"):
        await client.check_config()
    assert "zzz" in caplog.text and "a (A)" in caplog.text
    await client.aclose()


async def test_check_config_403_warns_and_does_not_raise(caplog):
    client = make_client(
        lambda req: httpx.Response(403, headers={"content-type": "application/json"}, content=b'{"status":"error","message":"forbidden"}'),
        config_name="x",
    )
    with caplog.at_level("WARNING"):
        await client.check_config()
    assert "cannot verify" in caplog.text
    await client.aclose()
