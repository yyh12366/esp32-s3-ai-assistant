<p align="right"><a href="README.md">English</a></p>

# ESP32-S3 AI Assistant

ESP32-S3 N16R8 语音助手项目，包含 ESP-IDF 固件、Python 中继服务和基于 SDL/LVGL 的桌面 UI 预览。

## 项目结构

| 路径 | 用途 |
| --- | --- |
| `firmware/` | ESP32-S3 开发板的 ESP-IDF 固件 |
| `relay/` | 处理 ASR、AstrBot 和 TTS 的 Python WebSocket 中继 |
| `firmware/tools/ui_preview/` | 复用固件 LVGL 界面的桌面预览 |
| `media/` | 提供给设备或预览程序的本地书籍与音乐 |
| `tests/` | Python 中继单元测试 |

## ESP32-S3 硬件与固件

S3 目标适配 ILI9341 屏幕、ES8311 音频编解码器和 NS4150B 功放，面向 N16R8 模组。

| 连接 | GPIO 引脚 |
| --- | --- |
| ILI9341 CS / DC / RST / SCK / MOSI | 10 / 4 / 5 / 12 / 11 |
| ES8311 I2C SDA / SCL | 8 / 9 |
| ES8311 I2S MCLK / BCLK / WS / DOUT / DIN | 16 / 17 / 15 / 7 / 18 |

开发板通过 5 V 输入供电，所有设备共地。接线或上电前，先核实开发板和每个模块的输入电压、电流与极性；GPIO 信号不能接入 5 V。

激活 ESP-IDF 5.5.3 后，在 `firmware/` 目录构建：

```sh
cd firmware
./tools/build_firmware.sh esp32s3
```

合并后的镜像输出到 `firmware/build/esp32s3/FoloToy-AI-Passport-full.bin`。需要烧录并打开串口监视器时，请自行连接开发板后，在 `firmware/` 目录执行：

```sh
idf.py -B build/esp32s3 flash monitor
```

默认中继地址为空。可在 `menuconfig` 中设置，或通过设备设置/BLUFI 配网写入自己的中继地址，例如 `relay=ws://<relay-host>:8765/ws`。S3 固件已通过构建和布局检查，尚未在实体开发板上验证。

## Python 中继

需要 Python 3.11 或更高版本。在仓库根目录执行：

```sh
python -m venv .venv
. .venv/bin/activate
python -m pip install -e '.[dev]'
cp .env.example .env
cp config.example.yaml config.yaml
```

在 `.env` 中填写自己的 `ASTRBOT_API_KEY`、`GROQ_API_KEY`、`BREEZE_API_KEY` 和 `BREEZE_VOICE_ID`；在 `config.yaml` 中设置 AstrBot 地址和中继选项。若为 ASR 配置 HTTP 代理，请额外执行 `python -m pip install -e '.[proxy]'` 并在 `config.yaml` 中设置代理。

加载环境变量并启动中继：

```sh
set -a
. ./.env
set +a
python -m relay --config config.yaml
```

中继默认也会在 8766 端口提供媒体服务。只启动媒体服务器（例如供桌面预览使用）时执行：

```sh
python tools/serve_media.py --host 127.0.0.1 --root ./media
```

将个人书籍和音乐放在 `media/books/` 与 `media/music/` 中。这些文件会被 Git 忽略；仓库只保留目录标记和说明文档。

## 桌面 UI 预览

预览需要 CMake、C 编译器、SDL2、libcurl，以及已获取的 LVGL 组件 `firmware/managed_components/lvgl__lvgl`。

```sh
cmake -S firmware/tools/ui_preview -B build/ui-preview
cmake --build build/ui-preview
build/ui-preview/ui_preview
```

默认从 `http://127.0.0.1:8766` 读取媒体。可用 `AV_MEDIA_BASE_URL` 设置其他媒体基址；媒体服务需要令牌时设置 `AV_MEDIA_TOKEN`。书籍和音乐页面会读取媒体服务提供的真实内容。按 Up/Down 选择条目或翻页，Enter 打开书籍或播放/暂停音乐，Esc 返回。

桌面预览不支持语音 WebSocket、Wi-Fi 或视频播放。它用于预览界面，不是完整的设备模拟器。

仓库中的中继单元测试可通过 `pytest` 运行。此公开副本不包含固件测试目录。

## 许可与媒体

仓库根目录采用 MIT 许可；firmware 目录保留原有的 MIT 许可和署名。第三方组件和字体保留各自的许可声明。提供角色素材的用户已允许将其随本项目公开分发；此说明不主张对第三方权利的所有权。

请勿向仓库添加私有凭据、设备令牌、个人媒体或运行日志。
