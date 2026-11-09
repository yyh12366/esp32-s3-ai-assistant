// main/kk_boot.h —— 合并固件的开机入口：首页（av_shell）或监测（KK 1-bit UI）。
#pragma once

// 按开机目标进入：监测 → kk_app_start()，否则 → av_shell_start()（首页）。
// 监测和首页共用 LCD，不能同时运行；切换由 av_shell_reboot_to() 保存目标后重启。
void kk_boot_run(void);
