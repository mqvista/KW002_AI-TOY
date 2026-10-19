# KW002 AI-TOY → 小智 AI

把一款基于 **涂鸦 T5AI（BK7236）模组** 的语音玩具（原厂固件 `tuyaos_demo_ai_toy` 1.0.31）改造成接入 **小智 AI（xiaozhi.me）** 的对话玩具。

## 目录结构

| 路径 | 说明 |
|---|---|
| `docs/firmware_io_analysis.md` | 原厂固件逆向：flash 格式、分区表、全部 IO（按键/功放/LED/充电/电池）、电池电量表 |
| `docs/xiaozhi_port_plan.md` | 移植方案、分阶段任务清单、已确认决策、风险 |
| `TuyaOpen/` | 涂鸦开源 SDK（git 子模块，固定版本） |
| `tools/fw_re/` | 固件逆向用 Python 脚本（去 CRC、Thumb 反汇编、交叉引用） |

## 硬件 IO 速查

| 功能 | GPIO | 说明 |
|---|---|---|
| 对话主按键 | P33 | 上拉，低电平按下；低功耗唤醒源 |
| 配网/复位键 | P12 | 上拉，低电平按下；长按 3 s 重新配网 |
| 功放使能 | P19 | 高电平开 |
| LED | P8 | 低电平亮；P8 是 PWM2（TuyaOpen `TUYA_PWM_NUM_6`），用 PWM 驱动做呼吸灯（充电灯另由充电芯片直接驱动） |
| 充电检测 | P21 | 低电平 = 充电中 |
| 电池电压 | P28 (ADC4) | 1/4 分压，引脚电压 ×4 = 电池电压 |
| 麦克风 / 喇叭 | 模组 MIC / SPK 模拟脚 | 片内 codec，16 kHz，开启 AEC |
| 下载 + 日志串口 | P10 RX / P11 TX | UART0；刷写 921600，日志 460800（板级 `BOARD_LOG_UART_PORT=0`，一根 USB-TTL 即可） |

## 固件下载

预编译固件见 [GitHub Releases](https://github.com/mqvista/KW002_AI-TOY/releases)。首次安装或串口刷机使用 `xiaozhi_toy_QIO_0.1.0.bin`，从地址 `0x0` 写入；`UG` 文件用于对应 T5 OTA 升级流程，不能当完整镜像从零地址刷入。各版本附有 SHA-256 文件校验清单和刷写说明。

## 开始

```powershell
git clone --recurse-submodules https://github.com/mqvista/KW002_AI-TOY.git
cd KW002_AI-TOY
powershell -ExecutionPolicy Bypass -File tools\setup.ps1   # 把 boards\ 接入 TuyaOpen（可重复执行）
. .\TuyaOpen\export.ps1                                     # 首次会下载 uv / Python / 依赖

# 编译硬件自检固件（首次会下载 T5AI 平台 SDK，约 700 MB）
cd apps\hw_test
tos.py build          # 输出 dist\hw_test_0.1.0\hw_test_QIO_0.1.0.bin
tos.py flash -p COM10                                       # 通过 UART0 (P10/P11) 刷写，自动复位
..\..\TuyaOpen\.venv\Scripts\python.exe ..\..\tools\monitor.py -r   # 看日志（-r 先复位看启动过程）
```

> **不要用 `tos.py monitor` 或默认设置的串口助手**：这根 USB-TTL（CH343）的 RTS 接在模组复位脚上，按默认方式打开串口会拉住 RTS，芯片一直处于复位、看起来像“固件没进去”。用 `tools/monitor.py`（打开串口时释放 RTS/DTR），或者在串口助手里把 RTS/DTR 都关掉。

本项目默认工作流程：固件修改并编译通过后直接烧录，跳过回读校验，不执行恢复 Flash 保护状态这一步，以烧录应答和启动日志确认结果；结束后释放 RTS/DTR 并关闭串口。详见 `AGENTS.md`。

| 应用 | 说明 |
|---|---|
| `apps/hw_test` | 硬件自检：LED 极性、按住 P33 录音松开回放、P12 播放测试音、每 5 s 打印电池/充电状态 |
| `apps/xiaozhi_toy` | 小智 AI 固件（官方 xiaozhi.me 服务器）：首次开机或长按 P12 3 s 进入 SoftAP 网页配网（播报要连接的热点名，手机连上自动弹出配网页）、语音播报激活码；待机时说「小智同学」或按 P33 开始对话，回答中可直接说话插话；可语音查询电量/电压、联网状况、调节音量（掉电保存）；低电量、插拔充电器、断网/恢复有语音提示；LED 待机呼吸、聆听常亮、说话闪烁 |

> 工程必须放在**本地磁盘**：TuyaOpen 的环境脚本需要创建目录链接，网络共享盘（SMB/NAS）不支持。

说「小智同学」唤醒后，只响提示音并直接聆听，不再主动播报语音问候。

联网后可说「小智同学，查询联网状况」「现在连的是哪个 Wi-Fi？」「信号怎么样？」「IP 地址是多少？」。查询返回 Wi-Fi 连接状态、热点名、IP/网关、信号强度和小智服务连接状态。Wi-Fi 连上但小智连接未建立时，互联网状态显示为未验证；断网期间无法使用云端语音查询，仍保留原有断网语音提示。

还可说「查询公网 IP」「外网 IP 是多少？」。设备按需通过 HTTPS 请求 [ipify](https://www.ipify.org/)，返回当前网络的公网 IPv4 出口地址；它可能是运营商 NAT 或 VPN 出口，与局域网 IP、路由器 WAN 地址不同。请求失败会报告查询失败，不使用旧结果；空闲时不后台查询。

## 注意

原厂 flash dump（`*.bin`）**不放进仓库**，里面有设备授权信息和原厂代码，请自行在本地备份，用来刷回原厂固件。

## 低功耗待机

`xiaozhi_toy` 的功放只在播放语音时开启，音频缓冲播放完并保留 200 ms 尾音余量后关闭；静音时也关闭功放，不改变保存的音量。

电池供电且空闲 5 分钟后自动深睡，充电中、播放、对话、配网和激活时不进入自动深睡。深睡后按 P33 唤醒，重新启动联网后恢复待机；深睡期间不支持语音唤醒。P21 只有充电状态，充满后仍插着电时可能重新开始休眠计时。

配置、平台限制与验收步骤见 [低功耗实现说明](docs/low_power_standby.md)。
