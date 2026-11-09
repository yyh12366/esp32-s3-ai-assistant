/*
 * ES8311 I2C 诊断组件实现。说明见 include/es8311_diag.h 和 README.md。
 */
#include "es8311_diag.h"

#include <string.h>
#include <inttypes.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "ES8311_DIAG"
#define DRIVER_DEFAULT_TIMEOUT_MS 100   // 与 audio_codec_ctrl_i2c.c 的 DEFAULT_I2C_TRANS_TIMEOUT 相同

static es8311_diag_cfg_t s_cfg;
static bool s_inited;
static uint32_t s_trace_seq;
static bool s_any_ok_in_stress;

/* ---------- 触发 GPIO ---------- */

static inline void trigger_set(int level)
{
    if (s_cfg.trigger_gpio >= 0) {
        gpio_set_level((gpio_num_t) s_cfg.trigger_gpio, level);
    }
}

/* 打印失败当下的 SDA/SCL 电平：持续为 0 说明总线被拉低 */
static void log_bus_levels(const char *label)
{
    if (s_cfg.sda_gpio < 0 && s_cfg.scl_gpio < 0) {
        return;
    }
    int sda = s_cfg.sda_gpio >= 0 ? gpio_get_level((gpio_num_t) s_cfg.sda_gpio) : -1;
    int scl = s_cfg.scl_gpio >= 0 ? gpio_get_level((gpio_num_t) s_cfg.scl_gpio) : -1;
    ESP_LOGW(TAG, "[%s] 失败后总线电平 SDA=%d SCL=%d", label, sda, scl);
}

/* 连续若干轮全部失败时提前结束，避免 100ms 超时把一个条件拖到数分钟 */
#define ABORT_AFTER_ALL_FAIL_ROUNDS 3

/* ---------- 原始事务（字节序列与 esp_codec_dev legacy 分支一致） ---------- */

static TickType_t xfer_ticks(void)
{
    uint32_t ms = s_cfg.timeout_ms ? s_cfg.timeout_ms : DRIVER_DEFAULT_TIMEOUT_MS;
    TickType_t t = pdMS_TO_TICKS(ms);
    return t ? t : 1;
}

static esp_err_t raw_write(const uint8_t *reg, int reg_len, const uint8_t *data, int data_len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        return ESP_ERR_NO_MEM;
    }
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, s_cfg.addr8, true);
    i2c_master_write(cmd, reg, reg_len, true);
    if (data_len) {
        i2c_master_write(cmd, data, data_len, true);
    }
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(s_cfg.port, cmd, xfer_ticks());
    i2c_cmd_link_delete(cmd);
    return ret;
}

/* 返回值：阶段 1（写寄存器地址）或阶段 2（读数据）的第一个错误；*phase 指出是哪一阶段 */
static esp_err_t raw_read(const uint8_t *reg, int reg_len, uint8_t *data, int data_len, int *phase)
{
    *phase = 0;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        return ESP_ERR_NO_MEM;
    }
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, s_cfg.addr8, true);
    i2c_master_write(cmd, reg, reg_len, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(s_cfg.port, cmd, xfer_ticks());
    i2c_cmd_link_delete(cmd);
    if (ret != ESP_OK) {
        *phase = 1;
        return ret;
    }

    cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        return ESP_ERR_NO_MEM;
    }
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, s_cfg.addr8 | 0x01, true);
    for (int i = 0; i < data_len - 1; i++) {
        i2c_master_read_byte(cmd, data + i, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, data + data_len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    ret = i2c_master_cmd_begin(s_cfg.port, cmd, xfer_ticks());
    i2c_cmd_link_delete(cmd);
    if (ret != ESP_OK) {
        *phase = 2;
    }
    return ret;
}

esp_err_t es8311_diag_init(const es8311_diag_cfg_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = *cfg;
    if (s_cfg.trigger_gpio >= 0) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << s_cfg.trigger_gpio,
            .mode = GPIO_MODE_OUTPUT,
        };
        esp_err_t e = gpio_config(&io);
        if (e != ESP_OK) {
            return e;
        }
        gpio_set_level((gpio_num_t) s_cfg.trigger_gpio, 0);
    }
    s_inited = true;
    s_trace_seq = 0;
    ESP_LOGI(TAG, "port=%d addr8=0x%02X timeout=%" PRIu32 "ms trigger_gpio=%d",
             s_cfg.port, s_cfg.addr8,
             s_cfg.timeout_ms ? s_cfg.timeout_ms : DRIVER_DEFAULT_TIMEOUT_MS,
             s_cfg.trigger_gpio);
    return ESP_OK;
}

esp_err_t es8311_diag_write(uint8_t reg, uint8_t val)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    trigger_set(0);
    esp_err_t e = raw_write(&reg, 1, &val, 1);
    if (e != ESP_OK) {
        trigger_set(1);
    }
    return e;
}

esp_err_t es8311_diag_read(uint8_t reg, uint8_t *val)
{
    if (!s_inited || val == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    int phase;
    *val = 0;
    trigger_set(0);
    esp_err_t e = raw_read(&reg, 1, val, 1, &phase);
    if (e != ESP_OK) {
        trigger_set(1);
    }
    return e;
}

/* ---------- 带记录的 ctrl_if ---------- */

static bool trace_is_open(const audio_codec_ctrl_if_t *ctrl)
{
    return s_inited;
}

static int trace_open(const audio_codec_ctrl_if_t *ctrl, void *cfg, int cfg_size)
{
    return s_inited ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_WRONG_STATE;
}

static int trace_close(const audio_codec_ctrl_if_t *ctrl)
{
    return ESP_CODEC_DEV_OK;
}

static int trace_get_info(const audio_codec_ctrl_if_t *ctrl, audio_codec_ctrl_info_t *info)
{
    if (info == NULL) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }
    memset(info, 0, sizeof(*info));
    info->type = AUDIO_CODEC_CTRL_I2C;
    info->i2c.port = s_cfg.port;
    info->i2c.addr = s_cfg.addr8;
    return ESP_CODEC_DEV_OK;
}

static int trace_read(const audio_codec_ctrl_if_t *ctrl, int addr, int addr_len, void *data, int data_len)
{
    if (!s_inited || data == NULL || data_len <= 0) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }
    int phase;
    int64_t t = esp_timer_get_time();
    trigger_set(0);
    esp_err_t e = raw_read((const uint8_t *) &addr, addr_len, data, data_len, &phase);
    uint32_t seq = ++s_trace_seq;
    if (e == ESP_OK) {
        ESP_LOGI(TAG, "#%03" PRIu32 " R %02X -> %02X ok t=%lldus",
                 seq, addr & 0xFF, ((uint8_t *) data)[0], (long long) t);
        return ESP_CODEC_DEV_OK;
    }
    trigger_set(1);
    ESP_LOGE(TAG, "#%03" PRIu32 " R %02X FAIL phase=%d err=%s(0x%x) t=%lldus",
             seq, addr & 0xFF, phase, esp_err_to_name(e), e, (long long) t);
    return ESP_CODEC_DEV_READ_FAIL;
}

static int trace_write(const audio_codec_ctrl_if_t *ctrl, int addr, int addr_len, void *data, int data_len)
{
    if (!s_inited || (data == NULL && data_len)) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }
    int64_t t = esp_timer_get_time();
    trigger_set(0);
    esp_err_t e = raw_write((const uint8_t *) &addr, addr_len, data, data_len);
    uint32_t seq = ++s_trace_seq;
    uint8_t v = data_len ? ((uint8_t *) data)[0] : 0;
    if (e == ESP_OK) {
        ESP_LOGI(TAG, "#%03" PRIu32 " W %02X = %02X ok t=%lldus", seq, addr & 0xFF, v, (long long) t);
        return ESP_CODEC_DEV_OK;
    }
    trigger_set(1);
    ESP_LOGE(TAG, "#%03" PRIu32 " W %02X = %02X FAIL err=%s(0x%x) t=%lldus",
             seq, addr & 0xFF, v, esp_err_to_name(e), e, (long long) t);
    return ESP_CODEC_DEV_WRITE_FAIL;
}

static const audio_codec_ctrl_if_t s_trace_ctrl = {
    .open = trace_open,
    .is_open = trace_is_open,
    .read_reg = trace_read,
    .write_reg = trace_write,
    .get_info = trace_get_info,
    .close = trace_close,
};

const audio_codec_ctrl_if_t *es8311_diag_trace_ctrl(void)
{
    return &s_trace_ctrl;
}

/* ---------- 重放 es8311_open() 序列 ---------- */

/*
 * 以下顺序与取值逐条照抄 esp_codec_dev 1.6.2 device/es8311/es8311.c 的 es8311_open()，
 * 对应配置：master_mode=false、use_mclk=true、invert_mclk=false、invert_sclk=false、
 * no_dac_ref=true。本组件不引入任何驱动以外的寄存器值。
 */
typedef enum {
    OP_W,          // 直接写 val
    OP_R0D,        // 读 0D，若不等于 val 则写 val
    OP_RMW_AND,    // 读 reg，写 (读值 & val)
} op_kind_t;

typedef struct {
    op_kind_t kind;
    uint8_t   reg;
    uint8_t   val;
} seq_op_t;

static const seq_op_t s_open_seq[] = {
    {OP_R0D, 0x0D, 0xFA},
    {OP_W, 0x44, 0x08},
    {OP_W, 0x44, 0x08},
    {OP_W, 0x01, 0x30},
    {OP_W, 0x02, 0x00},
    {OP_W, 0x03, 0x10},
    {OP_W, 0x16, 0x24},
    {OP_W, 0x04, 0x10},
    {OP_W, 0x05, 0x00},
    {OP_W, 0x0B, 0x00},
    {OP_W, 0x0C, 0x00},
    {OP_W, 0x10, 0x1F},
    {OP_W, 0x11, 0x7F},
    {OP_W, 0x00, 0x80},
    {OP_RMW_AND, 0x00, 0xBF},   // slave：清 bit6
    {OP_W, 0x01, 0x3F},         // use_mclk=true、不反相：0x3F & 0x7F & ~0x40
    {OP_RMW_AND, 0x06, 0xDF},   // 不反相 SCLK：清 bit5
    {OP_W, 0x13, 0x10},
    {OP_W, 0x1B, 0x0A},
    {OP_W, 0x1C, 0x6A},
    {OP_W, 0x44, 0x08},         // no_dac_ref=true
};

/* 回读快照用的寄存器：序列中写过的全部寄存器 */
static const uint8_t s_snap_regs[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x0B, 0x0C, 0x0D,
    0x10, 0x11, 0x13, 0x16, 0x1B, 0x1C, 0x44,
};
#define SNAP_N (sizeof(s_snap_regs) / sizeof(s_snap_regs[0]))

static void count_err(es8311_diag_stats_t *st, esp_err_t e, uint8_t reg, bool is_write)
{
    if (is_write) {
        st->write_fail++;
    } else {
        st->read_fail++;
    }
    if (e == ESP_FAIL) {
        st->nack++;
    } else if (e == ESP_ERR_TIMEOUT) {
        st->timeout++;
    } else {
        st->other++;
    }
    st->fail_by_reg[reg]++;
}

/* 返回 true 表示快照全部读成功 */
static bool take_snapshot(uint8_t *out)
{
    for (size_t i = 0; i < SNAP_N; i++) {
        if (es8311_diag_read(s_snap_regs[i], &out[i]) != ESP_OK) {
            return false;
        }
    }
    return true;
}

esp_err_t es8311_diag_stress(const char *label, int iterations, es8311_diag_stats_t *out)
{
    if (!s_inited || iterations <= 0) {
        return ESP_ERR_INVALID_STATE;
    }
    static es8311_diag_stats_t st;   // 结构较大，避免占用调用方任务栈
    memset(&st, 0, sizeof(st));
    uint8_t baseline[SNAP_N];
    uint8_t snap[SNAP_N];
    bool have_baseline = false;
    int failures_logged = 0;
    s_any_ok_in_stress = false;

    ESP_LOGI(TAG, "[%s] 开始重放 es8311_open() 序列 %d 次", label, iterations);
    for (int it = 0; it < iterations; it++) {
        bool iter_ok = true;
        bool iter_any_ok = false;
        for (size_t s = 0; s < sizeof(s_open_seq) / sizeof(s_open_seq[0]); s++) {
            const seq_op_t *op = &s_open_seq[s];
            esp_err_t e;
            uint8_t v = 0;
            switch (op->kind) {
            case OP_W:
                e = es8311_diag_write(op->reg, op->val);
                if (e != ESP_OK) {
                    count_err(&st, e, op->reg, true);
                }
                break;
            case OP_R0D:
                e = es8311_diag_read(op->reg, &v);
                if (e != ESP_OK) {
                    count_err(&st, e, op->reg, false);
                } else if (v != op->val) {
                    e = es8311_diag_write(op->reg, op->val);
                    if (e != ESP_OK) {
                        count_err(&st, e, op->reg, true);
                    }
                }
                break;
            case OP_RMW_AND:
            default:
                e = es8311_diag_read(op->reg, &v);
                if (e != ESP_OK) {
                    count_err(&st, e, op->reg, false);
                    break;   // 驱动在读失败时会写入 0 & mask；这里跳过写，避免引入不同的值
                }
                e = es8311_diag_write(op->reg, v & op->val);
                if (e != ESP_OK) {
                    count_err(&st, e, op->reg, true);
                }
                break;
            }
            if (e != ESP_OK) {
                iter_ok = false;
                if (failures_logged < 40) {   // 只打印前 40 条，避免日志本身拖慢总线节奏
                    ESP_LOGE(TAG, "[%s] it=%d step=%u reg=%02X err=%s(0x%x)",
                             label, it, (unsigned) s, op->reg, esp_err_to_name(e), e);
                    if (failures_logged < 3) {
                        log_bus_levels(label);
                    }
                    failures_logged++;
                }
            } else {
                iter_any_ok = true;
            }
        }
        /* 前 ABORT_AFTER_ALL_FAIL_ROUNDS 轮内没有任何一次访问成功：该条件下芯片不响应，提前结束 */
        if (!iter_any_ok && !s_any_ok_in_stress && it + 1 >= ABORT_AFTER_ALL_FAIL_ROUNDS) {
            st.iterations++;
            st.aborted = true;
            break;
        }
        if (iter_any_ok) {
            s_any_ok_in_stress = true;
        }

        if (iter_ok) {
            st.ok_iterations++;
            if (!have_baseline && take_snapshot(baseline)) {
                have_baseline = true;
            }
        } else if (have_baseline) {
            /* 失败后读回：若之前写成功过的寄存器值变了，说明芯片可能复位过 */
            vTaskDelay(pdMS_TO_TICKS(1));
            if (take_snapshot(snap) && memcmp(snap, baseline, SNAP_N) != 0) {
                st.reset_suspect++;
                for (size_t i = 0; i < SNAP_N; i++) {
                    if (snap[i] != baseline[i]) {
                        ESP_LOGW(TAG, "[%s] it=%d 回读差异 REG%02X 基线=%02X 现在=%02X",
                                 label, it, s_snap_regs[i], baseline[i], snap[i]);
                    }
                }
            }
        }
        st.iterations++;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    trigger_set(0);

    ESP_LOGI(TAG, "[%s] 汇总: 序列=%" PRIu32 " 成功=%" PRIu32 " 写失败=%" PRIu32
             " 读失败=%" PRIu32 " NACK=%" PRIu32 " TIMEOUT=%" PRIu32 " 其他=%" PRIu32
             " 疑似复位=%" PRIu32 "%s%s",
             label, st.iterations, st.ok_iterations, st.write_fail, st.read_fail,
             st.nack, st.timeout, st.other, st.reset_suspect,
             have_baseline ? "" : "（无成功基线，未做复位判断）",
             st.aborted ? "（前几轮无任何访问成功，提前结束）" : "");
    for (int r = 0; r < 256; r++) {
        if (st.fail_by_reg[r]) {
            ESP_LOGI(TAG, "[%s]   REG%02X 失败 %u 次", label, r, st.fail_by_reg[r]);
        }
    }
    if (out) {
        *out = st;
    }
    return ESP_OK;
}

/* 单字节寄存器写，可分别关闭寄存器字节和数据字节的 ACK 检查 */
static esp_err_t raw_write_ack(uint8_t reg, uint8_t val, bool ack_reg, bool ack_data)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        return ESP_ERR_NO_MEM;
    }
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, s_cfg.addr8, true);
    i2c_master_write_byte(cmd, reg, ack_reg);
    i2c_master_write_byte(cmd, val, ack_data);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(s_cfg.port, cmd, xfer_ticks());
    i2c_cmd_link_delete(cmd);
    return ret;
}

esp_err_t es8311_diag_ack_position(const char *label, uint8_t reg, uint8_t val, int iterations)
{
    if (!s_inited || iterations <= 0) {
        return ESP_ERR_INVALID_STATE;
    }
    static const struct {
        const char *name;
        bool ack_reg;
        bool ack_data;
    } variants[3] = {
        {"V0 全部检查", true, true},
        {"V1 不查数据字节", true, false},
        {"V2 只查地址字节", false, false},
    };
    uint32_t fail[3] = {0}, not_written[3] = {0}, readback_fail[3] = {0};
    int done = 0;
    bool aborted = false;

    ESP_LOGI(TAG, "[%s] ACK 位置测试 REG%02X=%02X，每种写法 %d 次", label, reg, val, iterations);
    for (int it = 0; it < iterations; it++) {
        for (int v = 0; v < 3; v++) {
            trigger_set(0);
            esp_err_t e = raw_write_ack(reg, val, variants[v].ack_reg, variants[v].ack_data);
            if (e != ESP_OK) {
                fail[v]++;
                trigger_set(1);
            }
            if (v > 0) {
                uint8_t rb = 0;
                if (es8311_diag_read(reg, &rb) != ESP_OK) {
                    readback_fail[v]++;
                } else if (rb != val) {
                    not_written[v]++;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        done = it + 1;
        if (done >= ABORT_AFTER_ALL_FAIL_ROUNDS && fail[0] == (uint32_t) done && fail[1] == (uint32_t) done
                && fail[2] == (uint32_t) done) {
            aborted = true;
            log_bus_levels(label);
            break;
        }
    }
    trigger_set(0);
    for (int v = 0; v < 3; v++) {
        ESP_LOGI(TAG, "[%s] %s: 事务失败=%" PRIu32 "/%d  回读不符=%" PRIu32 "  回读失败=%" PRIu32 "%s%s",
                 label, variants[v].name, fail[v], done, not_written[v], readback_fail[v],
                 v == 0 ? "（V0 不回读）" : "", aborted ? "（全部失败，提前结束）" : "");
    }
    return ESP_OK;
}

void es8311_diag_dump(const char *label)
{
    char line[80];
    int pos = 0;
    ESP_LOGI(TAG, "[%s] 寄存器快照", label);
    uint8_t probe = 0;
    esp_err_t pe = es8311_diag_read(0xFD, &probe);
    if (pe != ESP_OK) {
        ESP_LOGE(TAG, "[%s] 读 REGFD 失败 %s，跳过快照", label, esp_err_to_name(pe));
        log_bus_levels(label);
        return;
    }
    for (int r = 0x00; r <= 0x45; r++) {
        uint8_t v = 0;
        esp_err_t e = es8311_diag_read((uint8_t) r, &v);
        pos += snprintf(line + pos, sizeof(line) - pos, e == ESP_OK ? " %02X" : " --", v);
        if ((r & 0x0F) == 0x0F || r == 0x45) {
            ESP_LOGI(TAG, "  %02X:%s", r & 0xF0, line);
            pos = 0;
        }
    }
    for (int r = 0xFD; r <= 0xFF; r++) {
        uint8_t v = 0;
        esp_err_t e = es8311_diag_read((uint8_t) r, &v);
        if (e == ESP_OK) {
            ESP_LOGI(TAG, "  REG%02X=%02X", r, v);
        } else {
            ESP_LOGE(TAG, "  REG%02X 读失败 %s", r, esp_err_to_name(e));
        }
    }
}
