// main/main_allinone.c —— 合并固件入口（APP_VARIANT=allinone）。
//
// 一个固件同时包含监测工具（KK 1-bit UI）与语音助手（对讲 + 小说 + 音乐）。
// 两套渲染共用 LCD 完成回调，不能并行，所以由 kk_boot.c 在开机时二选一。
#include "kk_boot.h"

void app_main(void)
{
    kk_boot_run();
}
