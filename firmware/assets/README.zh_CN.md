<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 资源目录（Assets）

本目录集中存放可复用的资源（字库、图片、音乐、电池参数等），按资源类型分子目录管理。每个资源放在其类型对应的子目录，并记录放置路径、命名方式、集成方式与来源/许可。二进制资源（字体、图片、音频）不属于纯 markdown 文档，请勿与文档混放。涉及版权/授权的资源需注明来源与许可。

## 字库（fonts）

可复用的字库文件与生成的字库源码放在 `fonts/`。

- 命名要能反映字族、字重、字级与格式。
- 记录来源、许可、字符范围、转换命令与目标放置路径。
- 添加字库前评估 Flash 与内部 RAM 影响；ESP32-C3 无 PSRAM。
- 不提交许可不允许分发的字库。

### KK OLED 1 位 UI 字库

[`fonts/kk_font_data.h`](fonts/kk_font_data.h) 是生成后的渲染字库表，共 185 个字形，覆盖可打印 ASCII 以及 `main/kk_render_texts.c` 中的全部码点。字形来自 Fusion Pixel Font 12 px 等宽简体中文版 `zh_hans`，版本为 2026.10.25。直接按原生像素网格复制 BDF 数据，不做缩放或灰度阈值转换。汉字使用 12 × 12 单元；源字体的 6 px ASCII 居中放入既有 8 × 12 单元，保持渲染器的字符宽度。字库约占 5.8 KiB 只读数据，不分配运行时字库。

源文件为 [`fusion-pixel-12px-monospaced-zh_hans.bdf`](https://github.com/TakWolf/fusion-pixel-font/releases/tag/2026.10.25)，位于[官方 2026.10.25 发布压缩包](https://github.com/TakWolf/fusion-pixel-font/releases/download/2026.10.25/fusion-pixel-font-12px-monospaced-bdf-v2026.10.25.zip)。压缩包 SHA-256：`d8788cf5ffd6a2dabec397c33c878261195fb9146cdb67d8afd418fe73db15bd`；提取后 BDF SHA-256：`bdb520006cddacccc1d3f780d5fab79007a4d6d1f3559d04665e652af4cdf50f`。字体采用 SIL Open Font License 1.1，版权与完整许可见 [`fonts/OFL.txt`](fonts/OFL.txt)。

同时保留发布包中 [Ark Pixel](fonts/licenses/ark-pixel-OFL.txt)、[Cubic 11](fonts/licenses/cubic-11-OFL.txt) 和 [Galmuri](fonts/licenses/galmuri-LICENSE.txt) 的组成字体声明。

使用 Python 3 标准库和 `unzip` 重新生成：

```sh
curl -fsSL 'https://github.com/TakWolf/fusion-pixel-font/releases/download/2026.10.25/fusion-pixel-font-12px-monospaced-bdf-v2026.10.25.zip' -o /tmp/fusion-pixel-12px-bdf.zip
unzip -p /tmp/fusion-pixel-12px-bdf.zip fusion-pixel-12px-monospaced-zh_hans.bdf > /tmp/fusion-pixel-12px-monospaced-zh_hans.bdf
unzip -p /tmp/fusion-pixel-12px-bdf.zip OFL.txt > assets/fonts/OFL.txt
python3 tools/generate_kk_font.py --font /tmp/fusion-pixel-12px-monospaced-zh_hans.bdf
python3 tools/generate_kk_font.py --font /tmp/fusion-pixel-12px-monospaced-zh_hans.bdf --check
```

生成器校验固定 BDF 的摘要和度量、全部 UI 码点覆盖、像素边界以及序列化输出。仓库不保存 5.3 MB 的源 BDF，只嵌入 UI 固定字集。

| 文件 | 内容 | 来源、许可与生成方式 |
| --- | --- | --- |
| [`fonts/av_font_cjk_16.c`](fonts/av_font_cjk_16.c) | LVGL 16 px、4 bpp、不压缩；7544 个字形：可打印 ASCII、全部 GB2312（符号区与 6763 个汉字）以及少量补充标点；约占 1 MB Flash | [Noto Sans SC Regular](https://github.com/notofonts/noto-cjk)（`Sans/SubsetOTF/SC/NotoSansSC-Regular.otf`，SHA-256 `faa6c9df652116dde789d351359f3d7e5d2285a2b2a1f04a2d7244df706d5ea9`）的子集，SIL Open Font License 1.1，完整许可文本见 [`fonts/licenses/noto-sans-sc-OFL.txt`](fonts/licenses/noto-sans-sc-OFL.txt)。用 `python3 tools/gen_av_font.py`（`lv_font_conv@1.5.3`）重新生成；源 OTF 下载到缓存目录，不提交。由 `main/av_ui.c` 使用，Montserrat 14 作为 LVGL 图标的兜底字体。 |
| [`fonts/av_font_cjk_16.charset.txt`](fonts/av_font_cjk_16.charset.txt) | 上面字体的准确字符清单 | 由 `tools/gen_av_font.py` 写出；`tests/test_av_font.py` 用它核对字体。 |

## 图片（images）

可复用的源图与生成的显示资产放在 `images/`。

| 文件 | 尺寸与格式 | 用途与来源 |
| --- | --- | --- |
| [`images/voice_character_source.jpg`](images/voice_character_source.jpg) | 640 × 640，JPEG | 用户为对讲应用主界面提供的角色图。用户已允许将其随本项目公开分发；此说明不主张对第三方权利的所有权。 |
| [`images/voice_character.png`](images/voice_character.png)、[`images/av_voice_character.c`](images/av_voice_character.c) | 128 × 128 PNG RGB 及 LVGL 9 RGB565 | 早期白底版本，保留为源图变体。 |
| [`images/voice_character_cutout_source.png`](images/voice_character_cutout_source.png) | 1254 × 1254，PNG RGBA | 使用内置图像生成工具为用户提供的角色抠出背景，保留白色衣服和头饰。生成式编辑对部分线条和服装细节有改动。 |
| [`images/voice_character_cutout.png`](images/voice_character_cutout.png)、[`images/av_voice_character_cutout.c`](images/av_voice_character_cutout.c) | 128 × 128 PNG RGBA 及 LVGL 9 RGB565A8，像素数据 48 KiB | 早期透明角色，保留为表情图的参考。 |
| [`images/voice_character_float.gif`](images/voice_character_float.gif) | 136 × 136，透明 GIF，2.4 秒循环 | 早期浮动版本，使用 Pillow 执行 `python3 tools/gen_av_character_gif.py` 可重新生成。 |
| [`images/voice_character_states_source.png`](images/voice_character_states_source.png) | 1254 × 1254，PNG RGBA，2 × 2 图集 | 使用内置 imagegen 基于透明角色编辑四种面部表情。上排是待机和聆听，下排是思考和说话。生成图之间的部分服装和线条细节有所不同。 |
| `images/voice_character_{idle,listening,thinking,speaking}.png`、`images/av_character_{idle,listening,thinking,speaking}.c` | 四张 192 × 192 PNG RGBA 及 LVGL 9 RGB565A8 资源，每张 108 KiB | 由 `main/av_ui.c` 在固定位置显示。像素和透明度保存在 Flash 中，用同一个 LVGL 图片对象切换当前表情。 |
| [`images/voice_character_blink_source.png`](images/voice_character_blink_source.png)、[`images/voice_character_blink.png`](images/voice_character_blink.png)、[`images/av_character_blink.c`](images/av_character_blink.c) | 生成的透明源图；62 × 20 PNG RGBA 及 LVGL 9 RGB565A8 眼部图层，3720 字节 | 从已有 imagegen 闭眼帧裁出两个眼部区域，中间留透明空隙以露出原图刘海。生成提示要求轻闭双眼、保持姿势和透明背景。`main/av_ui.c` 在待机时随机间隔 3～5 秒，将眼部图层叠放于角色相对坐标 (60, 75)，持续 140 毫秒；眨眼过程中底图固定。其他页面和语音状态暂停 LVGL 定时器。使用 `tools/gen_av_character_states.py` 重新生成。 |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160，JPEG | 嵌入中英文项目 README 的产品主图，突出 AI Passport 产品形象与开放、人人可创作的理念。 |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724，PNG RGBA | 保留为可选技术参考图，不再用于首页主视觉。于 2026-09-17 使用内置图像生成工具为本仓库生成；已根据文档中的硬件能力契约核对图中的六项标签与参数。 |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336，PNG RGBA | 从仓库原始 `images/logo.png` 中精确裁切并去除背景的黑色字标；用于中英文项目 README 的浅色主题。 |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336，PNG RGBA | 提取字标的白色版本；README 使用 `<picture>` 在 GitHub 深色主题下显示。 |

- 使用描述性命名，并记录尺寸、像素格式、转换步骤与目标路径。
- 优先采用适合 240 × 320 RGB565 显示的格式，并纳入 Flash 与内部 RAM 考量。
- 许可允许时保留可编辑源文件，并记录来源与许可。
- 图片中不得包含设备二维码秘密、凭证或个人数据。

执行 `python3 tools/gen_av_character_states.py` 可重新生成当前表情资源，依赖 Pillow、`pypng` 和 `lz4`。脚本按等分四宫格提取表情，缩放到 192 × 192，再调用项目固定版本的 LVGL 转换器。imagegen 提示要求固定正面姿势和透明背景，只改变眼睛、眉毛和嘴巴，分别呈现闭嘴微笑、专注聆听、疑惑思考和开心张嘴说话。

## 音乐与音效（music）

可复用的音乐与音效源码放在 `music/`。

- 记录来源、许可、采样率、位深、声道、转换命令与目标路径。
- 与当前 BSP 音频路径匹配时优先采用 16 kHz、16 位单声道 PCM。
- 嵌入音频前评估 Flash 与内部 RAM 成本；长录音应流式或分块。
- 无再分发许可不提交媒体文件。

## 电池参数（battery）

[`battery/wanli-500mah-profile2-xil40x-20261020.txt`](battery/wanli-500mah-profile2-xil40x-20261020.txt)
保存万锂新能源 500 mAh 电芯的 CW2017 参数（profile2、XIL40X、2026-10-20）。
该文件是项目提供的 `喜梨_智能工卡_万锂新能源500mAh_profile2_XIL40X_20261020.txt`
的原样副本，按维护者要求保留用于本次固件更新；原文件没有附带单独的许可说明。

格式为 80 行两位十六进制数字，每行一个字节，按寄存器顺序排列。
BSP 在 `components/bsp/src/bsp_battery.c` 中嵌入这 80 字节，主机检查会验证
源文件及数组内容完全一致。文本文件 SHA-256：
`7ea96eba4634f4495a37c25ffacd54c1deaa0693b162c892d671a2ba68460919`。
解码后 80 字节 SHA-256：
`fa7365671093ef449c5bdcfb4adb279e3d275f888109f50b6b2f21e9b96220d5`。
这是电量计参数，不是充电配置，也不能通用于所有 500 mAh 电芯；详见
[硬件指南](../docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.zh_CN.md#9-cw2017-电池计)。
