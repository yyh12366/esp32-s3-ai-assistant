"""Entry point: python -m relay -c config.yaml"""
from __future__ import annotations

import argparse
import asyncio
import logging
import signal

from websockets.asyncio.server import serve

from .astrbot import AstrBotClient
from .config import load_config
from .media import MediaLibrary, MediaServer
from .registry import create_asr, create_tts
from .server import Relay


async def amain(config_path: str) -> None:
    cfg = load_config(config_path)
    logging.getLogger().setLevel(cfg.log_level.upper())
    log = logging.getLogger("relay")

    asr = create_asr(cfg.asr)
    tts = create_tts(cfg.tts)
    astrbot = AstrBotClient(cfg.astrbot)
    log.info("ASR=%s TTS=%s AstrBot=%s", cfg.asr.provider, cfg.tts.provider, cfg.astrbot.base_url)
    await astrbot.check_config()

    relay = Relay(cfg, asr, tts, astrbot)
    media = MediaServer(cfg.media, MediaLibrary(cfg.media.root), cfg.server.auth_token)
    stop = asyncio.get_running_loop().create_future()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            asyncio.get_running_loop().add_signal_handler(sig, stop.cancel)
        except NotImplementedError:  # Windows
            pass

    try:
        await media.start()
        async with serve(
            relay.handler,
            cfg.server.host,
            cfg.server.port,
            max_size=1 << 20,
            ping_interval=20,
            ping_timeout=20,
            compression=None,  # PCM doesn't compress; save device CPU
        ):
            log.info("listening on ws://%s:%d%s", cfg.server.host, cfg.server.port, cfg.server.path)
            try:
                await stop
            except asyncio.CancelledError:
                log.info("shutting down")
    finally:
        await media.stop()
        await asr.aclose()
        await tts.aclose()
        await astrbot.aclose()


def main() -> None:
    parser = argparse.ArgumentParser(prog="astrbot-relay")
    parser.add_argument("-c", "--config", default="config.yaml")
    args = parser.parse_args()
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    asyncio.run(amain(args.config))


if __name__ == "__main__":
    main()
