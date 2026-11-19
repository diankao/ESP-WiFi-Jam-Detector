# `ha_addon/jammer_detector_bridge/tests/` — Python 桥接器单元测试目录

[⬅ 返回 jammer_detector_bridge 目录](../README_CN.md) | [🏠 返回根目录文档](../../../README_CN.md)

本目录包含针对 `jdbridge` Python 包的 `pytest` 自动化测试套件。

---

## 📂 文件结构

```text
tests/
├── conftest.py                 # pytest 公共夹具（FakeSerial 虚拟串口、FakeMqtt 虚拟客户端、示例 hello 消息）
├── test_protocol.py            # 测试 JSON Lines 解析（含日志截断行提取）、命令生成与参数边界校验
├── test_discovery.py           # 测试 MQTT Discovery Topic 与实体配置生成完整性
├── test_bridge.py              # 测试握手同步、旧缓冲丢弃、状态/事件转发、MQTT 命令下发、超时重连与离线状态发布
└── fixtures/
    ├── link_samples.jsonl      # 由固件 test_host/test_link.c 导出的真实设备协议消息样本
    └── serial_capture.txt      # 真机串口抓包样本（包含启动日志、ESP_LOG 与协议 JSON 混排）
```

---

## ▶️ 运行测试

```bash
cd ha_addon/jammer_detector_bridge
pytest -v
```
