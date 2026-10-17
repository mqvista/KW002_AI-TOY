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

**串口接线：只用一根 USB-TTL（COM10，CH343）接 UART0**
| 用途 | 串口 | 模组引脚 | 波特率 |
|---|---|---|---|
| 刷写固件 | UART0 | P11 = TX，P10 = RX | tyutool 921600 |
| 运行日志 | UART0（同一个口） | P11 = TX，P10 = RX | 460800 |

T5AI 平台默认把日志放在 UART1（P0/P1，`CONFIG_UART_PRINT_PORT=1`）。板级 Kconfig 加了 `BOARD_LOG_UART_PORT=0` 后，平台的 `build_example.py` 会生成 `tuya_logport.config` 覆盖成 UART0；第一次切换时会自动清掉 `platform/T5AI/t5_os/build/bk7258` 重新生成配置（日志里会显示 `log-uart intent changed (default -> 0)`）。

> 刷写报 `serial I/O: Access is denied` 说明串口被占用，先关掉 tyutool 图形界面、串口助手、`tos.py monitor` 等。

刷写会覆盖片上原厂固件（`*_QIO_*.bin` 从 0 地址写入 bootloader + 应用）；原厂整片 dump 可以用 tyutool 写回恢复。

### 阶段 1：新建板级支持 `KW002_T5AI` 并验证硬件
- [x] 板级代码 `boards/KW002_T5AI/`（`kw002_t5ai.c`、`board_com_api.h`、`Kconfig`、`KW002_T5AI.config`）
- [x] `tools/setup.ps1`：建目录链接 `TuyaOpen\boards\T5AI\KW002_T5AI` → `boards\KW002_T5AI`，往 `TuyaOpen\boards\T5AI\Kconfig` 插入板级选项，拷贝板级 config，写入子模块 `info/exclude`；可重复执行
- [x] 硬件自检应用 `apps/hw_test` 编译通过（`hw_test_QIO_0.1.0.bin`）

板级实现要点：
- 音频：`TDD_AUDIO_T5AI_T` 完全照搬原厂配置（AEC 开、16 kHz、`spk_pin=19`、polarity LOW）。
- 按键：两个键都用**定时扫描 + 内部上拉**（`BUTTON_TIMER_SCAN_MODE` + `TUYA_GPIO_PULLUP`）。TuyaOpen 的中断模式在低电平有效时会配置成**下拉**，板上若没有外部上拉会一直误判按下；原厂也是上拉输入。
- 电源：用 `tdd_power_soc`：电池 ADC 通道 4、分压比 4.0、原厂 12 点电量表；充电检测 P21 低有效；P33 为深睡唤醒源。
- LED：P8 推挽，**低电平亮**（2026-10-18 实测纠正，原定高电平亮）。充电灯是充电芯片直接驱动的独立 LED，与 P8 无关。

参照 `TUYA_T5AI_CORE` 编写：
```c
#define BOARD_SPEAKER_EN_PIN    TUYA_GPIO_NUM_19   // 功放使能，高有效
#define BOARD_BUTTON_TALK_PIN   TUYA_GPIO_NUM_33   // 对话键，低有效
#define BOARD_BUTTON_RESET_PIN  TUYA_GPIO_NUM_12   // 复位/配网键，低有效
#define BOARD_LED_PIN           TUYA_GPIO_NUM_8    // LED（低有效，2026-10-18 实测）
#define BOARD_CHARGE_DET_PIN    TUYA_GPIO_NUM_21   // 低电平=充电中
#define BOARD_BAT_ADC_PIN       TUYA_GPIO_NUM_28   // ADC4，Vbat/4 分压

TDD_AUDIO_T5AI_T cfg = {
    .aec_enable = 1, .ai_chn = TKL_AI_0,
    .sample_rate = TKL_AUDIO_SAMPLE_16K, .data_bits = TKL_AUDIO_DATABITS_16,
    .channel = TKL_AUDIO_CHANNEL_MONO, .spk_sample_rate = TKL_AUDIO_SAMPLE_16K,
    .spk_pin = BOARD_SPEAKER_EN_PIN, .spk_pin_polarity = TUYA_GPIO_LEVEL_LOW,
};
```
#### 首次上机调试记录（2026-10-17）
- 刷写：`tos.py flash -p COM10`（tyutool 3.2.10，921600，2 MB 约 35 s）。CH343 的 RTS 接在模组复位脚上，tyutool 和抓日志脚本都能**自动复位**设备。芯片 ID `0x72360101`（BK7236）。
- **坑：RTS = 复位**。pyserial / `tos.py monitor` / 默认串口助手打开串口时会拉 RTS，芯片被一直按在复位，设备“像没刷进去”、日志 0 字节（刷写后第一次抓日志 0 字节、用户反馈“固件没进去”都是这个原因）。改用 `tools/monitor.py`：打开串口前设 `rts=False, dtr=False`，`-r` 才主动拉一下 RTS 复位。
- 日志：`BOARD_LOG_UART_PORT=0` 生效，UART0 @460800 能看到启动日志。崩溃转储原来仍在 UART1（`CONFIG_DUMP_UART_PRINT_PORT=1`），`tools/setup.ps1` 给平台 `build_example.py` 打了补丁，让崩溃转储也跟到 UART0。
- **崩溃 1**：`MemFault`（CFSR 0x82，空指针），addr2line 定位到 `tal_sw_timer_create` → 应用没调 `tal_sw_timer_init()`。LED 驱动的闪烁依赖软件定时器。所有应用开头都要先 `tal_sw_timer_init(); tal_workq_init();`。
  - 定位方法：`arm-none-eabi-addr2line -f -i -e dist/<app>/debug/bk7258_ap/app.elf <PC> <LR>`（TuyaOpen 应用跑在 AP 核 = CPU1）。
- **电池读数错误**：TuyaOpen `tdd_power_soc` 的默认 T5 模型（1.0 V = 码值 2469，每伏 2429）在本机偏差约 3 倍；本机 P28 稳定码值约 7000。用芯片校准函数 `bk_adc_data_calculate`（efuse，`tkl_adc_read_voltage` 内部也用它）采样得到：码值 3000/5000/7000/9000 → 389/655/922/1189 mV，即 1.0 V = 7583、每伏 7500。板级在**注册电源驱动并做过一次转换后**才计算（该曲线依赖 tkl_adc 设置的饱和模式，提前算会差 3 倍），结果 `cal_low=7583, cal_span=7500`。
- 开机第一次 ADC 转换时码值从 136 慢慢爬升（引脚刚切到模拟输入、分压电容在充电），板级先读一次再等 300 ms。
- 结果：电池 3704 mV（充电中），按原厂电量表约 29%，与原厂算法（校准电压 ×4）一致 → **分压比 1/4 确认**。

用 `apps/hw_test` 逐项验证（刷入后看串口日志）：
- [ ] 开机 LED 亮 2 s、灭 2 s，然后慢闪 → 确认 P8 极性
- [ ] 按住 P33 录音（最长 5 s），松开回放 → 麦克风、功放 P19、喇叭、音量
- [ ] 单击 P12 播放 1 kHz 测试音；长按 3 s 有日志
- [x] 每 5 s 打印电池电压/电量/充电状态：电压正确（3704 mV），充电中 P21 = 低 → charging
- [x] 拔掉充电器 → `discharging`，插回 → `charging`，P21 极性确认（低 = 充电中）
- [x] P33 按下/松开事件正常（扫描模式 + 内部上拉可用），录音后回放
- [x] P12 按下/松开/单击事件正常，单击触发 1 kHz 测试音
- [x] 用户确认：回放能听清，LED 在闪，复位键识别正常
- [x] 音量：用户要求 **100%**（hw_test 已改；小智固件默认音量也用 100）
- [x] 麦克风帧率：空闲时 5101 ms 收到 255 帧 / 163200 字节 = 31993 B/s（理论 32000）
- [x] 用户确认 100% 音量音质没问题
- **发现：原厂 VAD 会掐断麦克风数据**。第一次检测到人声后，每 5 s 帧数掉到 229 → 177 → **0**；第二次按住 3910 ms 只录到 3000 ms（句首被截）。日志里有 `vad_state: silence -> speech_start` 等切换。
  - 排除过程：自动每 10 s 播放 1 s 纯音共 6 次，帧数一直 254~255 → 与播放无关（纯音不会触发人声检测）。
  - 根因：T5AI 平台 `tkl_audio.c` 在 AEC 管道里默认开启原厂 VAD（`vad_en:1`），注释写明“使能原厂 VAD 时，一段时间检测不到声音后底层会停止向应用层输出数据”。
  - 修复：板级在 `tdd_audio_register` 之前调用 `tkl_ai_disable_vendor_vad()`，启动日志变为 `vad_en:0`。唤醒词 / VAD 由应用自己做，小智需要连续的麦克风数据和完整的句首。
  - 另外：`tdl_audio_play` 不阻塞，1 s 音频 0~1 ms 就交出去（平台语音队列可缓存数秒），流式播放 TTS 时要自己控制节奏。
- [x] 关闭原厂 VAD 后复测通过：说话 → 安静约 24 s → 再说话，每 5 s 一直是 254~255 帧（播放时统计周期变长，但仍是 50 帧/秒），不再出现 `vad_state`；按住 4574 ms → 录到 4580 ms，按住 4994 ms → 录到 4980 ms（5 s 录音上限）
- 播放节奏：4580 ms 音频写入耗时 3540 ms，即平台语音队列约缓存 **1 s**，超出部分 `tdl_audio_play` 按播放速度阻塞

**阶段 1 结论：所有 IO 已在真机验证通过**（P33/P12 按键、P19 功放 + 喇叭 100% 音量、麦克风、P8 LED、P21 充电检测、P28 电池 1/4 分压）。
- [ ] `examples/multimedia/audio_kws`：「你好小智」唤醒率

### 阶段 2：配网 + 激活 + 对话闭环（应用 `apps/xiaozhi_toy`）
用户要求阶段 2 直接做完配网和激活码播报（不再写死 Wi-Fi）。配网入口：**从未配网 → 开机直接播报“进入配网模式”并开启配网；已配网 → 长按 P12 3 s，播报后开启配网**。

- [x] SoftAP 配网（`xz_portal.c`）：开机扫描 Wi-Fi → 热点 `Xiaozhi-<MAC 后 4 位>`（无密码）→ TuyaOpen `http_host`/`http_site`/`http_captive` 网页 + 自研 DNS 劫持（`xz_dns.c`）→ 保存到 KV 后重启；新凭据首次连不上自动回配网
- [x] 长按 P12：播报 `wificonfig` → 置 KV 标志 → 重启 → 直接进配网（不重复播报）
- [x] SNTP 对时（`xz_time.c`）+ OTA 返回的 `server_time` 校准
- [x] OTA 签到 / 激活 v1（`xz_ota.c`）：POST `/xiaozhi/ota/`、`/xiaozhi/ota/activate`，证书校验固定 DigiCert Global Root G2
- [x] 激活码语音播报：`activation.ogg` + 数字 0~9（xiaozhi-esp32 提示音，MIT，`tools/gen_prompts.py` 嵌入为 C 数组）
- [x] WebSocket（`xz_ws.c`）：自研 RFC 6455 客户端，单 I/O 线程独占 TLS，自定义头 `Authorization/Protocol-Version/Device-Id/Client-Id`
- [x] 协议：`hello`（opus 16 kHz/60 ms，不声明 MCP）→ `listen start (auto)` → `stt`/`tts start|sentence_start|stop` → 播完自动继续聆听；说话时按 P33 发 `abort` 打断；聆听时按 P33 结束；30 s 无识别自动关闭
- [x] 音频（`xz_audio.c`）：麦克风 20 ms 帧 → Opus 60 ms（复杂度 3）上行；TTS/提示音共用解码播放线程（下行 24 kHz 直接解码到 16 kHz）
- [x] **真机多轮对话通过**（2026-10-18）：“你好。”→“哈喽～这么晚还没睡呀…”；“你叫什么名字？”→“我叫小柔啦…”；问能力 → 列举聊天/笑话/音乐/天气/新闻等
- [ ] 手机配网页自动弹出（DNS 补丁后待复测）

#### 阶段 2 调试记录
| 现象 | 根因 | 处理 |
|---|---|---|
| 热点名 `XiAozhi-5C90` | MAC 转大写时把前缀也转了 | 只转后缀 |
| 打开 192.168.4.1 显示“无法访问”（设备日志显示请求已到、页面已发） | `http_host` 处理完立即 close，T5 lwIP 发 RST，浏览器收到 reset 且 0 字节（电脑连热点用 curl 复现） | 发完响应后等客户端读完并关闭（`Connection: close`，最长 1.5 s）再返回 |
| 改用 `shutdown(SHUT_WR)` 后设备 MemFault | 该 lwIP 在 `lwip_netconn_do_close_internal` 空指针（addr2line 定位） | 不用 shutdown |
| 手机不弹配网页，设备收不到任何 DNS 查询 | 平台 DHCP 服务默认启动 “nack DNS”，对所有查询回 **REFUSED**，且绑定 AP 网卡优先接收 | `setup.ps1` 平台补丁注释掉 `dhcp_enable_nack_dns_server()`；应用 DNS 绑定 AP IP |
| OTA HTTPS 失败 `mbedtls_ssl_handshake returned 0x1c` | -0x1C = `MBEDTLS_ERR_THREADING_BAD_INPUT_DATA`：没调 `tuya_tls_init()`（正常由 `tuya_iot_init` 调用），mbedtls 互斥锁未注册 | 应用启动时调用 `tuya_tls_init()` |
| 对时后 `time()` 仍为 -1 | 平台 newlib `time()` 无 gettimeofday 后端，恒 -1；`settimeofday` 也不存在 | 应用内提供 `time()`（取 `tal_time_get_posix()`），并调用 `tal_time_service_init()`；RTC 用 `bk_rtc_settimeofday` |
| 监视脚本中文乱码 | Python 在 Windows 控制台的输出编码问题，设备输出的 UTF-8 是正确的（hexdump 确认） | 用 bash 直接读日志 |

### 阶段 3：完整交互
- [x] 状态机（idle/opening/listening/speaking/offline，`app_main.c`）
- [x] 激活流程：语音播报 6 位激活码（阶段 2 已完成）
- [x] 说话时按键打断（`abort`）
- [x] **回答中侦听麦克风、语音插话**（`XZ_LISTEN_REALTIME=1`，2026-10-18 真机通过）：会话开始发一次 `listen start(mode=realtime)`，麦克风全程上传，T5 硬件 AEC 去除喇叭回声，服务器检测到用户开口即停止当前 TTS。实测：讲故事中途说“换一个故事”“我不要小男孩，我要小女孩”都被正确打断并改答；100% 音量下未出现被自身回声误打断
  - 播放中收到新的 `stt` 时清空本地缓冲的旧回答（实测服务器通常先发 `tts stop`，此逻辑作为兜底）
  - 不能在 `tts start` 时清空缓冲：JSON 经主线程队列处理，语音包直接送播放，会误删新回答开头
- [x] **唤醒词「小智同学」**（`xz_wake.c`，2026-10-18 真机通过）：**平台模型里没有「你好小智」**，见下文「唤醒词调研」。实测：待机说「小智同学」→ 同一秒建链 → 服务器回「哎，我在呢～怎么啦？」，唤醒到出声约 1 s；说「退一下吧」服务器主动关闭会话 → 回到待机后可再次唤醒

#### 唤醒词调研（2026-10-18）
- T5AI 平台的 KWS 是 `tkl_kws.c` + 预编译库 `libtutuClear_wakeup_nihaotuya_xiaozhitongxue_heytuya_hituya_20251024_small_model.a`（Spectimbre TUTUClear 2.8.0），平台 CMake 已无条件链接。启动日志：`WAKEUP_info:nihaotuya-xiaozhitongxue-heytuya-hituya--200KB--20250804`，PSRAM 占用 18808 字节。
- 模型只认 4 个词：`你好涂鸦`(1)、`小智同学`(2)、`hey tuya`(3)、`hi tuya`(4)。头文件枚举里有 `TKL_KWS_WAKEUP_NIHAO_XIAOZHI`，但模型里没有，**无法用「你好小智」**（换词需要涂鸦/思必驰定制模型，或换别的 KWS 引擎）。
- 喂数据：TuyaOpen 的 `ai_components` 通过 `tkl_vad.c` 在 AEC 线程里挂 speex AES + RNN VAD，再调用 `tkl_kws_feed_with_vad()`（只在 VAD 判为语音时送检）。本应用已从 T5 音频驱动拿到 AEC 后的 16 kHz 帧，因此不挂那条链路，而是在麦克风回调里直接 `tkl_kws_feed_with_vad(pcm, len, 1)`（VAD 恒为 1，每 100 ms 唤醒一次 `kws_task` 检测）。`tkl_kws_feed_with_vad` 未在公开头文件声明，需 `extern`。
- 只在 **idle 且喇叭不出声** 时开启检测（离开 idle 立即关；idle 时由 `__periodic` 按 `!xz_audio_is_playing()` 开启，避免会话被服务器关闭后告别语还在播时自唤醒）：对话中 realtime 模式本来就能直接说话插话。`tkl_kws_disable/enable` 会清空环形缓冲，旧音频不会延迟触发。
- 模型对另外 3 个词有误报：实测待机时普通说话两次报出 `heytuya`(3)、`hituya`(4)，已在回调里只接受 `小智同学`(2)，其余仅打 DEBUG 日志。
- 唤醒流程（参照 xiaozhi-esp32 `HandleWakeWordDetectedEvent`，其 `CONFIG_SEND_WAKE_WORD_DATA` 默认 y）：播放 `popup` 提示音 → 建立 WebSocket → 收到 `hello` 后先发 `{"type":"listen","state":"detect","text":"小智同学"}`（服务器据此回一句问候）→ 再发 `listen start (realtime)`。未上传唤醒词音频本身（ESP32 用于声纹，此处不需要）。`XZ_WAKE_SEND_DETECT=0` 时只响提示音直接聆听。
- 平台代码小问题（未改）：`__kws_detect()` 内层查表循环复用了外层变量 `i`，命中未知 ID 时会跳过后续帧，对已知 4 个词无影响。
- [x] LED（P8，仅 idle 时）：平时灭，低电每 3 s 短闪 100 ms（`xz_ui.c`）。**不做充电指示**：充电灯是充电芯片直接驱动的独立 LED。
  - 实测纠错：P8 实为**低电平点亮**。按高电平亮写时，拔掉充电线后软件发“灭”灯反而亮了（`led_set_status: 0`），据此改板级 `BOARD_LED_ACTIVE_LV = LOW`。此前 idle 时 P8 其实一直是亮的
- [x] 低电提示音：≤20% 播一次 `low_battery.ogg`、≤10% 再播一次，只在 idle 且喇叭空闲时播；充电或回升到 ≥25% 后重新布防（实机尚未放电到 20% 验证）
- [x] **插拔充电器语音提示**（2026-10-18 真机通过）：插入播「开始充电」、拔出播「充电器已拔出」，连续插拔 4 次各报一次
  - 主循环轮询 `xz_battery` 缓存的充电状态（由 `pwr_chg` 线程回调更新），与上次播报状态不同且**稳定 800 ms** 才播报：插线瞬间 CHRG 会抖动（实测 1 s 内出现“断开→接通”两次回调）；不走事件队列，开机流程中丢事件也不会失步；开机时不报当前状态；小智正在说话时延后到说完再播
  - 语音素材：xiaozhi-esp32 没有充电相关语音，用 Windows 自带 zh-CN 语音「Microsoft Huihui Desktop」（System.Speech，离线）合成，`soundfile`（libsndfile 1.2.2）编码为 Ogg/Opus 16 kHz 单声道 20 ms 帧，响度按 `wificonfig.ogg` 的有声段 RMS 对齐：`apps/xiaozhi_toy/tools/tts_prompts.py` → `tools/gen_prompts.py`
  - 原有提示音格式（解析 OpusHead/TOC）：16 kHz 单声道，SILK WB 60 ms 帧（config 11）；播放器对帧长无要求
- [ ] 提示音：断网
- [x] **MCP 工具**（`xz_mcp.c`，hello 声明 `"features":{"mcp":true}`），工具名与参数与 xiaozhi-esp32 一致：
  - `self.get_device_status` → `{"audio_speaker":{"volume":100},"battery":{"level":100,"charging":true,"voltage":4.01},"network":{"type":"wifi","ssid":"…","signal":"strong"}}`（电压是在用户问“电压是多少”答不上来后加的；先用 `voltage_mv`，小智答“4010 毫伏”，改成以伏为单位、两位小数）
  - `self.audio_speaker.set_volume`（0~100，立即生效并存 KV `xz_vol`，开机读取）
  - **语音调音量真机通过**（2026-10-18）：“把音量调小一点”→ 先 `get_device_status`（100）再 `set_volume 40`；“调到 10%”→ 10；“调到 100%”→ 100；“调到 80%”→ 80；“看一下当前音量”→ 80
  - **语音查电量真机通过**（2026-10-18）：“你现在的电池电量是多少？”→ 服务器调用 `self.get_device_status` →“电量是满的喔，百分之百，而且还在充电中呢～”

#### 电池 / MCP 实现要点（2026-10-18）
- `xz_battery.c`：每 10 s 取一次 `tdl_power_battery_get_voltage`（板级已做 16 次平均 + efuse 校准），**只在喇叭停播 2 s 后采样**（100% 音量时功放会把电池电压拉低，避免误判低电），最近 6 次（1 分钟）平均后查原厂 12 点 OCV 表线性插值。
- 充电中端电压偏高（实测插着充电 4005 mV → 100%），电量照常上报但不做低电判断；P21 无“充满”信号。
- 充电状态变化由 `tdl_power_charger_on_event` 回调（在 `pwr_chg` 线程里），投递到主线程事件队列。
- MCP 交互顺序（实测）：hello 后服务器立即发 `initialize`、`tools/list`，对话中按需 `tools/call`；回复包成 `{"session_id":…,"type":"mcp","payload":{JSON-RPC}}`，工具结果为 `{"content":[{"type":"text","text":"…"}],"isError":false}`。
- 音量映射（平台 `tkl_ao_set_vol`）：0~30 → DAC 0~50%，30~100 → 50~70%；0 为静音。
- TuyaOpen 日志格式化不支持 `%%`（输出成 `?`），日志里用 `pct`。

### 阶段 4：电池、功耗
- [x] SoftAP 配网（阶段 2 已完成）
- [x] 电池：电量采样/平滑、低电播报、低电 LED、语音查询（见阶段 3）
- [ ] 低功耗：空闲超时关功放（P19 拉低），P33 唤醒
- [x] 音量记忆（KV `xz_vol`，语音设置后保存）

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
| 对话方式 | **唤醒词 + P33 按键 + AEC 自由对话**，播报中可打断。唤醒词原定「你好小智」，平台离线模型不含该词，改为「小智同学」（2026-10-18） |
| 工程位置 | **本地 `D:\tuya-T5`** = 仓库 [mqvista/KW002_AI-TOY](https://github.com/mqvista/KW002_AI-TOY)（公开），TuyaOpen 为子模块；板级 `boards/KW002_T5AI`、应用 `apps/xiaozhi_toy`。原定放在 Z:（NAS），但 `export.ps1` 在网络盘上失败（uv 无法创建目录链接，os error 4390），改到本地盘 |

## 5. 风险
| 风险 | 应对 |
|---|---|
| 官方服务器对非 ESP32 设备的兼容性未知（激活 v1 协议） | 先用 Python 客户端协议抓包对照；不行就切自建服务器 |
| T5 上 Opus 编码占 CPU/内存 | 使用 TuyaOpen 的 IPC 编码（跑在 CPU1）；降低复杂度 |
| AEC 效果依赖原厂硬件回采设计 | 原厂已启用 AEC，问题不大；不理想时退回按键对话 |
| 刷坏 / 授权区被覆盖 | 保留整片 dump，可随时刷回原厂 |
| LED 极性、分压比为推断值 | 分压比阶段 1 实测确认；LED 极性推断错误，2026-10-18 实测纠正为低电平亮 |

## 参考
- TuyaOpen：https://github.com/tuya/TuyaOpen（板级示例 `boards/T5AI/TUYA_T5AI_CORE`）
- 小智官方固件与协议：https://github.com/78/xiaozhi-esp32（`docs/websocket_zh.md`、`main/ota.cc`）
