"""Build ASR / TTS adapters from config by provider name."""
from __future__ import annotations

import importlib

from .asr.base import ASR
from .config import ProviderConfig
from .tts.base import TTS

# provider name -> "module:Class". Add new adapters here.
ASR_PROVIDERS = {
    "openai_compat": "relay.asr.openai_compat:OpenAICompatASR",
}
TTS_PROVIDERS = {
    "placeholder": "relay.tts.placeholder:PlaceholderTTS",
    "breeze": "relay.tts.breeze:BreezeTTS",
}


def _load(table: dict[str, str], cfg: ProviderConfig, kind: str):
    try:
        target = table[cfg.provider]
    except KeyError:
        raise ValueError(
            f"unknown {kind} provider {cfg.provider!r}; available: {sorted(table)}"
        ) from None
    module_name, cls_name = target.split(":")
    cls = getattr(importlib.import_module(module_name), cls_name)
    return cls(**cfg.options)


def create_asr(cfg: ProviderConfig) -> ASR:
    return _load(ASR_PROVIDERS, cfg, "asr")


def create_tts(cfg: ProviderConfig) -> TTS:
    return _load(TTS_PROVIDERS, cfg, "tts")
