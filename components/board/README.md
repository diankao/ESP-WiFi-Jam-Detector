# `components/board/` — 微雪 ESP32-C5-Touch-LCD-3.5 板级支持包 (BSP)

[⬅ 返回 components 目录](../README.md) | [🏠 返回根目录文档](../../README_CN.md)

本组件专门负责微雪 **Waveshare ESP32-C5-Touch-LCD-3.5** 开发板上除 Wi-Fi 射频以外的所有板载硬件外设初始化与访问，并实现了极其关键的 **射频静默窗口（Radio-Quiet Window）** 同步机制。

---

## 📂 文件结构

```text
components/board/
├── CMakeLists.txt
├── board.c                 # I2C 总线、CH32V006 扩展 IO、AXP2101 电源读取、ST7796 SPI DMA 刷屏、射频静默锁、FT6336 触摸、BOOT 键
├── st7796.c                # ST7796 LCD 控制器初始化命令序列与横屏方向配置 (MADCTL)
└── include/
    ├── board_pins.h        # 严格引自微雪官方仓库的 GPIO 引脚与 I2C 寄存器映射表（附官方源码出处行号）
    ├── board.h             # 对外暴露的板级 API 头文件
    └── st7796.h            # ST7796 内部初始化接口
```

---

## 📌 硬件总线与引脚映射表（`include/board_pins.h`）

### 1. 共享 I2C 总线（`I2C_NUM_0`，400 kHz）
* **引脚**：`SCL = GPIO 26`，`SDA = GPIO 27`
* **挂载在 I2C 总线上的设备**：
  * **`0x24` — CH32V006 IO 扩展芯片**：
    * `P0`：触摸屏复位 (`TP_RST`)
    * `P1`：LCD 屏幕复位 (`LCD_RST`)
    * `P5`：NS4150B 音频功放使能 (`PA_EN`，仅在播放报警音时拉高，平时拉低省电防干扰)
    * `Reg 0x05 (PWM)`：屏幕背光亮度无级调节（0..255）
  * **`0x34` — AXP2101 电源管理芯片 (PMIC)**：
    * 只读访问：读取 USB 供电状态 (`VBUS`)、锂电池连接状态、充电状态、电池电压 (`mV`) 与电量百分比（`0xA4` 寄存器）。
  * **`0x38` — FT6336 电容触摸控制器**（中断脚 `TOUCH_INT = GPIO 3`）：
    * 硬件原生坐标为竖屏 `320×480`，`board_touch_read()` 会自动将其换算为横屏 `480×320` 坐标。
  * **`0x18` — ES8311 音频编解码器**：通过 `board_i2c_bus()` 暴露句柄给 `main/audio.c` 使用。

### 2. SPI2 总线（ST7796 LCD 与 TF 卡共用，60 MHz）
* **公共引脚**：`SCLK = GPIO 6`，`MOSI = GPIO 7`
* **ST7796 LCD 专用**：`DC = GPIO 5`，`CS = GPIO 8`（分辨率 480×320 横屏，大端序 RGB565）
* **TF 卡槽专用**：`SD_CS = GPIO 9`，`SD_MISO = GPIO 2`（注意：需将板载拨码开关 **JP8 第 2 位拨到 ON**）

### 3. 按键与 I2S 音频
* **BOOT 按键**：`GPIO 28`（内部上拉，按下为低电平）
* **I2S 音频总线**：`BCLK = GPIO 23`，`WS = GPIO 10`，`DOUT = GPIO 25`，`DIN = GPIO 24`（无 MCLK）

---

## 🛡️ 核心设计：射频静默窗口（Radio-Quiet Window）

### 为什么要设计射频静默窗口？
在里程碑 M3 实测中，作者通过 `main/characterize.c` 对比测试发现：
* 当 ST7796 屏幕通过 60 MHz 的 SPI2 DMA 刷屏时，**高频数字谐波会耦合进板载 2.4 GHz 接收电路，导致 2462 MHz（Wi-Fi 11 信道）附近的底噪被人为抬高约 `3 dB`**（对 5 GHz 无影响）。

### 实现机制（`board_rf_quiet_begin` / `board_rf_quiet_end`）
在 `board.c` 中维护了一个互斥令牌：
1. **每次射频快照采样前（约耗时 2 ms）**，`sweep.c` 调用 `board_rf_quiet_begin()`：
   * 如果当前正好有一条 LCD 像素条带（Strip，耗时约 5 ms）正在 SPI DMA 传输中，采样任务最多等待这一个条带传完，然后立即锁住令牌。
   * 令牌锁住期间，任何新的 `board_lcd_draw_async()`、TF 卡写入（`eventlog.c`）或 USB 串口发送（`link.c`）都会自动挂起等待。
2. **采样结束瞬间**，调用 `board_rf_quiet_end()` 释放令牌，恢复屏幕 DMA 和外设 I/O。
3. 这样既彻底消除了屏幕 SPI 对 2.4 GHz 接收机的自干扰，又保证了约 10 fps 的流畅频谱/瀑布图刷新。
