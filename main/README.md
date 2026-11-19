# `main/` — 核心固件应用层源码详解

[⬅ 返回根目录文档](../README_CN.md)

本目录包含 `esp-wifi-jammer-detector` 的全部核心业务逻辑、定点信号处理（DSP）、干扰检测状态机、LVGL 触控界面、NVS 配置管理、TF 卡日志和 USB 串口通信协议。

> **架构设计亮点**：为了支持在电脑上直接跑离线单元测试（见 [`../test_host/README.md`](../test_host/README.md)），本目录中的算法核心文件（`bandplan.c`、`dsp.c`、`detect.c`、`plot.c`、`params.c`、`settings_values.c`、`link_proto.c`）**全部采用纯标准 C17 编写，不引用任何 ESP-IDF 头文件**。

---

## 📂 文件清单与模块职责划分

### 1. 系统入口与实验测量
| 文件 | 职责说明 |
| :--- | :--- |
| `app_main.c` | **程序主入口**。按顺序初始化 NVS、加载 `settings`、初始化微雪板级外设 `board_init()`、挂载 TF 卡日志 `eventlog_start()`、初始化音频 `audio_init()`、启动 SDR 接收机 `rx_init()`、启动扫频任务 `sweep_start()`、启动检测任务 `detector_start()`、启动 LVGL 与 UI、启动 USB 通信 `link_start()`，并每小时打印一次内部 SRAM 捕获保护区余量与 PSRAM 剩余内存。 |
| `characterize.c` / `.h` | **硬件射频特性标定模块**（默认关闭，通过 `CONFIG_JD_CHARACTERIZE` 开启）。用于在开发阶段实测 20/40/80 MS/s 各采样率的滤波器平坦度、换频耗时，以及验证 LCD SPI 刷屏对 2.4 GHz 底噪的自干扰（实测在 2462 MHz 抬升 ~3 dB，从而促成了射频静默窗口设计）。 |
| `Kconfig.projbuild` | 提供 `idf.py menuconfig` 中的 `Jammer Detector` 配置项（如 `CONFIG_JD_CHARACTERIZE`、`CONFIG_JD_CPU_STATS`）。 |

---

### 2. 频段规划、定点 DSP 与扫频调度管线
| 文件 | 职责说明 |
| :--- | :--- |
| `bandplan.c` / `.h` | **双频调谐规划与频谱网格映射（纯 C）**。<br>• 将 80 MS/s、1024 点 FFT 的中心 48 MHz（614 个 FFT bin，每 4 个合并为 1 个 `312.5 kHz` 网格点，即每频点产出 153~154 个有效网格点）无缝平铺到两个 Wi-Fi 频段：<br>• **2.4 GHz (`BAND_24`)**：`2400–2483.5 MHz`，共 **2 个调谐中心**（2424、2472 MHz），拼接为 **268 bins**。<br>• **5 GHz (`BAND_5`)**：`5150–5895 MHz`，共 **16 个调谐中心**（5174…5894 MHz，步进 48 MHz），拼接为 **2384 bins**，附带 U-NII-1 / 2A / 2C / 3 子频段标签与信道刻度。 |
| `dsp.c` / `.h` | **无 FPU 定点数字信号处理库（纯 C）**。<br>• ESP32-C5 没有硬件浮点单元（FPU），因此全部采用定点运算：<br>• `dsp_power_spectrum()`：Q15 Hann 窗 + 1024 点 Q15 定点基 2 FFT（复用 `esp-sdr` 的 `spectrum_fft.h` 内核），输出 DC 居中的线性功率数组。<br>• `dsp_dc_notch()`：将本振泄漏产生的中心 DC ±2 bins 替换为两侧相邻频点的均值。<br>• `dsp_cdb()`：基于查表与线性插值的高速定点对数换算，计算 $10 \log_{10}(P)$，单位为 **0.01 dB (`cdB`)**，误差 `< 0.03 dB`。<br>• `dsp_reduce()` / `dsp_reduce_min()`：将 4 个相邻 FFT bin 合并为 1 个 312.5 kHz 网格点，并维护多次快照间的 `avg`、`max` 和 **`min`（持续能量）**。 |
| `sweep.c` / `.h` | **扫频与自动增益量程任务（FreeRTOS 优先级 10，采样瞬间提权至 12）**。<br>• **唯一接触 `espsdr_rx` 驱动的任务**。<br>• **Auto-Range（自动增益校准）**：从最大增益档往下以步长 2 递减，在全频段采够至少 96 次快照，直到 ADC 10-bit 满量程削波率 ≤ `100 ppm`，再回退 2 档保存。<br>• **扫频循环**：按权重交替扫描 2.4 GHz 和 5 GHz（默认 2:1）。每次 `rx_capture()` 都包裹在 `board_rf_quiet_begin/end()` 射频静默锁内；每完成一次快照根据 `yield_pct`（默认 10%）主动短暂 `vTaskDelay` 让出 CPU 给检测任务与空闲任务。 |

---

### 3. 四类射频干扰检测引擎
| 文件 | 职责说明 |
| :--- | :--- |
| `detect.c` / `.h` | **核心干扰检测算法与状态机（纯 C，零硬件依赖）**。<br>• **内存管理**：不自行调 `malloc`，由调用方传入一块按 `detect_mem_bytes(plan)` 计算好的连续内存（2.4G 约 22 KB，5G 约 191 KB），内部通过 `layout()` 切分。<br>• **基线管理**：前 60 秒（`learn_s`）收集最多 32 帧样本求每 bin 中位数及每 10 MHz 单元（32 bins）的 20% 分位数底噪；转入 `NORMAL` 后用限幅 EMA（Q8 定点精度 `cdB * 256`）慢速跟随环境温漂。<br>• **四类判据**：每轮扫频计算 `DETECT_WIDEBAND`（`min` 谱底噪抬升）、`DETECT_NARROWBAND`（≤5 MHz 窄峰在 90% 扫频中持续存在）、`DETECT_SWEPT`（窄强峰单调移动覆盖 ≥60% 频段）、`DETECT_PULSED`（仅 `avg` 底噪抬升或快照削波，只触发 `SUSPECT` 不触发 `ALARM`）。<br>• **状态流转**：`DETECT_LEARNING` → `DETECT_NORMAL` → `DETECT_SUSPECT` → `DETECT_ALARM`。 |
| `detector.c` / `.h` | **检测任务封装层（FreeRTOS 优先级 9）**。<br>• 注册 `sweep` 的切片回调 `on_segment()`，使用**双缓冲（Double-Buffering）**接收拼接完成的整段频谱（若检测任务忙则丢弃并计数，绝不阻塞扫频任务）。<br>• 启动时从 `jd_nvs` 分区恢复上次保存的基线（匹配频段几何参数与增益档位），运行中每 1 小时自动把 EMA 更新后的基线回写 Flash。<br>• 当发生状态迁移时，触发 `audio_alarm_set()`、`eventlog_append()`、`ui_notify_alarm_ sweep()` 和 `link_notify_event()`。 |

---

### 4. 屏幕渲染与 LVGL 触控 UI（480×320 横屏）
| 文件 | 职责说明 |
| :--- | :--- |
| `lvgl_port.c` / `.h` | **LVGL v9 移植层**。接管 LVGL 内存分配器使其全部使用 PSRAM；分配双 40 行 PSRAM DMA 绘图缓冲区，通过 `board_lcd_draw_async()` 异步刷屏；绑定 FT6336 触摸输入。 |
| `plot.c` / `.h` | **高性能频谱与瀑布图光栅化渲染器（纯 C）**。<br>• 为什么不用 LVGL 的 Chart 控件？因为全屏 LVGL 重绘在无 FPU 的单核 C5 上耗时 ~80 ms，会严重拖慢帧率和扫频。<br>• `plot.c` 直接在 RGB565 像素条带（Strip）缓冲区中绘制网格、U-NII 标签、基线（灰）、均值填充曲线（青）、Max-Hold 峰值线（橙）以及瀑布图伪彩色查表（蓝 `-5 dB` → 青 → 黄 → 红 `+30 dB`），直接经 DMA 送入 ST7796。 |
| `ui.c` / `.h` / `ui_priv.h` | **UI 框架、BOOT 按键轮询与全局报警闪框**。<br>• 短按 `BOOT` 键循环切换：`Status` → `Spectrum/Waterfall` → `Settings`。<br>• 长按 `BOOT` 键（≥ 1.5 秒）：全频段重新学习基线。<br>• 任一频段处于 `ALARM` 时，在屏幕四边闪烁红色警示边框并驱动蜂鸣器节奏。 |
| `ui_status.c` | **状态主屏（默认页）**。左右两个大磁贴分别展示 2.4 GHz 和 5 GHz 状态（灰/绿/黄/红）、底噪相对基线变化（dB）、证据占空比进度条、上次报警时间及每秒扫频速率。点击磁贴直接跳转到对应频段的频谱页。 |
| `ui_spectrum.c` | **实时频谱与瀑布图页**。支持 `2.4 / 5` 频段切换、`Reset max` 清除峰值保持、`Waterfall` 切换滚动瀑布图（后台始终为两个频段各维护 236 行历史瀑布图数据）。同时负责在报警后约 2 分钟将瀑布图渲染为带刻度和红标的 BMP 图像交由 `eventlog` 存卡。 |
| `ui_settings.c` | **三标签设置页（General / Detection / Scan & timing）**。采用分页标签而非长滚动列表以避免全屏重绘卡顿。滑块松手即生效，延迟 2 秒或离开页面时写入 Flash。 |

---

### 5. 配置持久化、音频、TF 卡日志与 USB 通信
| 文件 | 职责说明 |
| :--- | :--- |
| `params.c` / `.h` | **可调参数元数据表（纯 C）**。统一定义 12 个可调参数（学习时间、宽带阈值/比例、窄带阈值/比例、扫频阈值/比例、报警占空比、持续时间、恢复时间、频段权重、屏幕亮度）的键名、显示名、单位、最小/最大值、步长和读写指针，供触摸屏 UI 和 USB 协议共用。 |
| `settings_values.c` | **配置默认值与合法性校验（纯 C）**。实现 `settings_default()` 和 `settings_check()`。 |
| `settings.c` / `.h` | **NVS Flash 持久化存储**。操作独立分区 `jd_nvs`，读写 `settings_t` 结构体、开机计数器 `boot` 以及双频段的自描述基线二进制块（`b0`、`b1`）。 |
| `audio.c` / `.h` | **报警蜂鸣器驱动**。通过 `esp_codec_dev` 驱动 I2C 上的 ES8311 DAC 与 CH32V006 控制的 NS4150B 功放。**仅在发声期间开启 I2S 时钟与功放电源**，平时完全断电静默，实测发声时对底噪影响 `< 0.1 dB`。 |
| `eventlog.c` / `.h` | **TF 卡 CSV 日志与报警 BMP 瀑布图分块写入**。<br>• 状态变化时追加一行到 `/jd_events.csv`。<br>• 保存约 300 KB 的报警瀑布图 `.bmp` 时，**每次只写 32 行像素（在射频静默窗口内），每块之间间隔 300 ms**，保证写卡期间扫频不会长时间中断。卡剩余空间 `< 2 MB` 时自动停止写图片。 |
| `link_proto.c` / `.h` | **USB 串口 JSON Lines 协议编解码器（纯 C）**。格式化 `hello`、`status`、`event`、`ack` 消息并解析主机下发的 `ping`、`hello`、`get`、`relearn`、`set` 命令。 |
| `link.c` / `.h` | **USB Serial/JTAG 收发任务**。仅在主机最近 60 秒内发过命令时才流式推送状态与事件；发送数据同样包裹在射频静默窗口内，且采用非阻塞写入（主机不读即丢弃，绝不拖慢扫频）。 |
