# T5AI 语音玩具移植「小智 AI」计划

> 依据：`firmware_io_analysis.md`（原厂固件 IO 逆向结果）+ TuyaOpen / xiaozhi-esp32 源码调研（2026-10-17）。
> 本文件随进度持续更新。

---

## 1. 调研结论

### 1.1 现成方案情况
- **小智官方固件（78/xiaozhi-esp32）只支持 ESP32 系列**，没有 T5/BK7258 的官方移植；社区也没找到成熟的 T5AI 小智固件。
- **TuyaOpen**（涂鸦开源 SDK，github.com/tuya/TuyaOpen）完整支持 T5AI，且已有：
  - 板级驱动框架（音频 codec、按键、LED、ADC、GPIO）；
  - **Opus 编码器**（libopus，可在 CPU1 上经 IPC 运行）和 **Opus/OggOpus 解码器**；
  - 本地唤醒词，**包含「你好小智」「小智同学」「小智管家」**；
  - AEC 回声消除、VAD；
  - mbedTLS、HTTP 客户端、cJSON、KV 存储、Wi-Fi 管理。
- 原厂固件就是 TuyaOS（TuyaOpen 的前身），两者的 TKL 驱动层一致。

### 1.2 原厂音频配置与 TuyaOpen 完全对应
原厂 `tkl_ai_init` 的配置可以直接对应到 TuyaOpen 的 `TDD_AUDIO_T5AI_T`：

| TuyaOpen 字段 | 原厂固件取值 | 说明 |
|---|---|---|
| `aec_enable` | **1** | 原厂开启了 AEC → 硬件支持回声消除，可做“边播边听/打断” |
| `ai_chn` | `TKL_AI_0` | |
| `sample_rate` / `data_bits` / `channel` | 16 kHz / 16 bit / 单声道 | |
| `spk_sample_rate` | 16 kHz | |
| `spk_pin` | **19** | 功放使能 |
| `spk_pin_polarity` | `TUYA_GPIO_LEVEL_LOW`(0) | 与 TuyaOpen 官方 T5AI 板写法一致，实际效果为高电平开功放 |

### 1.3 小智协议要点（xiaozhi-esp32 `docs/websocket_zh.md`、`main/ota.cc`）
1. **OTA/激活**：`POST https://api.tenclass.net/xiaozhi/ota/`
   - 请求头：`Device-Id`(MAC)、`Client-Id`(UUID)、`Activation-Version: 1`、`User-Agent`、`Accept-Language`；
   - 请求体：设备信息 JSON；
   - 响应：`websocket{url, token}`，未绑定时带 `activation{code, message}`（6 位验证码，用户在 xiaozhi.me 控制台输入绑定）。
2. **WebSocket**：带 `Authorization: Bearer <token>`、`Protocol-Version`、`Device-Id`、`Client-Id` 请求头连接；
   - 设备发 `hello`（opus、16 kHz、单声道、60 ms 帧）→ 服务器回 `hello`（下行通常 24 kHz）；
   - 文本帧 JSON：`listen`(start/stop/detect)、`abort`、`stt`、`llm`(表情)、`tts`(start/sentence_start/stop)、`mcp`(JSON-RPC 设备控制)；
   - 二进制帧：Opus 音频包。
3. 下行 24 kHz 的 Opus 可让解码器直接按 16 kHz 输出，不需要另外重采样。

---

## 2. 总体方案（推荐）

**基于 TuyaOpen 新建独立应用 `apps/xiaozhi_toy`（放在仓库里，不放进 TuyaOpen 子模块），复用底层组件，自己实现小智协议和状态机，不依赖涂鸦云。**

```
┌──────────────── xiaozhi_toy 应用（新写）────────────────┐
│ 状态机: 空闲/连接中/聆听/说话/激活/配网/低电             │
│ 小智协议: OTA激活(HTTPS) + WebSocket(JSON+Opus) + MCP     │
│ 配网: SoftAP + 网页（不依赖涂鸦 App 和授权码）            │
├──────────────── TuyaOpen 复用组件 ──────────────────────┤
│ tdl_audio(AEC/VAD) │ 唤醒词“你好小智” │ libopus 编解码   │
│ tdl_button / LED   │ ADC 电池         │ TLS/HTTP/cJSON/KV │
├──────────────── 新板级 KW002_T5AI（新写）────────────────┤
│ P19 功放  P33 对话键  P12 复位键  P8 LED  P21 充电  P28 电池│
└──────────────────────────────────────────────────────────┘
```

为什么不直接改 TuyaOpen 的 `your_chat_bot`：它的 `ai_mode_*` / `ai_chat_main` 直接调用 `tuya_ai_agent_*`、`tuya_ai_input_*`，并且要等涂鸦云 MQTT 连接成功，还需要 TuyaOpen 授权码（UUID/AuthKey）。改造它的工作量和风险都比新写一个精简应用大。

---

## 3. 分阶段实施

### 阶段 0：环境与安全网
- [ ] 保留原厂完整 dump `tyutool_read_t5ai.bin`（含涂鸦授权区，用于随时刷回原厂；目前在 `Z:\tuya-T5`，不进公开仓库）；另存一份只读副本
- [x] 建仓库 https://github.com/mqvista/KW002_AI-TOY，本地 `D:\tuya-T5`；TuyaOpen 作为子模块（commit `cdc064e`）
- [x] Windows 下 `export.ps1` 配置环境（uv + Python 3.12 + make，装在 `TuyaOpen\.tools`、`.venv`，已被子模块忽略）
- [x] 编译 `examples/get-started/sample_project`（T5AI）成功；首次编译自动下载 T5AI 平台 SDK（约 700 MB）和 gcc-arm-none-eabi 10.3
- [ ] 刷写并看日志，打通 **刷写(UART0: P10/P11) → 看日志** 流程
- [ ] 验证可以用 tyutool 把原厂 dump 整片刷回（恢复路径可用）

> 编译日志中的 `country code error: ...ip-api.com` 是 tos.py 查询所在国家失败，不影响编译。

**串口接线**（TuyaOpen T5AI 平台配置 `CONFIG_UART_PRINT_PORT=1`、460800；引脚来自原厂固件 UART 表）：
| 用途 | 串口 | 模组引脚 | 波特率 |
|---|---|---|---|
| 刷写固件 | UART0 | P11 = TX，P10 = RX | tos.py / tyutool 自动 |
| 运行日志 | UART1 | P0 = TX，P1 = RX | 460800 |

刷写会覆盖片上原厂固件（`*_QIO_*.bin` 从 0 地址写入 bootloader + 应用）；原厂整片 dump 可以用 tyutool 写回恢复。

### 阶段 1：新建板级支持 `KW002_T5AI` 并验证硬件
- [x] 板级代码 `boards/KW002_T5AI/`（`kw002_t5ai.c`、`board_com_api.h`、`Kconfig`、`KW002_T5AI.config`）
- [x] `tools/setup.ps1`：建目录链接 `TuyaOpen\boards\T5AI\KW002_T5AI` → `boards\KW002_T5AI`，往 `TuyaOpen\boards\T5AI\Kconfig` 插入板级选项，拷贝板级 config，写入子模块 `info/exclude`；可重复执行
- [x] 硬件自检应用 `apps/hw_test` 编译通过（`hw_test_QIO_0.1.0.bin`）

板级实现要点：
- 音频：`TDD_AUDIO_T5AI_T` 完全照搬原厂配置（AEC 开、16 kHz、`spk_pin=19`、polarity LOW）。
- 按键：两个键都用**定时扫描 + 内部上拉**（`BUTTON_TIMER_SCAN_MODE` + `TUYA_GPIO_PULLUP`）。TuyaOpen 的中断模式在低电平有效时会配置成**下拉**，板上若没有外部上拉会一直误判按下；原厂也是上拉输入。
- 电源：用 `tdd_power_soc`：电池 ADC 通道 4、分压比 4.0、原厂 12 点电量表；充电检测 P21 低有效；P33 为深睡唤醒源。
- LED：P8 推挽，暂定高电平亮（待实测）。

参照 `TUYA_T5AI_CORE` 编写：
```c
#define BOARD_SPEAKER_EN_PIN    TUYA_GPIO_NUM_19   // 功放使能，高有效
#define BOARD_BUTTON_TALK_PIN   TUYA_GPIO_NUM_33   // 对话键，低有效
#define BOARD_BUTTON_RESET_PIN  TUYA_GPIO_NUM_12   // 复位/配网键，低有效
#define BOARD_LED_PIN           TUYA_GPIO_NUM_8    // LED（高有效，待验证）
#define BOARD_CHARGE_DET_PIN    TUYA_GPIO_NUM_21   // 低电平=充电中
#define BOARD_BAT_ADC_PIN       TUYA_GPIO_NUM_28   // ADC4，Vbat/4 分压

TDD_AUDIO_T5AI_T cfg = {
    .aec_enable = 1, .ai_chn = TKL_AI_0,
    .sample_rate = TKL_AUDIO_SAMPLE_16K, .data_bits = TKL_AUDIO_DATABITS_16,
    .channel = TKL_AUDIO_CHANNEL_MONO, .spk_sample_rate = TKL_AUDIO_SAMPLE_16K,
    .spk_pin = BOARD_SPEAKER_EN_PIN, .spk_pin_polarity = TUYA_GPIO_LEVEL_LOW,
};
```
用 `apps/hw_test` 逐项验证（刷入后看串口日志）：
- [ ] 开机 LED 亮 2 s、灭 2 s，然后慢闪 → 确认 P8 极性
- [ ] 按住 P33 录音（最长 5 s），松开回放 → 麦克风、功放 P19、喇叭、音量
- [ ] 单击 P12 播放 1 kHz 测试音；长按 3 s 有日志
- [ ] 每 5 s 打印电池电压/电量/充电状态 → 对比万用表，修正分压比；插拔充电器看 P21 状态变化
- [ ] `examples/multimedia/audio_kws`：「你好小智」唤醒率

### 阶段 2：小智协议最小闭环（先写死 Wi-Fi）
- [ ] Wi-Fi 连接（SSID/密码先写在配置里）
- [ ] OTA 请求：HTTPS POST，解析 websocket url/token/激活码，存 KV
- [ ] WebSocket 客户端：TLS + 自定义请求头 + 帧收发（TuyaOpen 自带的 websocket_transporter 不支持自定义头，打算在 TLS 传输层上自己写一个轻量实现）
- [ ] `hello` 握手 → 按键触发 `listen start` → 上行 Opus(16 kHz/60 ms) → 收 `stt` / `tts` → 下行 Opus 解码播放
- [ ] 跑通“按住 P33 说话 → 小智回答”

### 阶段 3：完整交互
- [ ] 状态机（参照 xiaozhi-esp32 `application.cc` / `device_state_machine.cc`）
- [ ] 唤醒词「你好小智」+ AEC 自由对话 + 播报时打断（`abort`）
- [ ] LED 状态：配网闪烁 / 聆听常亮 / 说话呼吸 / 低电慢闪 / 充电
- [ ] 激活流程：设备无屏幕 → **语音播报 6 位激活码**（需要 0~9 数字提示音资源）
- [ ] 提示音：开机、配网、连网成功、断网、低电（可复用 TuyaOpen 本地中文提示音）
- [ ] MCP 工具：音量调节、电量查询（`self.audio_speaker.set_volume`、`self.get_battery` 等）

### 阶段 4：配网、电池、功耗
- [ ] SoftAP 配网：热点 `Xiaozhi-XXXX` + 网页填 Wi-Fi；P12 长按 3 s 重新配网（沿用原厂习惯）
- [ ] 电池：P28 采样 → ×4 → 查原厂电压表（见 `firmware_io_analysis.md` 4.1）；P21 充电检测；低于 20% 播报
- [ ] 低功耗：空闲超时关功放（P19 拉低），P33 唤醒
- [ ] 音量记忆（KV）

### 阶段 5：稳定性与调优
- [ ] AEC/VAD 参数、麦克风增益、功放音量调校
- [ ] 断线重连、服务器超时、长时间运行内存泄漏测试
- [ ] （可选）OTA 固件升级

---

## 4. 已确认的决策（2026-10-17）
| 项目 | 决定 |
|---|---|
| 服务器 | **官方 xiaozhi.me**（OTA 激活 v1，设备语音播报 6 位激活码，在控制台绑定） |
| 配网 | **SoftAP 网页配网**（长按 P12 进入；热点 + 网页填写 Wi-Fi），不依赖涂鸦云 |
| 对话方式 | **唤醒词「你好小智」+ P33 按键 + AEC 自由对话**，播报中可打断 |
| 工程位置 | **本地 `D:\tuya-T5`** = 仓库 [mqvista/KW002_AI-TOY](https://github.com/mqvista/KW002_AI-TOY)（公开），TuyaOpen 为子模块；板级 `boards/KW002_T5AI`、应用 `apps/xiaozhi_toy`。原定放在 Z:（NAS），但 `export.ps1` 在网络盘上失败（uv 无法创建目录链接，os error 4390），改到本地盘 |

## 5. 风险
| 风险 | 应对 |
|---|---|
| 官方服务器对非 ESP32 设备的兼容性未知（激活 v1 协议） | 先用 Python 客户端协议抓包对照；不行就切自建服务器 |
| T5 上 Opus 编码占 CPU/内存 | 使用 TuyaOpen 的 IPC 编码（跑在 CPU1）；降低复杂度 |
| AEC 效果依赖原厂硬件回采设计 | 原厂已启用 AEC，问题不大；不理想时退回按键对话 |
| 刷坏 / 授权区被覆盖 | 保留整片 dump，可随时刷回原厂 |
| LED 极性、分压比为推断值 | 阶段 1 实测确认 |

## 参考
- TuyaOpen：https://github.com/tuya/TuyaOpen（板级示例 `boards/T5AI/TUYA_T5AI_CORE`）
- 小智官方固件与协议：https://github.com/78/xiaozhi-esp32（`docs/websocket_zh.md`、`main/ota.cc`）
