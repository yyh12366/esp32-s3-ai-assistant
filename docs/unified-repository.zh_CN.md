**简体中文** · [English](unified-repository.md)

# 统一 relay 与固件代码仓库

仓库根目录保留原来的 Python relay，在 `firmware/` 下放置一套 ESP-IDF 工程。
三个构建目标共用 BSP：

- `allinone`（默认）：一个固件同时含两套应用。开机出现选择界面，选择结果存 NVS，
  3 秒不操作自动进入上次的选择。
- `kk_oled`：只含监测工具。
- `astrbot_voice`：只含语音应用。

监测工具的 1-bit 直写和语音应用的 LVGL 会注册同一个 LCD 完成回调，无法同时渲染，
所以合并版在开机时二选一，而不是并行运行。

| 部分 | 源码 | 功能 |
| --- | --- | --- |
| relay | `relay/` | WebSocket、ASR、AstrBot、TTS 和音频节流 |
| relay 媒体 | `relay/media.py` | 给设备的 TXT 分页与 MP3→16kHz PCM HTTP 服务 |
| KK OLED UI | `firmware/main/kk_*.c` | 单色显存、XOR / 阻尼界面、NVS 设置及硬件监测 |
| AstrBot 语音 | `firmware/main/av_*.c` | 按键对讲、蓝牙配网、语音播放、小说、音乐 |
| 开机选择器 | `firmware/main/kk_boot.c` | 启动时选择监测或语音，结果存 NVS |
| 板级驱动 | `firmware/components/bsp/` | 共用引脚、总线、显示、按键、codec 及电池驱动 |

relay 仍从仓库根目录通过 `python -m relay -c config.yaml` 启动，现有 Python
包和部署入口保持可用。在虚拟环境中执行
`python -m pip install -e '.[dev]'` 安装开发依赖。

固件构建需要 ESP-IDF 5.5.3。使用说明见
[合并固件](../firmware/docs/development/allinone-firmware.md)、[KK OLED UI](../firmware/docs/development/kk-oled-ui.zh_CN.md)、
[AstrBot 语音](../firmware/docs/development/astrbot-voice.zh_CN.md) 以及
[固件开发规范](../firmware/AGENTS.zh_CN.md)。

构建镜像（默认 `allinone`）：

```sh
cd firmware
./tools/build_firmware.sh              # 合并版
./tools/build_firmware.sh kk_oled
./tools/build_firmware.sh astrbot_voice
```

合并镜像输出到 `firmware/build/<variant>/FoloToy-AI-Passport-full.bin`。
烧录用 `./tools/flash_firmware.sh allinone /dev/ttyACM0`，或
`idf.py -p <port> write_flash 0x0 <镜像>`。
`firmware/tools/validate.sh --static` 运行两个应用的宿主机测试；
`firmware/tools/validate.sh --firmware` 构建并校验全部三个版本。

## 代码来源

以 relay 仓库原有历史为基础，固件从当前本地工作区复制，原工作区保持原样：

- KK UI：`ai-passport` 的 `feature/kk-oled-ui`，基于 `488beda41309f052d81f08d0016d65a3fd85eb7e`，包含原生点阵字体优化。
- 语音：`ai-passport` 的 `feature/astrbot-voice`，`0dd073b`，包含本地 BLUFI 堆诊断和蓝牙缓冲调整。
- relay：`main`，`63f2a69`，包含本地 README 和登录启动项模板修改。

保留固件的工程文档、工具、组件锁、资源、许可证和宿主机测试。本次是源码归拢，
不合并独立 Git 历史，不创建 commit、push 或发布。

## 本地配置与数据

运行用 `.env`、`config.yaml`、会话、录音、固件私有配置、生成的二进制和虚拟环境
均排除。请由受版本管理的示例创建本地配置。正在运行的 relay 和私有文件继续保留
在原路径，除非明确执行运行环境迁移。

两个固件使用各自的 NVS 配置命名空间。重新构建不会改变设备；从 `0x0` 写入合并
镜像可能重置 NVS，需要保留配置时使用兼容的分段烧录。整理源码不授权烧录，也不
授权启动或替换已经部署的服务。
