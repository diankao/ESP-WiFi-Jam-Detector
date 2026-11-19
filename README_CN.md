# ESP-WiFi-Jam-Detector 中文总览与目录树导航

本项目是面向微雪 **Waveshare ESP32-C5-Touch-LCD-3.5** 的**纯接收（Receive-Only）2.4 GHz + 5 GHz 双频物理层射频干扰探测器**。它基于 [ESPARGOS/esp-sdr](https://github.com/ESPARGOS/esp-sdr) 将 ESP32-C5 的 Wi-Fi 接收机作为 80 MS/s 快照式 SDR 使用，通过实时频谱拼接、自学习基线与 `min/avg/max` 统计特征检测宽带压制、窄带连续波、扫频干扰与脉冲宽带能量（如微波炉）。

---

## 🌳 仓库目录树与各目录文档导航

我们在每个子目录下均放置了对应的中文详解文档，你可以按目录逐层查阅：

```text
ESP-WiFi-Jam-Detector/
├── README.md                                  # 原版英文说明文档
├── README_CN.md                               # [本文档] 中文全局架构总览、目录树导航与编译刷机指南
├── 项目讲解与魔改移植指南.md                  # 深度原理剖析、微雪开发板选购避坑与跨芯片魔改移植手册
├── CMakeLists.txt                             # ESP-IDF 顶层构建脚本
├── sdkconfig.defaults                         # ESP32-C5 默认 SDK 配置（PSRAM、CPU 240MHz、PHY 校准等）
├── partitions.csv                             # Flash 分区表（含独立 256 KB 的 jd_nvs 分区存放设置与基线）
├── repository.yaml                            # Home Assistant Add-on 仓库声明文件
│
├── main/                                      # 【核心固件应用层】扫频调度、定点 DSP、检测引擎、LVGL UI、日志与 USB 协议
│   └── README.md                              # -> 点击查看 main/ 下 20+ 个 C/H 源文件的模块拆解与调用链
│
├── components/                                # 【ESP-IDF 硬件与驱动组件层】
│   ├── README.md                              # -> 点击查看组件层职责划分与魔改移植替换指南
│   ├── board/                                 # 微雪 ESP32-C5-Touch-LCD-3.5 板级驱动（ST7796/FT6336/AXP2101/射频静默锁）
│   │   └── README.md                          # -> 点击查看引脚定义、外设初始化与防 LCD 自干扰机制
│   └── espsdr_rx/                             # 移植自 esp-sdr 的 ESP32-C5 原始 I/Q 快照接收驱动与 SRAM 保护区
│       └── README.md                          # -> 点击查看 80 MS/s 快照采样原理、sram_guard.ld 与跨芯片移植指南
│
├── docs/                                      # 【设计规范、USB 协议说明与演示媒体】
│   └── README.md                              # -> 点击查看 BRIEF.md 设计里程碑与 LINK.md 串口 JSON 协议中文详解
│
├── test_host/                                 # 【宿主机离线单元测试】无需开发板即可在电脑上测试 DSP 与检测算法
│   └── README.md                              # -> 点击查看 5 组宿主单元测试的编译运行方法与覆盖场景
│
├── tools/                                     # 【合成射频频谱仿真器】
│   └── README.md                              # -> 点击查看 sim_spectra.py 7 种仿真信号（含微波炉/扫频/单音）生成原理
│
└── ha_addon/                                  # 【Home Assistant 集成插件】
    ├── README.md                              # -> 点击查看 Home Assistant 插件目录说明
    └── jammer_detector_bridge/                # USB-to-MQTT 自动发现桥接服务（Python）
        └── README_CN.md                       # -> 点击查看 jdbridge 源码结构、DTR/RTS 防复位机制与 MQTT 实体映射
```

---

## 🔄 系统数据流与任务架构图

```text
[ESP32-C5 Wi-Fi 射频前端 (80 MS/s 10-bit I/Q)]
       │
       ▼  (包裹在 board_rf_quiet_begin/end 射频静默窗口内，暂停 LCD/SD SPI DMA)
[components/espsdr_rx]  rx_tune() + rx_capture(1024 samples)
       │
       ▼
[main/sweep.c]  (FreeRTOS Prio 10, 采样期间临时提权至 Prio 12)
  ├─ 每个频点连续抓拍 K=4 次快照
  ├─ 调用 [main/dsp.c]：Hann 窗 -> 1024点 Q15 定点 FFT -> 挖除 DC ±2 bins -> 4合1 降采样为 312.5 kHz 网格
  ├─ 统计 K 次快照的 avg_cdb (均值)、max_cdb (最大)、min_cdb (最小/持续能量)
  └─ 按 [main/bandplan.c] 拼接整段频谱：
       • 2.4 GHz：2 个中心频点 (2424, 2472 MHz) -> 268 bins
       • 5 GHz  ：16 个中心频点 (5174..5894 MHz) -> 2384 bins
       │
       ├─────────────────────────────┐
       ▼ (双缓冲无锁队列移交)         ▼ (sweep_get_view 供 UI 读取)
[main/detector.c] (Prio 9)      [main/ui*.c & plot.c] (Prio 11)
  └─ 调用 [main/detect.c]：       ├─ Status 页：双频状态卡片、底噪抬升、占空比条
       ├─ 60s 基线学习与 EMA 跟踪  ├─ Spectrum 页：实时均值、Max-Hold、基线对比
       ├─ 评估 4 类干扰证据        ├─ Waterfall 页：236 行滚动彩色瀑布图
       └─ 状态机：                 └─ Settings 页：三标签页参数调节（延迟 2s 存 NVS）
          LEARNING -> NORMAL -> SUSPECT -> ALARM
       │
       ├──► [main/audio.c]    ：ALARM 时通过 ES8311 + NS4150B 播放 2 kHz 蜂鸣
       ├──► [main/eventlog.c] ：写入 TF 卡 /jd_events.csv，并在报警约 2 分钟后保存前后对称的瀑布图 .bmp
       └──► [main/link.c]     ：通过 USB-C 串口输出 JSON Lines 遥测 -> [ha_addon] 桥接至 Home Assistant
```

---

## ⚡ 快速刷机与编译备忘

### 1. 免编译直接刷入 Release 固件
```powershell
pip install esptool
esptool --chip esp32c5 -p COMx -b 460800 write-flash 0x0 esp-wifi-jammer-detector-v1.1.0-merged.bin
```

### 2. 源码编译（必须使用锁定的 ESP-IDF Commit）
`espsdr_rx` 依赖 ESP32-C5 闭源 PHY 库的内部符号偏移，必须使用 commit `25fe69f946311abdaf9ad56591f25fedbc20ac98`：
```bash
mkdir -p ~/esp/esp-idf-espsdr && cd ~/esp/esp-idf-espsdr
git init && git remote add origin https://github.com/espressif/esp-idf.git
git fetch --depth 1 origin 25fe69f946311abdaf9ad56591f25fedbc20ac98 && git checkout FETCH_HEAD
git submodule update --init --recursive --depth 1
./install.sh esp32c5

. ~/esp/esp-idf-espsdr/export.sh
idf.py set-target esp32c5
idf.py build
idf.py -p COMx flash monitor
```

### 3. Flash 分区表说明（`partitions.csv`）
| 分区名 | 类型 | 偏移地址 | 大小 | 用途 |
| :--- | :--- | :--- | :--- | :--- |
| `nvs` | data/nvs | `0x9000` | 24 KB | ESP-IDF Wi-Fi/PHY 默认校准数据 |
| `phy_init` | data/phy | `0xf000` | 4 KB | PHY 初始化参数 |
| `factory` | app/factory | `0x10000` | 4 MB | 主固件程序 |
| `jd_nvs` | data/nvs | `0x410000` | 256 KB | **本项目专用持久化分区**：存放用户阈值、自动量程增益及双频学习基线（重刷固件不会丢失） |
