// main/av_boot.c —— 独立语音固件（APP_VARIANT=astrbot_voice）的启动入口。
//
// 直接交给 av_shell 的仅对讲模式：没有首页、小说和音乐，设置从对讲进入并返回对讲。
#include "av_boot.h"

#include "av_shell.h"

void av_boot_start(void)
{
    av_shell_start_voice_only();
}
