# T5AI 语音玩具固件 IO 逆向分析记录

> 目标：为把该玩具改成接入「小智 AI」，从原厂固件 `tyutool_read_t5ai.bin` 中找出麦克风、扬声器、按键、充电、电池、LED 等 IO。
> 本文件持续更新，记录每一步研究过程与结论。

---

## 1. 固件基本信息

| 项目 | 值 |
|---|---|
| 文件 | `tyutool_read_t5ai.bin`，8 MB（整片 flash dump） |
| 芯片 | Beken **BK7236**（涂鸦 T5AI 模组），Cortex-M33 双核 |
| 应用固件名 | `tuyaos_demo_ai_toy-f4e781c`，版本 `1.0.31`（涂鸦 TuyaOS AI 玩具 demo 方案） |
| 疑似 PID | `b0aeqpcmldso90vd`（`tuya_app_main.c` 中传给 IoT 初始化） |

### 1.1 Flash 格式
- 0x000000–0x31B000：代码区，每 **34 字节 = 32 字节数据 + 2 字节 CRC16**（多项式 0x8005，初值 0xFFFF，大端存放）。
- 去掉 CRC 后的逻辑地址 = `0x02000000 + 去CRC偏移`（通过字符串指针交叉引用验证）。
- 0x7CB000 起：KV（`useruser`、`psram_id`、`rtc_tv_sec` 等），0x7DD000 区域为涂鸦加密 KV，0x7FD000 为授权区。

### 1.2 分区表（物理地址，位于 bootloader 中）
| 分区 | 起始 | 大小 |
|---|---|---|
| bootloader | 0x000000 | 0x11000 |
| app (CPU0) | 0x011000 | 0x1CB000 |
| app1 (CPU1) | 0x1DC000 | 0x77000 |
| app2 | 0x253000 | 0x22000 |
| download (OTA) | 0x275000 | 0x165000 |

---

## 2. 分析方法
1. Python 去除 CRC → `code.bin`，提取字符串。
2. 字符串定位到应用源文件：`app_key.c`、`tuya_key.c`、`tuya_ai_battery.c`、`tuya_ai_toy.c`、`ty_ai_toy_mf_test.c`、`tuya_app_main.c`。
3. capstone (Thumb) 反汇编，识别 TKL GPIO 接口：
   | 地址 | 函数 | 依据 |
   |---|---|---|
   | 0x020F7F5C | `tkl_gpio_init(pin, cfg)` | 含 "set direct error" |
   | 0x020F7FF8 | `tkl_gpio_write(pin, level)` | |
   | 0x020F8024 | `tkl_gpio_read(pin, *level)` | |
   | 0x020F8050 | `tkl_gpio_irq_init(pin, cfg)` | 含 "GPIO_INT_TYPE_HIGH_LEVEL Not supported" |
   | 0x020F80B8 | `tkl_gpio_irq_enable(pin)` | |
   | 0x020F81B2 | GPIO → ADC 通道映射 | |
   `cfg` 结构为 3 字节 `{mode, direct, level}`；mode: 0=上拉 1=下拉 3=浮空；direct: 0=输入 1=输出。
4. 扫描全部调用点，反推每个引脚的参数和用途。

---

## 3. 板级引脚配置表（关键发现）

`tuya_app_main.c` 从 flash 常量 **0x0201684A** 拷贝 6 字节配置，传给 `app_key_init()` 和 `ai_toy_init()`：

```
{ 12, 33, 19, 8, 21, 2 }
```

另外 `tuya_ai_toy.c` 中电池初始化硬编码 ADC 引脚 **28**。

---

配置表各字节含义（已全部确认）：

| 下标 | 值 | 含义 |
|---|---|---|
| [0] | 12 | 配网/复位键 GPIO |
| [1] | 33 | AI 对话主按键 GPIO |
| [2] | 19 | 扬声器功放 PA 使能 GPIO（作为 `spk_gpio` 传给 `tkl_ai_init`） |
| [3] | 8 | LED GPIO |
| [4] | 21 | 充电检测 GPIO |
| [5] | 2 | 对话触发模式：0=按键触发（读 P33 消抖判断说话），1/2=VAD 语音活动检测模式（日志 "vad start"）；=2 时音频配置首字段置 1，该字段对应 TuyaOpen 中的 `aec_enable`，说明**原厂开启了 AEC 自由对话**（云端参数 `"processing.interrupt":true` 支持打断） |

---

## 4. IO 最终结论

| 功能 | GPIO | 配置 / 极性 | 证据 / 说明 |
|---|---|---|---|
| **配网/复位键**（key0） | **P12** | 输入、内部上拉，**低电平=按下** | `app_key.c`：长按 ≥3 s → "long press, remove device"（`tuya_iot_wf_gw_unactive` 解绑重新配网）；短按只打印 "normal press"。产测 key 表 `{12,33}` → keyID `{0,1}` |
| **AI 对话主按键**（key1） | **P33** | 输入、内部上拉，**低电平=按下** | `tuya_key` 驱动注册 1 个按键：`low_level_detect=1`，长按 3000 ms，连击间隔 200 ms，回调 `key_process`（NORMAL_KEY/SEQ_KEY/LONG_KEY）；进入低功耗时 `tkl_gpio_irq_enable(33)` 作为**唤醒源** |
| **LED 指示灯** | **P8** | 推挽输出（初始化 level=0） | 产测 500 ms 闪烁；网络状态、低电量时写 1/0。**实测为低电平点亮**（2026-10-18；原推测高电平亮是错的，原厂初始化 level=0 即上电点亮）。另有一颗**充电指示灯由充电芯片直接驱动**，不经 GPIO，软件无法控制 |
| **充电检测** | **P21** | 输入、浮空（外部应有上拉） | `is_charging = (read(P21) == 0)` → **低电平=正在充电**（典型充电 IC 的 CHRG 开漏脚）；日志 "%s, in charge"、"battery charging status change to %d" |
| **电池电压检测** | **P28 = ADC4** | ADC，12 bit，**1/4 分压** | `tuya_ai_battery_init` 模式 0（ADC），`pin2adc(28)=通道4`。50 组×8 次去极值平均，码值 −50 校正，引脚电压 ×4 后查表（详见 4.2） |
| **扬声器功放 PA 使能** | **P19** | 推挽输出，**高电平=开启** | `tkl_ai_init` 配置 `spk_gpio=19`、`spk_gpio_polarity=0`；`tkl_ai_start` 中 polarity=0 时输出高，stop 时写回 0；进入低功耗前置低、唤醒后置高 |
| **麦克风** | 模组专用模拟脚 MIC（非 GPIO） | 片内 Audio ADC | `tkl_ai_init` card=1（BOARD 板载 codec，非 UAC），16 kHz / 16 bit / 单声道，mic 增益 0x2F；另有 `tuya_audio_adc_mic2_to_dac_test` 产测项 |
| **扬声器** | 模组专用模拟脚 SPK（非 GPIO） | 片内 Audio DAC → 外部 PA | 播放 16 kHz，spk 增益 0x2F；TTS 为 MP3 16 kHz 单声道 |
| 下载/日志串口 | P10(RX) / P11(TX) | UART0 | `tkl_uart_init` 引脚表：UART0 = P10/P11，UART1 = P1/P0，UART2 = P30/P31 |

### 4.1 电池电压-电量对照表（.data 0x280026F4）
| mV | 3988 | 3970 | 3930 | 3891 | 3839 | 3831 | 3781 | 3720 | 3545 | 3317 | 3230 | 3000 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| % | 100 | 90 | 80 | 70 | 60 | 50 | 40 | 30 | 20 | 10 | 5 | 1 |

- 共 12 个点（表长度由 0x02170C3C 返回常数 12），格式 `{uint32 mV, uint32 %}`，按电压从高到低排列。
- 电量 ≤20% 判为低电量（播放提示音 "battery low, play alert"）。

### 4.2 电量计算流程（`tuya_ai_battery.c`，逆向自 0x0216E32C / 0x0216E238 / 0x0216E148）
1. 采样：`tkl_adc_read` 读 P28（ADC4）8 次，去掉最大、最小值后取平均（÷6），共做 50 组。
2. 滤波：求 50 组的平均值和标准差（单位为 ADC 码值）：
   - 标准差 > 120：数据抖动太大，丢弃，200 ms 后重采；
   - 81 ~ 120：只取落在 平均值±标准差 内的样本重新平均；
   - ≤ 80：直接用平均值。
3. 码值 −50 做偏移校正。
4. 换算电压：`bk 的 saradc 码值转电压函数(code, ch4)` 得到**引脚电压（V）× 1000 × 4** = 电池电压 mV（0x0216E24A 处 `vmul 1000.0f`，0x0216E25A 处 `lsl #2`）。
   → **P28 前的硬件分压比为 1/4**（例如上 300k / 下 100k），满电 3.988 V 时引脚约 1.0 V。
5. 查表：电压 > 3988 mV → 100%；落在两点之间 → **线性插值**；≤ 3000 mV → 0%。
6. 平滑：结果放入 30 格的环形缓冲 `bat_capacity_list[30]`；60 s 定时器取缓冲中非零项平均值，**向下取整到 10 的倍数**后通过回调上报（所以 UI 上电量是 0/10/20…100 跳变）。
7. 充电中（P21 为低）时跳过测量，不更新电量。

- 移植时仍建议用万用表实测一次 P28 电压 ÷ 电池电压，确认 1/4 分压比。

### 4.3 其他行为
- 空闲 120 s（0x1D4C0 ms）进入低功耗，P19 拉低关闭功放，P33 中断唤醒。
- 云端协议：涂鸦 AI 云（`ai.session.new`，ASR+VAD，TTS mp3），换小智后整套替换。

---

## 5. 关键地址索引（便于复查）
| 地址 | 内容 |
|---|---|
| 0x0201684A | 板级 6 字节引脚配置 `{12,33,19,8,21,2}` |
| 0x0216DC80 | `tuya_app_main` 应用入口（拷贝配置，调用 key/ai_toy 初始化） |
| 0x0216DFA4 / 0x0216E070 | `app_key.c` 按键任务 / 初始化 |
| 0x0216E520 | `tuya_ai_battery_init` |
| 0x021725C0 | ai_toy 电池配置（ADC=28，充电检测=21） |
| 0x02172398 | 充电状态回调（读 P21） |
| 0x02172D6C | `ty_ai_toy_start`（音频配置、PA 脚、tkl_ai_init） |
| 0x02173074 | `ai_toy_init`（P33 按键注册、低功耗逻辑） |
| 0x020FC028 / 0x020FC26C | `tkl_ai_init` / `tkl_ai_start`（PA 极性处理） |
| 0x021E6EB4 → 0x28001000 | .data 初始化镜像（RAM 0x28001000 起，长度 0x2194） |
