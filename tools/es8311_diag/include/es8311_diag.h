/*
 * ES8311 I2C 诊断组件（配合 esp_codec_dev 1.6.2 的 legacy I2C 路径使用）
 *
 * 前提：调用方已用旧版驱动 i2c_param_config() + i2c_driver_install() 装好 I2C 总线，
 *       且工程开启 CONFIG_CODEC_I2C_BACKWARD_COMPATIBLE=y。本组件只发起事务，不安装驱动。
 *
 * 所有事务的字节序列与 esp_codec_dev/platform/audio_codec_ctrl_i2c.c 的 legacy 分支一致
 * （写：S addr reg data P；读：S addr reg P + S addr|1 data(NACK) P），
 * 区别只是这里把 i2c_master_cmd_begin() 的原始 esp_err_t 保留下来并打印。
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c.h"
#include "audio_codec_ctrl_if.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    i2c_port_t port;       // 与 i2c_driver_install() 相同的端口
    uint8_t    addr8;      // 8-bit 地址，ES8311 默认 0x30（7-bit 0x18）
    int        trigger_gpio; // 首次失败时拉高的空闲 GPIO，给示波器/逻辑分析仪触发；-1 关闭
    uint32_t   timeout_ms; // 0 表示使用驱动默认的 100ms
    int        sda_gpio;   // 失败时打印 SDA 电平；-1 不打印（注意结构体零初始化时是 GPIO0，需显式设置）
    int        scl_gpio;   // 失败时打印 SCL 电平；-1 不打印
} es8311_diag_cfg_t;

typedef struct {
    uint32_t iterations;     // 执行的序列次数
    uint32_t ok_iterations;  // 全程无错误的次数
    uint32_t write_fail;     // 写事务失败总数
    uint32_t read_fail;      // 读事务失败总数
    uint32_t nack;           // ESP_FAIL：从机未应答
    uint32_t timeout;        // ESP_ERR_TIMEOUT：总线超时或互斥锁等待超时
    uint32_t other;          // 其他错误码
    uint32_t reset_suspect;  // 失败后回读快照与基线不一致的次数（疑似芯片复位/掉电）
    bool     aborted;        // 前几轮全部失败而提前结束
    uint16_t fail_by_reg[256]; // 按寄存器统计失败次数
} es8311_diag_stats_t;

/** 保存配置并（可选）配置触发 GPIO。必须在其他函数之前调用。 */
esp_err_t es8311_diag_init(const es8311_diag_cfg_t *cfg);

/** 单次写/读，返回 i2c_master_cmd_begin() 的原始错误码。读操作两阶段分别检查。 */
esp_err_t es8311_diag_write(uint8_t reg, uint8_t val);
esp_err_t es8311_diag_read(uint8_t reg, uint8_t *val);

/**
 * 返回一个“带记录”的控制接口，可直接替换 audio_codec_new_i2c_ctrl() 的返回值传给
 * es8311_codec_new()。每次寄存器访问都会打印序号、寄存器、值、esp_err 名称和时间戳。
 */
const audio_codec_ctrl_if_t *es8311_diag_trace_ctrl(void);

/**
 * 按 esp_codec_dev 1.6.2 es8311_open() 的顺序（slave、use_mclk=true、不反相、
 * no_dac_ref=true）重放寄存器访问 iterations 次，统计失败并打印汇总。
 * label 用于区分测试条件（如 "A_no_i2s"、"B_i2s_mclk"）。
 * out 可为 NULL。
 */
esp_err_t es8311_diag_stress(const char *label, int iterations, es8311_diag_stats_t *out);

/**
 * 判断写事务的 NACK 出现在哪个字节。对同一寄存器交替发三种写法，各 iterations 次：
 *   V0：地址、寄存器、数据三个字节都检查 ACK（与驱动一致）
 *   V1：只检查地址和寄存器字节，数据字节不检查
 *   V2：只检查地址字节
 * V1/V2 写完后回读，统计“值没写进去”的次数。
 * 只用驱动本身会写的 reg/val（例如 0x44=0x08），不要传驱动序列之外的值。
 */
esp_err_t es8311_diag_ack_position(const char *label, uint8_t reg, uint8_t val, int iterations);

/** 打印 0x00~0x45 及 0xFD~0xFF 的寄存器快照（只读，不改值）。 */
void es8311_diag_dump(const char *label);

#ifdef __cplusplus
}
#endif
