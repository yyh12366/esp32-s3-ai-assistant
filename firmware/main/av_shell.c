// main/av_shell.c —— 合并固件的首页与功能切换。
//
// 按键只有一个回调 → 队列 → shell 任务。当前功能决定按键去向：
//   首页      UP/DOWN 移动光标，OK 单击进入；选「监测」保存目标后重启
//   大肥鱼    所有按键原样交给对讲（按住说话需要 PRESS/RELEASE）；离线 OK 长按 / 在线 UP 长按回首页
//   小说/音乐 单击/长按交给媒体页；列表页长按 OK 回首页
//   设置      单击/长按交给设置页；根菜单长按 OK 或选「返回」回首页
// 编译时定义 AV_SHELL_VOICE_ONLY（astrbot_voice）时没有首页、小说、音乐，设置从对讲进入，返回对讲。
#include "av_shell.h"

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "av_app.h"
#include "av_audio.h"
#include "av_ota.h"
#include "av_settings.h"
#include "av_store.h"
#include "av_ui.h"
#include "av_wifi.h"
#ifndef AV_SHELL_VOICE_ONLY
#include "av_media_app.h"
#endif

static const char *TAG = "av_shell";

#define SHELL_STACK    4096   // 任务只做分发和小界面，不做重活
#define SHELL_QUEUE    12
#define SHELL_TICK_MS  100

typedef enum { ITEM_KEY, ITEM_VOICE_HOME } item_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t btn, ev;
} shell_item_t;

typedef enum {
    MODE_NONE = 0,
    MODE_HOME,
    MODE_VOICE,
    MODE_BOOKS,
    MODE_MUSIC,
    MODE_SETTINGS,
} shell_mode_t;

enum { HOME_VOICE, HOME_BOOKS, HOME_MUSIC, HOME_SETTINGS, HOME_MONITOR,
#ifdef AV_S3_VIDEO
       HOME_VIDEO,
#endif
       HOME_COUNT };

static QueueHandle_t s_queue;
static shell_mode_t s_mode = MODE_NONE;
static bool s_voice_only;
static int s_home_sel;
static const char *s_startup_error;   // 对讲启动时显示的持久错误（NVS / 音频 / Wi-Fi 初始化失败）

static void enter_mode(shell_mode_t next);

// ---------------------------------------------------------------------------
// 按键 / 回调（只投递，不做事）
// ---------------------------------------------------------------------------
static void on_button(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    shell_item_t item = {.kind = ITEM_KEY, .btn = (uint8_t)btn, .ev = (uint8_t)ev};
    if (s_queue) xQueueSend(s_queue, &item, 0);
}

// 对讲请求回首页（应用任务里调用，只能非阻塞）
static void on_voice_home_request(void)
{
    shell_item_t item = {.kind = ITEM_VOICE_HOME};
    if (s_queue) xQueueSend(s_queue, &item, 0);
}

static void log_heap(const char *what)
{
    ESP_LOGI(TAG, "%s: heap free=%u largest=%u", what,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

// 回到“首页”：合并固件是首页菜单，astrbot_voice 是对讲
static void return_home(void)
{
    enter_mode(s_voice_only ? MODE_VOICE : MODE_HOME);
}

// ---------------------------------------------------------------------------
// 首页
// ---------------------------------------------------------------------------
static void home_show(void)
{
    static const char *const labels[HOME_COUNT] = {"大肥鱼", "小说", "音乐", "设置", "监测",
#ifdef AV_S3_VIDEO
                                                  "Video",
#endif
    };
    if (!bsp_lvgl_lock(500)) return;
    av_ui_menu_show("首页", labels, NULL, HOME_COUNT, s_home_sel, "上下选择   OK 进入");
    bsp_lvgl_unlock();
}

static void reboot_to_monitor(void)
{
    if (bsp_lvgl_lock(500)) {
        av_ui_page_show("监测", "正在切换到监测…", NULL, 0, NULL);
        bsp_lvgl_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(300));
    av_shell_reboot_to(AV_SHELL_TARGET_MONITOR);
}

static void home_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_UP) {
        s_home_sel = (s_home_sel + HOME_COUNT - 1) % HOME_COUNT;
        home_show();
    } else if (btn == BSP_BTN_DOWN) {
        s_home_sel = (s_home_sel + 1) % HOME_COUNT;
        home_show();
    } else if (btn == BSP_BTN_OK) {
        switch (s_home_sel) {
        case HOME_VOICE: enter_mode(MODE_VOICE); break;
        case HOME_BOOKS: enter_mode(MODE_BOOKS); break;
        case HOME_MUSIC: enter_mode(MODE_MUSIC); break;
        case HOME_SETTINGS: enter_mode(MODE_SETTINGS); break;
        case HOME_MONITOR: reboot_to_monitor(); break;
#ifdef AV_S3_VIDEO
        case HOME_VIDEO: av_shell_reboot_to(AV_SHELL_TARGET_VIDEO); break;
#endif
        default: break;
        }
    }
}

// ---------------------------------------------------------------------------
// 功能切换
// ---------------------------------------------------------------------------
static void leave_mode(shell_mode_t next)
{
    switch (s_mode) {
    case MODE_VOICE:
        // astrbot_voice 进设置时保持中继连接（与旧版一致）；其余情况挂起对讲
        if (s_voice_only && next == MODE_SETTINGS) av_app_hide();
        else av_app_exit();
        break;
#ifndef AV_SHELL_VOICE_ONLY
    case MODE_BOOKS:
    case MODE_MUSIC:
        av_media_app_exit();
        break;
#endif
    case MODE_SETTINGS:
        av_settings_exit();
        break;
    default:
        break;
    }
}

static void enter_mode(shell_mode_t next)
{
    leave_mode(next);
    s_mode = next;
    switch (next) {
    case MODE_HOME:
        home_show();
        break;
    case MODE_VOICE:
        if (av_app_start(s_startup_error) != ESP_OK) {
            ESP_LOGE(TAG, "对讲启动失败");
            s_mode = MODE_NONE;
            enter_mode(s_voice_only ? MODE_SETTINGS : MODE_HOME);
            return;
        }
        av_app_enter();
        break;
#ifndef AV_SHELL_VOICE_ONLY
    case MODE_BOOKS:
        av_media_app_enter(false);
        break;
    case MODE_MUSIC:
        av_media_app_enter(true);
        break;
#endif
    case MODE_SETTINGS:
        av_settings_enter();
        break;
    default:
        break;
    }
    log_heap(next == MODE_VOICE ? "enter voice" : next == MODE_SETTINGS ? "enter settings" :
             next == MODE_HOME ? "enter home" : "enter media");
}

// ---------------------------------------------------------------------------
// 按键分发（shell 任务）
// ---------------------------------------------------------------------------
static void handle_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    const bool tap_or_long = ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG;
    switch (s_mode) {
    case MODE_VOICE:
        av_app_post_key(btn, ev);  // 对讲需要 PRESS / RELEASE，全部转发
        break;
    case MODE_HOME:
        home_key(btn, ev);
        break;
    case MODE_SETTINGS:
        if (tap_or_long && av_settings_key(btn, ev)) return_home();
        break;
#ifndef AV_SHELL_VOICE_ONLY
    case MODE_BOOKS:
    case MODE_MUSIC:
        if (tap_or_long && av_media_app_key(btn, ev)) return_home();
        break;
#endif
    default:
        break;
    }
}

static void shell_tick(void)
{
    av_ota_boot_poll(av_wifi_connected());  // 新固件联网稳定后确认有效（否则下次复位回滚）
#ifndef AV_SHELL_VOICE_ONLY
    if (s_mode == MODE_BOOKS || s_mode == MODE_MUSIC) av_media_app_poll();
#endif
    if (s_mode == MODE_SETTINGS) av_settings_poll();
}

static void shell_task(void *arg)
{
    (void)arg;
    for (;;) {
        shell_item_t item;
        if (xQueueReceive(s_queue, &item, pdMS_TO_TICKS(SHELL_TICK_MS)) == pdTRUE) {
            if (item.kind == ITEM_VOICE_HOME) {
                if (s_mode == MODE_VOICE) enter_mode(s_voice_only ? MODE_SETTINGS : MODE_HOME);
            } else {
                handle_key((bsp_btn_t)item.btn, (bsp_btn_ev_t)item.ev);
            }
        }
        shell_tick();
    }
}

// ---------------------------------------------------------------------------
// 一次性初始化
// ---------------------------------------------------------------------------
static esp_err_t shell_init(void)
{
    ESP_LOGI(TAG, "AstrBot shell 启动%s", s_voice_only ? "（仅对讲）" : "");
    bsp_i2c_init();
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败 (MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return ESP_FAIL;
    }
    if (bsp_lvgl_lock(1000)) {
        av_ui_init();
        bsp_lvgl_unlock();
    }
    bsp_display_backlight(80);

    s_queue = xQueueCreate(SHELL_QUEUE, sizeof(shell_item_t));
    if (!s_queue) return ESP_ERR_NO_MEM;

    // 错误优先级：Wi-Fi < 音频 < NVS（后者覆盖前者，与旧版启动顺序一致）
    const char *err_text = NULL;
    const esp_err_t nvs = av_store_init();
    if (bsp_battery_init() != ESP_OK) ESP_LOGW(TAG, "电量计不可用，界面不显示电量");

    if (av_audio_init(av_store_get_volume()) != ESP_OK) {
        err_text = "音频初始化失败，无法录音/播放";
    }
    if (nvs == ESP_OK) {
        if (av_wifi_init() != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi 初始化失败");
            err_text = "Wi-Fi 初始化失败，无法联网";
        }
    }
    if (nvs != ESP_OK) err_text = "存储(NVS)不可用，无法保存设置";
    s_startup_error = err_text;
    av_ota_init_boot();  // 记录运行分区的 OTA 状态（新固件 PENDING_VERIFY 时等待确认）

    if (xTaskCreate(shell_task, "av_shell", SHELL_STACK, NULL, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    av_app_set_home_cb(on_voice_home_request);
    if (bsp_button_init(on_button, NULL) != ESP_OK) ESP_LOGE(TAG, "按键初始化失败");
    log_heap("shell ready");
    return ESP_OK;
}

void av_shell_start(void)
{
    s_voice_only = false;
    if (shell_init() != ESP_OK) {
        ESP_LOGE(TAG, "shell 初始化失败");
        return;
    }
    enter_mode(MODE_HOME);
}

void av_shell_start_voice_only(void)
{
    s_voice_only = true;
    if (shell_init() != ESP_OK) {
        ESP_LOGE(TAG, "shell 初始化失败");
        return;
    }
    enter_mode(MODE_VOICE);
}
