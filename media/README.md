# 设备媒体目录

`relay` 启动后会在这里提供两个 HTTP 列表（端口见 `config.yaml` 的 `media.port`，默认 8766）：

| 路径 | 内容 |
| --- | --- |
| `/books` | `books/*.txt` 的文件名（每行一个，顺序即设备列表顺序） |
| `/music` | `music/` 下的音频文件名（`.mp3` `.m4a` `.flac` `.wav` `.ogg`） |

设备端只接收 16kHz/16bit/单声道 PCM，MP3 等格式由 relay 上的 `ffmpeg` 实时转码，
ESP32-C3 不做解码（无 PSRAM，内部 RAM 紧张）。因此 relay 所在机器必须装 `ffmpeg`：

```sh
ffmpeg -version   # 有输出即可
```

放文件后不需要重启 relay，列表是每次请求时扫描目录。TXT 支持 UTF-8（含 BOM）和 GB18030。
