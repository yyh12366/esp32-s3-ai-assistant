/*
 * ES8311 诊断示例：按矩阵条件 A/B/C 依次测试，最后用带记录的 ctrl_if 跑一次官方 es8311_codec_new()。
 * 引脚按你的接线；ILI9341 不在这里初始化（条件 D 需要你在自己的工程里对比刷屏开/关）。
 */
#include "es8311_diag.h"
#include "es8311_codec.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define I2C_PORT   I2C_NUM_0
#define PIN_SDA    8
#define PIN_SCL    9
#define PIN_MCLK   16
#define PIN_BCLK   17
#define PIN_WS     15
#define PIN_DOUT   7
#define PIN_DIN    18
#define TRIG_GPIO  (-1)     // 改成一个空闲 GPIO 可给示波器触发
#define N_ITER     200

static const char *TAG = "diag_main";
static i2s_chan_handle_t s_tx, s_rx;

static void i2c_legacy_init(void)
{
    i2c_config_t c = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 20000,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &c));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0));
}

/* with_mclk=false 时 MCLK 引脚不输出（条件 C） */
static void i2s_start(bool with_mclk)
{
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan, &s_tx, &s_rx));
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = with_mclk ? PIN_MCLK : I2S_GPIO_UNUSED,
            .bclk = PIN_BCLK, .ws = PIN_WS, .dout = PIN_DOUT, .din = PIN_DIN,
        },
    };
    std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &std));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));
}

static void i2s_stop(void)
{
    i2s_channel_disable(s_tx);
    i2s_channel_disable(s_rx);
    i2s_del_channel(s_tx);
    i2s_del_channel(s_rx);
    s_tx = s_rx = NULL;
}

void app_main(void)
{
    i2c_legacy_init();
    es8311_diag_cfg_t cfg = {
        .port = I2C_PORT, .addr8 = 0x30, .trigger_gpio = TRIG_GPIO, .timeout_ms = 0,
    };
    ESP_ERROR_CHECK(es8311_diag_init(&cfg));
    es8311_diag_dump("power_on");

    /* A：I2S 未初始化，三根时钟线都不翻转 */
    es8311_diag_stress("A_no_i2s", N_ITER, NULL);

    /* B：I2S 主机运行，MCLK 输出（与当前工程一致） */
    i2s_start(true);
    vTaskDelay(pdMS_TO_TICKS(20));
    es8311_diag_stress("B_i2s_mclk", N_ITER, NULL);
    i2s_stop();

    /* C：I2S 运行但 MCLK 不输出。只测 I2C，不判断音频是否正常 */
    i2s_start(false);
    vTaskDelay(pdMS_TO_TICKS(20));
    es8311_diag_stress("C_i2s_no_mclk", N_ITER, NULL);
    i2s_stop();

    /* 最后：恢复条件 B，用带记录的 ctrl_if 跑一次官方初始化，定位具体失败的寄存器 */
    i2s_start(true);
    vTaskDelay(pdMS_TO_TICKS(20));
    es8311_codec_cfg_t codec_cfg = {
        .ctrl_if = es8311_diag_trace_ctrl(),
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .pa_pin = -1,
        .mclk_div = 256,
        .no_dac_ref = true,
    };
    const audio_codec_if_t *codec = es8311_codec_new(&codec_cfg);
    ESP_LOGI(TAG, "es8311_codec_new(trace) => %s", codec ? "OK" : "NULL");
    es8311_diag_dump("after_codec_new");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
