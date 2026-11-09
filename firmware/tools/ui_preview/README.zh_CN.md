# 桌面 UI 预览

English: [README.md](README.md)

此桌面程序复用固件的 LVGL UI、中文字库、图片和 KK 渲染器。小说与音乐页面读取现有媒体 HTTP 服务；它不会连接语音 WebSocket 中继。

## 构建和运行

在仓库根目录执行：

```sh
cmake -S firmware/tools/ui_preview -B /tmp/astr2esp32-ui-preview
cmake --build /tmp/astr2esp32-ui-preview
/tmp/astr2esp32-ui-preview/ui_preview
```

默认媒体服务地址为 `http://127.0.0.1:8766`。可通过 `AV_MEDIA_BASE_URL` 指定其他媒体基址；服务需要鉴权时，在启动预览的 shell 中设置 `AV_MEDIA_TOKEN`。请求会对 token 做 URL 编码，预览程序不会记录或输出 token。

构建需要 CMake、C 编译器、SDL2、libcurl，以及已有的 `firmware/managed_components/lvgl__lvgl` 目录。

按键：`Up`/`Down` 选择条目或翻页；`Enter` 打开小说或播放歌曲，在播放页暂停/继续；`Esc` 返回上一级，离开播放页时停止播放。

## 检查和截图

```sh
/tmp/astr2esp32-ui-preview/ui_preview --check
/tmp/astr2esp32-ui-preview/ui_preview --check-audio
/tmp/astr2esp32-ui-preview/ui_preview --screenshots /tmp/astr2esp32-ui-shots
```

`--check` 会读取真实书单、歌单和第一页正文，检查 PCM 分块处理，并确认拒绝连接会显示为错误。它不会播放音频。截图模式读取当前 HTTP 响应，不会启动窗口或播放音频。

`--check-audio` 会把第一首真实曲目流式送入 SDL 虚拟音频设备，暂停四秒、恢复、取消并检查队列；不会产生可听声音。

## 支持范围

预览暂不支持语音/WebSocket、Wi-Fi 配网和视频播放。语音页面会标明这是电脑预览。监测页显示桌面运行时间，硬件读数显示未知；媒体服务可连接不代表语音中继已连接。
