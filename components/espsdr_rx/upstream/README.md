# `components/espsdr_rx/upstream/` — 上游 ESPARGOS/esp-sdr 移植源码目录

[⬅ 返回 espsdr_rx 目录](../README.md) | [🏠 返回根目录文档](../../../README_CN.md)

本目录存放直接从上游 **[ESPARGOS/esp-sdr](https://github.com/ESPARGOS/esp-sdr)**（Commit `f86da818`）同步过来的底层射频与定点 FFT 实现文件。除 `families/c5_c6_c61/receiver.c.orig` 作为未修改参考备份外，其余文件均原样（verbatim）参与编译或被 `#include`。

---

## 📂 子目录与文件说明

```text
upstream/
├── common/
│   ├── burst_gain.h            # 手动接收增益写入与硬件 AGC 切换逻辑（被 ../rx_core.c 包含）
│   ├── burst_gain_min.h        # 最小增益边界定义
│   ├── rx_bandwidth.h          # 模拟基带滤波器电容档位映射表（11..48 MHz）
│   ├── rx_calibration_state.c  # 通过链接器 --wrap 拦截并保存 PHY 启动时的初始校准状态
│   ├── rx_recalibration.c/.h   # 换频后的纯接收直流（DC）偏移重校准（确保零 TX 发射）
│   ├── rx_tuning.h             # 2.4 GHz / 5 GHz 频率范围合法性检查
│   └── spectrum_fft.h          # Q15 定点基 2 FFT 与蝶形运算内核（被 main/dsp.c 包含）
│
├── families/c5_c6_c61/
│   └── receiver.c.orig         # 上游原始的 ESP32-C5 / C6 / C61 共用接收驱动源码（仅供对比和移植 C6/C61 时参考，不参与编译）
│
└── targets/esp32c5/
    ├── chip.h                  # 定义 SRAM_OWNER_REG (0x60095004)、IQ_BUFFER (0x40830000)、128 KB 内存保留区与 adctrig() 触发封装
    ├── sram_guard.ld           # 链接脚本断言：防止 .data / .bss 越过 0x40820000 侵入硬件采样捕获区
    └── tuning.h                # ESP32-C5 频点切换辅助头文件
```
