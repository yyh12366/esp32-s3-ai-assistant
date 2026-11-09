# es8311_diag：ES8311 I2C 初始化失败诊断组件

适用环境：ESP-IDF 5.5.3、esp_codec_dev 1.6.2、legacy I2C 驱动（`CONFIG_CODEC_I2C_BACKWARD_COMPATIBLE=y`）。

组件提供三项功能：

| 功能 | 作用 |
|---|---|
| `es8311_diag_trace_ctrl()` | 替换传给 `es8311_codec_new()` 的 `ctrl_if`，逐条打印每次寄存器访问的寄存器、值、`esp_err` 名称和时间戳 |
| `es8311_diag_stress(label, N)` | 把 `es8311_open()` 的寄存器序列原样重放 N 次，按寄存器和错误类型统计失败；失败后回读，与成功基线比较，用来判断芯片是否复位过 |
| `es8311_diag_ack_position(label, reg, val, N)` | 交替用三种写法（全部检查 ACK / 不查数据字节 / 只查地址字节）写同一寄存器，判断 NACK 出在哪个字节，以及值实际有没有写进去 |
| `es8311_diag_dump(label)` | 只读快照：0x00–0x45 和 0xFD–0xFF |

所有 I2C 事务的字节序列与 esp_codec_dev 的 legacy 分支一致。重放用的寄存器值逐条取自驱动源码，对应配置为 slave、use_mclk、不反相、no_dac_ref。组件没有引入任何驱动之外的寄存器值。

## 接入方法

1. 把整个 `es8311_diag/` 目录（不含 `example/`）复制到固件工程的 `components/` 下。
2. 你的工程需要先自己完成 `i2c_param_config()` 和 `i2c_driver_install()`。
3. 参考 `example/main/diag_main.c`，在 `es8311_codec_new()` 之前调用：

```c
es8311_diag_init(&(es8311_diag_cfg_t){ .port = I2C_NUM_0, .addr8 = 0x30, .trigger_gpio = -1 });
es8311_diag_stress("A_no_i2s", 200, NULL);   // 在 I2S 初始化之前
// ... 初始化并 enable I2S ...
es8311_diag_stress("B_i2s_mclk", 200, NULL);
codec_cfg.ctrl_if = es8311_diag_trace_ctrl(); // 替换原来的 ctrl_if，定位失败的寄存器
```

条件 D（ILI9341 刷屏开/关）要在你自己的工程里，分别在刷屏和不刷屏两种状态下各跑一次 `stress`。

## 注意：esp_codec_dev 1.6.2 + IDF ≥ 5.3 + legacy 模式的编译问题

在 IDF 5.5.3 上用官方 1.6.2 原样编译 legacy 模式，会报：

```
audio_codec_ctrl_i2c.c: fatal error: driver/i2c.h: No such file or directory
```

原因是 IDF ≥ 5.3 时，组件的 `priv_requires` 只包含 `esp_driver_i2c`，而 legacy 头文件在 `driver` 组件里。本目录的 example 编译验证时，在组件副本的 `CMakeLists.txt` 里给 `priv_requires` 加了 `driver`。

如果你的工程能干净编译通过，请确认以下两点：

- `build/config/sdkconfig.h` 里是否确实有 `#define CONFIG_CODEC_I2C_BACKWARD_COMPATIBLE 1`。
- `managed_components/espressif__esp_codec_dev` 是否被改过，或者被 `components/` 下的同名组件覆盖了。

另外，legacy 和新驱动两条路径打印的都是 `Fail to write to dev 30`，仅凭日志区分不出走的是哪一条。

## 结果怎么看

| 观察结果 | 指向的原因 |
|---|---|
| A ≈ 0 失败，B 失败多，C 明显改善 | MCLK 或时钟线串扰到 SCL/SDA（硬件/走线） |
| A 也失败 | 与时钟无关：查 I2C 电平、上拉、供电、芯片状态 |
| `疑似复位` > 0（回读与基线不一致） | 芯片掉电或复位：在模块端测 5V 和 LDO 输出 |
| 失败主要是 `ESP_FAIL` | 从机 NACK |
| 失败主要是 `ESP_ERR_TIMEOUT` | 总线被拉住；或者**另一个任务**正占着同一 I2C 端口，legacy 驱动等待总线锁超时也返回这个码 |
| ACK 位置：V0 失败、V1 不失败且回读一致 | 数据字节收到 NACK，但值其实已写入：问题出在第 3 个字节的 ACK 时序（信号完整性） |
| ACK 位置：V1 也失败、V2 不失败 | 寄存器字节就 NACK 了 |
| ACK 位置：V2 也失败 | 地址字节 NACK：芯片当时不响应（复位/掉电/总线干扰） |
| ACK 位置：V1/V2 不报错，但回读不符 | 字节被芯片错收：总线噪声导致数据损坏 |
| 失败集中在固定的某个寄存器 | 和那一步的芯片状态变化有关，而不是随机噪声 |
| 所有条件都 0 失败，但官方初始化仍然失败 | 回到软件：比较 trace 日志与重放的差异，排查多任务并发 |

`trigger_gpio` 设成一个空闲引脚后，每次失败时它会被拉高。示波器设上升沿触发、加预触发，就能抓到失败的那一帧 I2C 波形。
