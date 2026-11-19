# `ha_addon/jammer_detector_bridge/` — Home Assistant USB-to-MQTT 桥接插件详解

[⬅ 返回 ha_addon 目录](../README.md) | [🏠 返回根目录文档](../../README_CN.md)

本插件运行在 Home Assistant 主机上，通过 USB 串口读取 `ESP-WiFi-Jam-Detector` 的实时状态、报警事件与配置参数，并通过 **MQTT Discovery（自动发现）** 将探测器注册为一个完整的 Home Assistant 设备，无需编写任何 YAML 配置，也无需开启 ESP32-C5 的 Wi-Fi。

---

## 📂 目录与文件结构

```text
ha_addon/jammer_detector_bridge/
├── README.md                   # 原版英文简述
├── README_CN.md                # [本文档] 中文架构与源码说明
├── DOCS.md                     # Home Assistant 插件商店内显示的详细英文使用文档与自动化示例
├── CHANGELOG.md                # 插件版本更新记录
├── config.yaml                 # Home Assistant Add-on 配置文件（声明串口权限、MQTT 服务依赖与配置项）
├── Dockerfile                  # 本地构建容器镜像脚本（基于 Alpine Python 3）
├── requirements.txt            # Python 依赖：pyserial 与 paho-mqtt
├── run.sh                      # 容器启动入口：通过 bashio 读取 HA 配置与 MQTT 凭据并启动 python3 -m jdbridge
├── translations/
│   └── en.yaml                 # HA 配置页面字段翻译
├── jdbridge/                   # 【核心 Python 桥接包】
│   ├── __init__.py
│   ├── __main__.py             # CLI 参数解析、MQTT 自动重连循环与主线程入口
│   ├── protocol.py             # 设备 JSON Lines 协议解析、命令构造与参数范围校验
│   ├── discovery.py            # 生成全套 Home Assistant MQTT Discovery 配置 Payload 与 Topic 规划
│   └── bridge.py               # 串口 <-> MQTT 状态机：DTR/RTS 无复位打开串口、随机 ID 握手同步、15s 心跳、掉线自动重连
└── tests/                      # 【Python 单元测试套件 (pytest)】
    ├── conftest.py
    ├── test_protocol.py        # 协议解析与命令生成测试
    ├── test_discovery.py       # MQTT Discovery 实体定义生成测试
    ├── test_bridge.py          # 端到端桥接状态机与断线重连模拟测试
    └── fixtures/
        ├── link_samples.jsonl  # 由固件 C 代码生成的真实 JSON Lines 样本
        └── serial_capture.txt  # 混杂了 ESP_LOG 控制台日志的真实串口抓包数据
```

---

## 🔄 `jdbridge` 核心工作机制（`jdbridge/bridge.py`）

1. **无复位打开串口（`open_serial`）**：
   显式设置 `s.dtr = True` 和 `s.rts = True` 后再调用 `s.open()`，避免触发 ESP32-C5 USB Serial/JTAG 的硬件复位电路。
2. **随机 ID 握手去陈旧缓冲（Sync Handshake）**：
   串口刚打开时，内核缓冲区可能残留着数分钟前未读的旧数据。`Bridge` 会先生成一个随机整数 `sync_id`，发送 `{"c":"ping","id":sync_id}`，并在收到匹配的 `ack` 之前**忽略串口读到的所有旧行**。
3. **自动发现实体构建（`hello` → MQTT Discovery）**：
   同步完成后发送 `{"c":"hello"}`，根据设备返回的唯一 MAC 地址和 `params` 参数表，通过 `discovery.py` 自动向 `homeassistant/<component>/jammer_<id>/...` 发布全套实体定义：
   * **每个频段（2.4 GHz & 5 GHz）**：
     * `binary_sensor`：干扰报警（`problem` 类别，可直接触发 HA 自动化通知）
     * `sensor`：当前状态（`learning/normal/suspect/alarm/off`）、干扰类型（`wideband/swept/narrowband/pulsed`）、底噪抬升（`dB`）、频谱占用率（`%`）、累计报警次数
     * `event`：每次状态跳变触发一次事件，附带全部检测指标属性
   * **设备级实体**：
     * `sensor` / `binary_sensor`：电池电量百分比、充电状态
     * `button`：一键重新学习基线（`Re-learn baseline`）
     * `switch`：报警蜂鸣器开关（`Alarm beep`）
     * `select`：扫描频段选择（`2.4 GHz only` / `5 GHz only` / `2.4 GHz + 5 GHz`）
     * `number`：12 个检测阈值、时间窗口与屏幕亮度滑块（在 HA 中拖动滑块会立即通过 `set` 命令下发给设备并写入 Flash）
4. **心跳与掉线可用性管理**：
   每 15 秒发送一次 `ping` 维持设备 60 秒流式推送窗口；一旦串口断开或超过 30 秒无响应，自动将设备可用性 Topic（`jammer/<id>/availability`）置为 `offline`，并以 `(2, 5, 10, 30)` 秒退避策略自动重连。

---

## 🧪 本地运行 Python 测试

```bash
cd ha_addon/jammer_detector_bridge
pytest
```
