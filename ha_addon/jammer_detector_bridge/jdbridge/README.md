# `ha_addon/jammer_detector_bridge/jdbridge/` — Python 桥接核心包

[⬅ 返回 jammer_detector_bridge 目录](../README_CN.md) | [🏠 返回根目录文档](../../../README_CN.md)

本目录是 Home Assistant 桥接插件的 Python 核心源码包（可通过 `python3 -m jdbridge` 直接启动，无需 Home Assistant 容器也可在任意 Linux/Windows/macOS 机器上把探测器桥接到 MQTT Broker）。

---

## 📂 模块职责说明

| 文件 | 职责说明 |
| :--- | :--- |
| `__init__.py` | 包初始化文件，定义版本号。 |
| `__main__.py` | **命令行入口与 MQTT 主循环**。<br>解析 `--serial`、`--mqtt-host`、`--mqtt-port`、`--mqtt-user`、`--mqtt-password`、`--topic-prefix` 等参数，建立 `paho-mqtt` 连接并启动 `Bridge.run_forever()`。 |
| `protocol.py` | **设备端 JSON Lines 协议解析与命令构造**。<br>• `parse_line()`：从可能混有 `ESP_LOG` 日志的串口行中提取 `{"t":...}` JSON 对象并校验版本 `v == 1`。<br>• `command()`：构造下发给设备的紧凑单行 JSON 命令。<br>• `set_value_from_mqtt()`：将 MQTT 收到的字符串/数值转换为设备 `set` 命令所需的合法参数值。 |
| `discovery.py` | **Home Assistant MQTT Discovery 实体生成器**。<br>根据设备 `hello` 消息上报的唯一 ID、固件版本及 `params` 参数元数据表，动态生成双频报警、状态、干扰类型、底噪、占用率、事件、电池以及 15 个可调控制实体的 Discovery JSON 配置。 |
| `bridge.py` | **串口与 MQTT 双向状态机**。<br>负责 `DTR=True, RTS=True` 无复位打开串口、随机 ID `ping` 握手去陈旧缓冲、15 秒定时心跳、30 秒超时重连、以及将 MQTT 控制指令转发至串口。 |

---

## ▶️ 脱离 Home Assistant 独立运行示例

如果你在普通 Linux / Windows 电脑上跑了一个 MQTT Broker（如 Mosquitto），可以直接在命令行运行此包：

```bash
pip install pyserial paho-mqtt
python3 -m jdbridge --serial COM5 --mqtt-host 127.0.0.1 --mqtt-port 1883
```
