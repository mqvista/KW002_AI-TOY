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
| LED | P8 | 推挽输出 |
| 充电检测 | P21 | 低电平 = 充电中 |
| 电池电压 | P28 (ADC4) | 1/4 分压，引脚电压 ×4 = 电池电压 |
| 麦克风 / 喇叭 | 模组 MIC / SPK 模拟脚 | 片内 codec，16 kHz，开启 AEC |
| 下载串口 | P10 RX / P11 TX | UART0 |

## 开始

```powershell
git clone --recurse-submodules https://github.com/mqvista/KW002_AI-TOY.git
cd KW002_AI-TOY\TuyaOpen
. .\export.ps1        # 首次会下载 uv / Python / 依赖
```

> 工程必须放在**本地磁盘**：TuyaOpen 的环境脚本需要创建目录链接，网络共享盘（SMB/NAS）不支持。

## 注意

原厂 flash dump（`*.bin`）**不放进仓库**，里面有设备授权信息和原厂代码，请自行在本地备份，用来刷回原厂固件。
