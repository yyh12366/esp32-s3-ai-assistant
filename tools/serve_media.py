#!/usr/bin/env python3
"""独立启动媒体 HTTP 服务（TXT 书单/分页 + MP3→PCM 流）。

用法：python tools/serve_media.py [--host 0.0.0.0] [--port 8766] [--root ./media]
"""
from __future__ import annotations

import argparse
import asyncio
import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from relay.config import MediaConfig
from relay.media import MediaLibrary, MediaServer


async def amain(cfg: MediaConfig) -> None:
    server = MediaServer(cfg, MediaLibrary(cfg.root))
    await server.start()
    await asyncio.Event().wait()


def main() -> None:
    parser = argparse.ArgumentParser(prog="serve-media")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8766)
    parser.add_argument("--root", default=str(Path(__file__).resolve().parents[1] / "media"))
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    try:
        asyncio.run(amain(MediaConfig(host=args.host, port=args.port, root=args.root)))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
