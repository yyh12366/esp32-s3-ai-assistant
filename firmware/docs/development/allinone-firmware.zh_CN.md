**简体中文** · [English](allinone-firmware.md)

# 合并固件（监测工具 + 语音助手）

`APP_VARIANT=allinone` 把所有应用编进一个镜像。开机进入首页（`av_shell.c`）：
**大肥鱼**（按住说话的对讲）、**小说**、**音乐**、**设置**，以及 **监测**（KK 1-bit
硬件监测工具）。前四项在运行时切换，不需要重启。选择「监测」会把开机目标写入 NVS
命名空间 `boot` 的 `target` 键，然后重启进入监测；在监测首页长按 OK 会把首页设为
开机目标并重启回来。

## 按键对照

| 界面 | UP/DOWN 单击 | OK 单击 | OK 长按 | UP 长按 |
| --- | --- | --- | --- | --- |
| 首页 | 移动光标 | 进入该项 | — | — |
| 大肥鱼 | 滚动回复 | 按住说话（按下/松开） | 离线时：回首页 | 回首页（先结束当前回合） |
| 小说 / 音乐 | 翻页 / 切歌 | 打开 / 播放 / 暂停 | 返回上一级；列表页：回首页 | — |
| 设置 | 移动光标 | 执行该项 | 根菜单：回首页 | 根菜单：回首页 |

离开大肥鱼时会挂起：停麦、打断播放、断开中继 WebSocket，挂起期间不触发任何重连定时器，
回到前台才恢复（`av_session.c` 的 `AV_EV_SUSPEND` / `AV_EV_RESUME`）。

## 为什么监测需要重启

监测工具的 `bsp_display_mono2_blit()` 直接写 LCD，语音版的 LVGL 走
`esp_lvgl_port`。两者都通过 `esp_lcd_panel_io_register_event_callbacks()` 注册
`on_color_trans_done`，而 ESP-IDF 的 SPI panel IO 只保存一个回调（后注册的会覆盖
先注册的并打警告）。同时运行会让其中一方的 DMA 完成信号丢失，画面卡死或传输超时。
因此合并版在启动时二选一，同一时刻只有一套渲染在跑。

进入或离开监测会自动重启，用户不需要手动重启。

## 构建与烧录

```sh
cd firmware
source $IDF_PATH/export.sh          # ESP-IDF 5.5.3
./tools/build_firmware.sh           # 默认 allinone
./tools/flash_firmware.sh allinone /dev/ttyACM0
```

`./tools/build_firmware.sh kk_oled` 和 `astrbot_voice` 仍可单独构建，用于排查问题或
只想要其中一个功能。

## 资源占用（ESP32-C3，无 PSRAM）

| 版本 | 应用大小 | 静态 DRAM | 剩余 DRAM |
| --- | --- | --- | --- |
| `kk_oled` | 403 KB | 84 KB (26%) | 237 KB |
| `astrbot_voice` | 3.11 MB | 225 KB (70%) | 96 KB |
| `allinone` | 3.15 MB | 234 KB (73%) | 88 KB |

合并版只比语音单版多约 9 KB 静态 DRAM（KK 的 2.4 KB 显存、状态机和栈）。运行期
真正吃紧的是 Wi-Fi + BLUFI + LVGL + 音频 + 媒体 HTTP 同时活跃时的堆，务必上机看
`av_app.c` 每 30 秒打印的 `heap free/largest/min`，largest 低于 20 KB 就要收敛功能。

## 小说与音乐

两者都走 relay 的媒体 HTTP 服务（`relay/media.py`，默认端口 8766），设备端不做
MP3 解码：

- `/books`、`/books/<i>?page=&cols=&lines=`：TXT 分页，支持 UTF-8/GB18030。
- `/music`、`/music/<i>.pcm`：relay 用 ffmpeg 把音频转成 16kHz/16bit/单声道 PCM 流。

设备端把 PCM 直接喂给对讲用的 `av_audio_feed()`，复用同一套环形缓冲和播放任务，
所以音乐与 TTS 不会抢 I2S；开始录音时会先停掉音乐。

阅读进度（书名索引 + 页码）存在 NVS 的 `astrbot_voice/read_pos`。

## 未执行的真机检查

编译和宿主机测试通过不代表下列项目已验证，需上机确认：

1. 首页菜单与各功能切换在真实面板上的可读性与按键手感。
2. 合并版同时启用 Wi-Fi、BLUFI、LVGL、音频后的最小可用堆。
3. MP3 流式播放的连续性（欠载日志）、暂停/切歌、与对讲互斥。
4. 长 TXT 的分页速度与翻页延迟。
5. 从监测模式切到语音模式（重启后）时 NVS 读取是否稳定。
