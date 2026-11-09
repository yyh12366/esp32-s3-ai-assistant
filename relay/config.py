"""Load config.yaml into typed settings."""
from __future__ import annotations

import os
import re
from dataclasses import dataclass, field, fields
from pathlib import Path
from typing import Any

import yaml


@dataclass
class ServerConfig:
    host: str = "0.0.0.0"
    port: int = 8765
    path: str = "/ws"
    auth_token: str = ""


@dataclass
class AudioConfig:
    sample_rate: int = 16000
    frame_ms: int = 40
    prebuffer_ms: int = 400
    max_listen_seconds: int = 30
    downlink_codec: str = "ima_adpcm"


@dataclass
class AstrBotConfig:
    base_url: str = "http://127.0.0.1:6185"
    api_key: str = ""
    config_id: str = ""
    config_name: str = ""
    username_prefix: str = ""
    request_timeout: float = 120
    session_store: str = "./data/sessions.json"


@dataclass
class TextConfig:
    min_chars: int = 4
    max_chars: int = 60


@dataclass
class MediaConfig:
    """TXT/MP3 service the device reads over HTTP (separate port from the WS relay)."""

    host: str = "0.0.0.0"
    port: int = 8766
    root: str = "./media"


@dataclass
class ProviderConfig:
    """`provider` picks the adapter; `options` is that adapter's own sub-section."""

    provider: str
    options: dict[str, Any] = field(default_factory=dict)


@dataclass
class Config:
    server: ServerConfig
    audio: AudioConfig
    astrbot: AstrBotConfig
    text: TextConfig
    media: MediaConfig
    asr: ProviderConfig
    tts: ProviderConfig
    log_level: str = "INFO"


def _build(cls, raw: dict | None):
    raw = raw or {}
    known = {f.name for f in fields(cls)}
    unknown = set(raw) - known
    if unknown:
        raise ValueError(f"{cls.__name__}: unknown keys {sorted(unknown)}")
    return cls(**raw)


def _provider(raw: dict | None, default: str) -> ProviderConfig:
    raw = dict(raw or {})
    name = raw.pop("provider", default)
    return ProviderConfig(provider=name, options=dict(raw.get(name) or {}))


_ENV_RE = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}")


def _expand_env(value: Any) -> Any:
    """Replace ${VAR} in string values with environment variables (secrets stay out of the file)."""
    if isinstance(value, str):
        return _ENV_RE.sub(lambda m: os.environ.get(m.group(1), ""), value)
    if isinstance(value, dict):
        return {k: _expand_env(v) for k, v in value.items()}
    if isinstance(value, list):
        return [_expand_env(v) for v in value]
    return value


def load_config(path: str | Path) -> Config:
    data = _expand_env(yaml.safe_load(Path(path).read_text(encoding="utf-8")) or {})
    return Config(
        server=_build(ServerConfig, data.get("server")),
        audio=_build(AudioConfig, data.get("audio")),
        astrbot=_build(AstrBotConfig, data.get("astrbot")),
        text=_build(TextConfig, data.get("text")),
        media=_build(MediaConfig, data.get("media")),
        asr=_provider(data.get("asr"), "openai_compat"),
        tts=_provider(data.get("tts"), "placeholder"),
        log_level=str((data.get("logging") or {}).get("level", "INFO")),
    )
