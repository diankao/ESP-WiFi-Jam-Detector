# `components/espsdr_rx/` — ESP32-C5 快照式 SDR 接收机驱动组件

[⬅ 返回 components 目录](../README.md) | [🏠 返回根目录文档](../../README_CN.md)

本组件移植并精简自开源项目 **[ESPARGOS/esp-sdr](https://github.com/ESPARGOS/esp-sdr)**（Commit `f86da818`），利用 ESP32-C5 内部未公开的 Wi-Fi PHY/ADC 硬件快照转储引擎，将芯片变为一台支持 **2.4 GHz (2400–2483.5 MHz)** 与 **5 GHz (5150–5895 MHz)** 的宽带软件定义无线电接收机。

---

## 📂 目录与文件结构

```text
components/espsdr_rx/
├── CMakeLists.txt                              # 挂载 sram_guard.ld 链接脚本与 --wrap 符号拦截选项
├── VENDORED.md                                 # 上游 esp-sdr 文件来源与修改对照说明
├── rx_core.c                                   # 精简后的 ESP32-C5 专用接收核心（移除了串口协议与片上 FFT）
├── include/
│   └── espsdr_rx.h                             # 对外暴露的 5 个极简纯接收 API
└── upstream/                                   # 原封不动引自上游 esp-sdr 的底层文件
    ├── common/
    │   ├── burst_gain.h / burst_gain_min.h     # 手动增益档位控制与硬件 AGC 切换
    │   ├── rx_bandwidth.h                      # 模拟基带滤波器带宽配置（11..48 MHz）
    │   ├── rx_recalibration.c / .h             # 换频后的纯接收直流（DC）重校准（无 TX 回环发射）
    │   ├── rx_calibration_state.c              # 拦截并保存 PHY 初始校准状态（配合链接器 --wrap）
    │   ├── rx_tuning.h                         # 频点合法性校验与调谐辅助函数
    │   └── spectrum_fft.h                      # Q15 定点基 2 FFT 核心算法（被 main/dsp.c 调用）
    ├── families/c5_c6_c61/
    │   └── receiver.c.orig                     # 上游原始的 C5/C6/C61 三合一接收源码备份（未参与编译，供对比/移植参考）
    └── targets/esp32c5/
        ├── chip.h                              # ESP32-C5 专属：IQ_BUFFER 地址、SRAM 保留区声明、adctrig 调用封装
        ├── sram_guard.ld                       # 链接器脚本断言：严禁 .data/.bss 侵入 0x40820000 之后的捕获区
        └── tuning.h                            # ESP32-C5 PLL 与通道调谐封装
```

---

## 🔬 底层工作原理详解

### 1. 原始 I/Q 快照捕获是怎么实现的？（`chip.h` & `rx_core.c`）
* ESP32-C5 的 Wi-Fi PHY 库中隐藏了一个内部调试函数 `adctrig(...)`。
* 当调用 `adctrig(n - 1, 0, 0, divider * 2, ...)` 时，硬件 ADC 转储引擎会暂时接管内部 SRAM 的一个 **128 KB 存储体（`0x40820000 ~ 0x40840000`）**，并将最高 **80 MS/s** 采样率的原始 I/Q 基带采样直接写入物理地址 `0x40830000`（`IQ_BUFFER`）。
* 每个 32-bit 字（Word）中包含一对 **10-bit 有符号复数样本（I 和 Q，范围 `-512..511`）**，首个字的高位还附带了当前硬件增益编码（`gain_code`）。
* `rx_capture()` 在捕获前会先向 `IQ_BUFFER` 写入哨兵魔数（`0xa5a0055a` 和尾部金丝雀 `0x5a5aa5a5`），捕获完成后校验魔数是否被完整覆盖且尾部未越界，随后解包为 `int16_t` 交错的 I/Q 数组并统计满量程削波（`|I| >= 511` 或 `|Q| >= 511`）样本数。

### 2. 为什么必须保护 `0x40820000`（`sram_guard.ld`）？
* 因为硬件捕获引擎通过设置 `SRAM_OWNER_REG (0x60095004)` 接管 `0x40820000` 起的 128 KB SRAM 时，如果 CPU 的普通变量（`.data`/`.bss`）或任务栈落在这个区域，采样瞬间就会发生内存损坏或崩溃。
* 因此：
  1. `chip.h` 使用 `SOC_RESERVE_MEMORY_REGION(0x40820000, 0x40840000, c5_rf_dump)` 阻止 Heap 分配器使用该区域。
  2. `sram_guard.ld` 在编译链接阶段检查 `ASSERT(_bss_end <= 0x40820000)`，一旦静态内存超标直接报错拒绝生成固件。

### 3. 为什么能做到“纯接收零发射”换频校准？（`rx_recalibration.c`）
* 普通 ESP32 Wi-Fi 切换信道时，官方 PHY 库默认可能会开启内部 TX 音调回环（TX tone loopback）做 I/Q 与 DC 校准。
* 本组件通过链接器选项 `--wrap=phy_chip_set_chan_ana` 和 `--wrap=phy_set_rx_gain_cal_dc` 拦截了底层校准流程，在每次 `rx_tune()` 换频时显式调用 `phy_pbus_xpd_tx_off()` 关闭发射通路，仅执行**纯接收直流偏移校准（Receive-Only DC Recalibration）**。

---

## 🛠️ 对外接口（`include/espsdr_rx.h`）

| 函数 | 说明 |
| :--- | :--- |
| `rx_init()` | 初始化 Wi-Fi/PHY 为 NULL 模式并接管接收通路（仅在开机时调一次） |
| `rx_tune(unsigned mhz)` | 切换本振中心频率（`> 3000 MHz` 自动切换至 ESP32-C5 的 5 GHz 射频前端并做 DC 重校准） |
| `rx_set_gain_manual(unsigned index)` | 锁定手动接收增益档位（0..`rx_gain_max()`），防止硬件 AGC 掩盖底噪抬升 |
| `rx_set_gain_auto()` | 恢复硬件 AGC（本项目仅预留接口，扫频时全程使用手动增益） |
| `rx_set_bw(unsigned mhz)` | 设置模拟滤波器带宽（`0` 代表最宽模拟带宽，配合 80 MS/s 采样） |
| `rx_capture(n, rate, iq_out, info)` | 触发一次 `n` 点（默认 1024 点）I/Q 快照捕获，解包输出到 `iq_out` 并返回耗时与削波统计 |

---

## 🔄 移植到其它 ESP 芯片时的修改点

* **移植到 ESP32-C6 / ESP32-C61**：
  直接对照 `upstream/families/c5_c6_c61/receiver.c.orig`，把上游 `esp-sdr` 中 `targets/esp32c6` 的 `chip.h`、`tuning.h`、`sram_guard.ld` 放进 `upstream/targets/`，并在 `rx_core.c` 中恢复 `CONFIG_IDF_TARGET_ESP32C6` 条件编译块即可。
* **移植到 ESP32-S3 / ESP32 / ESP32-C3**：
  从上游 `ESPARGOS/esp-sdr` 仓库的 `main/targets/esp32s3/`（或对应型号目录）提取其 `receiver.c` 与 `sram_guard.ld`，替换 `rx_core.c` 内部实现，保持 `espsdr_rx.h` 的上述 5 个函数原型不变即可。
