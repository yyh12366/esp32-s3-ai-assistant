// main/av_boot.h —— 独立语音固件（astrbot_voice）的启动入口。
//
// 由 main_astrbot_voice.c 调用。合并固件不使用它，而是由 kk_boot.c 进入 av_shell 首页。
#pragma once

void av_boot_start(void);
