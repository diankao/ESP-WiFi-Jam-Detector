# `docs/` — 项目设计规范、USB 协议说明与演示媒体

[⬅ 返回根目录文档](../README_CN.md)

本目录存放项目的原始设计任务书（M0–M6 开发里程碑）、USB 串口通信协议定义以及真机运行截图与微波炉实测视频。

---

## 📂 文件列表

```text
docs/
├── BRIEF.md                # 项目完整设计需求书（硬件选型、架构约束、M0~M6 里程碑验收标准）
├── CLAUDE.md               # 开发铁律与各里程碑硬件实测状态记录
├── LINK.md                 # USB Serial/JTAG JSON Lines 双向通信协议规范（v1）
└── media/                  # 真机实测演示动图与视频
    ├── status.gif          # 状态主屏幕演示（双频 NORMAL）
    ├── waterfall-2g4.gif   # 2.4 GHz 瀑布图演示（可见 1/6/11 信道突发 Wi-Fi 流量）
    ├── waterfall-5g.gif    # 5 GHz 瀑布图演示（140~165 信道活动）
    ├── microwave-2g4.gif   # 家用微波炉运行测试动图（2.4 GHz 瀑布图出现工频脉冲宽带能量，触发 SUSPECT）
    ├── microwave-2g4.mp4   # 微波炉测试完整 33 秒带声音视频
    ├── settings-*.jpg      # 三个设置标签页实拍图（General / Detection / Scan & timing）
    └── home-assistant.png  # Home Assistant 接入后的仪表盘完整截图
```

---

## 🔌 `LINK.md` — USB 串口 JSON Lines 协议速查

由于探测器扫频时高频切换 Wi-Fi 射频频点，设备无法同时连接普通 Wi-Fi 网络。因此通过 ESP32-C5 内置的 **USB Serial/JTAG** 接口（与 `ESP_LOG` 共用同一个虚拟串口）提供双向 JSON Lines 通信。

### 1. 打开串口不断电重启的诀窍（DTR & RTS）
ESP32-C5 的硬件 USB Serial/JTAG 在检测到 **`DTR = 0` 且 `RTS = 1`** 时会自动触发芯片硬复位。因此上位机打开串口前必须将 `DTR` 和 `RTS` **同时置为高电平（`True`）**：
```python
import serial
s = serial.Serial()
s.port = "COM5"   # Linux 下为 /dev/ttyACM0
s.dtr = True
s.rts = True
s.open()
```

### 2. 流控与陈旧缓冲丢弃机制
* **60 秒心跳激活**：只有当上位机在最近 60 秒内发送过合法命令时，设备才会主动推送 `status`（每 10 秒及状态变化时）和 `event`（每次状态迁移时）。上位机每 15 秒发一次 `{"c":"ping","id":1}` 即可保持数据流。
* **识别协议行**：所有协议消息均以 `{"t":` 开头，同一串口上的普通 `ESP_LOG` 日志行直接忽略即可。
* **清理 USB 历史残留缓冲**：刚打开串口时，首先发送带随机 ID 的 `{"c":"ping","id":<随机整数>}`，并丢弃收到对应 `{"t":"ack","v":1,"id":<随机整数>,"ok":true}` 之前的所有旧消息。

### 3. 主机 → 设备命令一览
| 命令 JSON | 作用说明 |
| :--- | :--- |
| `{"c":"hello"}` | 请求设备返回包含唯一 MAC ID、固件版本及 12 项可调参数范围表的 `hello` 消息，随后附带一条 `status` |
| `{"c":"ping","id":1}` | 心跳保活，设备回复 `ack` |
| `{"c":"get","id":1}` | 立即请求一次完整的 `status` 状态快照 |
| `{"c":"relearn","id":2}` | 清除 2.4 GHz 和 5 GHz 基线并重新进入 60 秒学习期（等效于长按 BOOT 键） |
| `{"c":"set","k":"beep","v":true,"id":3}` | 开启/关闭报警蜂鸣器，立即生效并保存至 NVS |
| `{"c":"set","k":"scan","v":3,"id":4}` | 设置扫描频段掩码：`1` = 仅 2.4G，`2` = 仅 5G，`3` = 双频同时扫描 |
| `{"c":"set","k":"wide_db","v":8,"id":5}` | 修改 `params` 表中的任意检测阈值或屏幕亮度，立即同步到触摸屏 UI 并写入 NVS |
