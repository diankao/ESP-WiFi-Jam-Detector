# `components/` — 硬件抽象与底层驱动组件目录

[⬅ 返回根目录文档](../README_CN.md)

本目录存放 ESP-IDF 自定义组件，将**微雪开发板外设**与**ESP32-C5 底层 SDR 射频捕获硬件**同 `main/` 中的业务逻辑彻底隔离。

---

## 📂 组件列表

```text
components/
├── board/          # 微雪 Waveshare ESP32-C5-Touch-LCD-3.5 板级支持包 (BSP)
│   └── README.md   # -> 查看屏幕、触摸、电源芯片、IO 扩展芯片与“射频静默窗口”详解
└── espsdr_rx/      # ESP32-C5 原始 I/Q 快照 SDR 接收驱动（移植自 ESPARGOS/esp-sdr）
    └── README.md   # -> 查看 80 MS/s ADC 快照捕获原理、SRAM 保护区与跨芯片移植说明
```

---

## 🔧 魔改与移植时的组件替换原则

得益于清晰的组件边界，当你把本项目移植到其它 ESP 开发板时：

1. **换板不换芯片（比如换成无屏的 ESP32-C5 核心板，或换其它屏幕）**：
   * `components/espsdr_rx/` **完全不用动**。
   * 只需修改或裁剪 [`components/board/`](board/README.md)（例如把 `board_rf_quiet_begin/end()` 改为空函数，或替换为你自己的屏幕驱动）。
2. **换芯片（比如换成 ESP32-C6 / ESP32-S3 单 2.4 GHz 方案）**：
   * 只需修改 [`components/espsdr_rx/`](espsdr_rx/README.md)，保持 `include/espsdr_rx.h` 中的 5 个标准 C 接口签名不变，上层 `main/sweep.c` 即可无缝调用。
