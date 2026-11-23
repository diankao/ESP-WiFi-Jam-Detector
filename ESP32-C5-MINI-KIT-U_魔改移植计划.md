# 微雪 ESP32-C5-MINI-KIT-U (无屏幕版) 魔改移植计划与架构选项

> **目标硬件**：Waveshare ESP32-C5-MINI-KIT-U（外置天线、无屏幕、无 PSRAM、4MB Flash、板载 WS2812B RGB 灯）  
> **基准项目**：ESP-WiFi-Jam-Detector v1.1.0（原版针对 Waveshare ESP32-C5-Touch-LCD-3.5）

---

## 一、 新旧硬件关键差异与硬核约束分析

在开始写代码前，必须正视 **ESP32-C5-MINI-KIT-U** 与原版 **ESP32-C5-Touch-LCD-3.5** 的底层硬件差异——**最大的挑战不是“去掉屏幕”，而是“没有 PSRAM 且只有 4MB Flash”**：

| 硬件维度 | 原版 (ESP32-C5-Touch-LCD-3.5) | 目标板 (ESP32-C5-MINI-KIT-U) | 移植影响与硬性改动点 |
| :--- | :--- | :--- | :--- |
| **核心模组** | ESP32-C5-WROOM-1 (或定制封装) | **ESP32-C5-MINI-1 (N4)** | 同样支持 2.4G + 5G 双频 `esp-sdr` 快照捕获，射频能力 100% 相同 |
| **天线接口** | IPEX 外置双频天线 | **U版带 IPEX 外置双频 FPC 天线** | 5 GHz 与 2.4 GHz 灵敏度均有保障 |
| **Flash 容量** | **32 MB** | **4 MB** (`0x400000`) | **【致命点 1】** 原版 `partitions.csv` 中 `factory` 分区占 4MB、`jd_nvs` 起始偏移在 `0x410000`（超过 4MB 边界），直接刷入会报错无法启动，**必须缩减分区表**！ |
| **PSRAM** | **8 MB PSRAM** (`CONFIG_SPIRAM=y`) | **无 PSRAM (0 MB)** | **【致命点 2】** 原版把 LVGL、FFT 缓冲、双频频谱缓冲、检测器历史全部放在 `MALLOC_CAP_SPIRAM`（约 300+ KB），且开启了 `CONFIG_SPIRAM=y`。无 PSRAM 时必须全部迁回内部 384 KB SRAM，并**精简内存账本**！ |
| **内部 SRAM** | 384 KB（其中 `0x40820000~0x40840000` 128 KB 被射频 ADC 征用） | 同左（静态段 `_bss_end` 必须 `< 0x40820000`，可用内部 Heap 约 180~210 KB） | 必须仔细控制 2.4G + 5G 缓冲区的 RAM 占用，防止 Heap 耗尽 (`ESP_ERR_NO_MEM`) |
| **板载外设** | ST7796 屏幕、FT6336 触摸、ES8311 音频、AXP2101 电源、CH32V006 扩展、TF 卡槽 | **GPIO27**：板载 WS2812B RGB 灯<br>**GPIO28**：BOOT 按键 (Active Low)<br>**USB Type-C**：原生 USB Serial/JTAG | 剥离 I2C/SPI/I2S/LCD/TF 卡驱动，改用 **WS2812B RGB 状态灯 + BOOT 键 + USB 串口** |

---

## 二、 内部 SRAM 内存账本精算（无 PSRAM 双频共存方案）

ESP32-C5 内部共有 384 KB SRAM，其中 `sram_guard.ld` 锁定了 `0x40820000 ~ 0x40840000`（128 KB）专供 `adctrig` 射频采样硬件 DMA 使用。除去 Wi-Fi/PHY 静态段和 FreeRTOS 任务栈，留给应用程序的动态内部 Heap 约 **190 KB ~ 210 KB**。

如果不做裁剪直接将 `MALLOC_CAP_SPIRAM` 改为 `MALLOC_CAP_DEFAULT`，原版内存需求如下：
1. **`sweep.c` DSP 与频谱缓冲**：
   * FFT 工作区 (`window`, `twiddle`, `work`, `order`, `s_iq`, `s_power`, `s_avg`, `s_max`, `s_sum`, `s_seg_*`) ≈ **31 KB**
   * 2.4 GHz (268 bins) `avg/max/hold` ≈ **1.6 KB**
   * 5 GHz (2384 bins) `avg/max/hold` ≈ **14.3 KB**（注：无屏模式下如果不需要 UI 最大保持，`hold_cdb` 甚至可以省掉）
2. **`detector.c` 双缓冲与检测器工作区（大头！）**：
   * `detector.c` 为每个频段维护了双缓冲 `spec[2][3]`（2 个 slot × `avg/min/max`）：
     * 2.4 GHz：`2 × 3 × 268 × 2B` ≈ **3.2 KB**
     * 5 GHz：`2 × 3 × 2384 × 2B` ≈ **28.6 KB**（注：无屏且采集与检测串行或单缓冲时，可从 2 个 slot 降为 1 个 slot，立省 14.3 KB）
   * `detect.c` 中每个频点（bin）占用：`base_q8`(4B) + `cw_hist`(4B) + `scratch`(2B) + `mark`(1B) + `learn_bin`(`2B × DETECT_LEARN_MAX`)：
     * 原版 `DETECT_LEARN_MAX = 32` 时：每 bin 占 **75 Bytes** → 5 GHz (2384 bins) 高达 **191 KB**！（直接撑爆内部 SRAM）
     * **优化方案**：将 `DETECT_LEARN_MAX` 从 `32` 调整为 **`8`**（基线学习期保留 8 个重采样分位点，对中位数估计精度影响极小），每 bin 仅占 **27 Bytes**！
       * 2.4 GHz `detect_mem_bytes`：从 21.8 KB 降至 **~7.8 KB**
       * 5 GHz `detect_mem_bytes`：从 191 KB 降至 **~69.5 KB**
   * 基线保存临时缓冲 `s_rec`：5 GHz 约 **10.1 KB**（可在保存时动态 `malloc/free`，平时占 **0 KB**！）

> **结论**：只要将 `DETECT_LEARN_MAX` 设为 `8`（或 `12`），并在无屏模式下完全移除 LVGL 和 UI 瀑布图缓存，**2.4 GHz + 5 GHz 双频全功能检测总 Heap 占用仅需 ~145 KB**，在无 PSRAM 的 ESP32-C5-MINI-KIT-U 上可以非常充裕、稳定地 7×24 小时运行！

---

## 三、 已锁定的定制方案（User Decisions）

根据你的选择，本项目的魔改架构已经锁定如下：

1. **工程结构方式：【1A - 双板宏开关共存】**
   * 通过 `Kconfig.projbuild` 中的配置宏（如 `CONFIG_JD_BOARD_TOUCH_LCD_3_5` 与 `CONFIG_JD_BOARD_C5_MINI_KIT`）区分两款开发板。
   * 保留原版全部文件与 git 提交历史兼容性，方便随时向上游或自己的 GitHub 仓库 `git push`。
   * 为 `ESP32-C5-MINI-KIT-U` 提供专属的 `sdkconfig.defaults.c5_mini_kit` 与 `partitions_4mb.csv`。

2. **板载 WS2812B RGB 灯（GPIO27）：【2A - 独立封装的全状态灯效模块】**
   * 封装为独立清爽的组件/模块（如 `status_led.h` / `status_led.c`），对外暴露简洁的状态更新接口：
     * **自动量程 (`AUTORANGE`)**：紫色快闪
     * **基线学习 (`LEARNING`)**：蓝色呼吸/慢闪
     * **正常监控 (`NORMAL`)**：绿色微亮（或低占空比心跳）
     * **可疑干扰 (`SUSPECT`)**：黄色/琥珀色中速闪烁
     * **确认压制 (`ALARM`)**：红色高频爆闪

3. **上位机与串口交互：【3B - 保持 JSON 协议 + 频谱流扩展 + 电脑端 Python 实时频谱/瀑布图上位机】**
   * 保持原版 `link.c` 的 JSON Lines 协议（100% 兼容 Home Assistant 插件）。
   * 扩展串口频谱订阅指令，并配套编写电脑端 `tools/pc_viewer.py` 上位机，插上 USB 即可在电脑端实时查看 2.4 GHz / 5 GHz 频谱、最大保持、学习基线、瀑布图和报警面板。

4. **外接声音报警：【4B - 预留硬件 LEDC PWM 蜂鸣器 GPIO（零 CPU 抢占）】**
   * **技术评估**：ESP32-C5 内置硬件 **LEDC (PWM) 外设**。开启/关闭 2 kHz 方波蜂鸣器只需写一次寄存器（耗时 `< 2 µs`），由硬件定时器自动翻转引脚，**完全不走 I2S DMA，也不占用任何主循环/CPU 时间，更不会像 SPI 刷屏那样抢占 `sweep` 射频采样**！
   * 因此我们将预留一个可配置的蜂鸣器引脚（如默认 `GPIO4`，可在 Kconfig 中修改或关闭），用纯硬件 LEDC + FreeRTOS 软件定时器翻转开关即可。

5. **BOOT / RESET 按键：【完全不作为日常功能键（仅开发/刷机使用）】**
   * 微雪 `ESP32-C5-MINI-KIT-U` 板载的 `BOOT` 和 `RESET` 是超小贴片微动按键，手感紧且不便日常按压，仅适合救砖/强制进入下载模式。
   * **完全移除按键轮询任务**（省去 GPIO28 轮询定时器）。所有日常操作（重新学习基线 `relearn`、重新校准增益 `autorange`、灵敏度调节、频段开关）统一通过 **USB 串口命令 / 电脑端 Python 上位机按钮 / Home Assistant 面板** 一键触发（如有外接大按键需求，未来可随时在排针 GPIO 上扩展）。

---

## 四、 实施阶段划分（含阶段 0：先硬件体检(B) 再搭环境跑通 HelloWorld(A)）

### 阶段 0：拆开一块 `ESP32-C5-MINI-KIT-U` 硬件体检 + 跑通 HelloWorld & RGB 灯
在动任何复杂射频代码前，按 **“先 B（esptool 极速硬件体检）再 A（搭建锁定版 ESP-IDF 编译并烧录 HelloWorld）”** 的顺序执行：
1. **步骤 0-B：esptool 极速硬件与 Flash 体检**：
   * 安装 `esptool` 与 `pyserial`，扫描板子插入后的 COM 端口。
   * 读取 `ESP32-C5-MINI-KIT-U` 的芯片型号、Revision、Wi-Fi 6 双频 MAC 地址、Flash 厂商与真实容量（确认 4MB Flash 完好），并读取原始出厂串口日志。
2. **步骤 0-A：搭建锁定版 ESP-IDF 环境并烧录 `HelloWorld + GPIO27 RGB 灯`**：
   * 拉取底层 `esp-sdr` 强依赖的指定 ESP-IDF commit（`25fe69f946311abdaf9ad56591f25fedbc20ac98`）并安装 `esp32c5` 工具链。
   * 编译并烧录阶段 0 验收程序：验证 4 MB Flash 烧录正常、USB Serial/JTAG 串口每秒打印 `Hello World from ESP32-C5-MINI-KIT-U!`、板载 GPIO27 WS2812B 彩灯正常循环呼吸变色。

### 阶段 1：工程双板宏配置与 4MB 分区表适配
1. 创建 `partitions_4mb.csv`：
   * `nvs` (24 KB)、`phy_init` (4 KB)、`factory` (2.5 MB)、`jd_nvs` (256 KB，偏移 `0x290000`，位于 4 MB 范围内)。
2. 在 `main/Kconfig.projbuild` 中增加板型宏开关与蜂鸣器/RGB 引脚配置，并创建 `sdkconfig.defaults.c5_mini_kit`（设置 `4MB Flash`、关闭 `CONFIG_SPIRAM` 和 LVGL）。

### 阶段 2：板级支持包（`components/board`）双板隔离与 RGB/蜂鸣器封装
1. 通过宏隔离原版 LCD/I2C/PMU/Touch 代码，为 `C5-MINI-KIT-U` 提供极简 `board.c` 实现（零开销 `board_rf_quiet_*`，无需轮询贴片小按键）。
2. 独立封装 `status_led`（GPIO27 WS2812B RMT 驱动，含 5 种工作状态灯效）与硬件 `ledc` PWM 蜂鸣器接口（零 CPU 抢占）。

### 阶段 3：内存与检测引擎适配（无 PSRAM 双频 2.4G + 5G 运行）
1. 在无 PSRAM 模式下将 `main/detect.h` 中的 `DETECT_LEARN_MAX` 自动设为 `8`。
2. 将 `sweep.c` 和 `detector.c` 中的 `MALLOC_CAP_SPIRAM` 抽象为自动适配宏（有 PSRAM 用 SPIRAM，无 PSRAM 用 DEFAULT）。
3. 将 `detector.c` 中用于保存基线的 10 KB 缓冲改为按需分配或复用，确保 2.4G + 5G 双频常驻 Heap 低于 145 KB。

### 阶段 4：主程序（`main/app_main.c`）解耦与串口频谱流扩展
1. 在 `C5-MINI-KIT-U` 模式下跳过 LVGL / TF 卡初始化，启动 `status_led`。
2. 解耦 `link.c` 的远程 `set` 命令，并在 `link.c` 中额外增加 `autorange` 远程命令（配合原有的 `relearn` 命令，完全替代物理按键）。
3. 在 `link.c` 中新增频谱导出命令，并编写电脑端 `tools/pc_viewer.py` 实时频谱 + 瀑布图上位机（带 `[Re-learn Baseline]` 和 `[Auto-range Gain]` 按钮）。

### 阶段 5：宿主机算法测试与真机联调验收
1. 运行 `test_host` 仿真测试与真机烧录，用手机测速 / 微波炉验证双频检测、RGB 状态灯及电脑端上位机。

---

## 五、 实施记录与编译 / 烧录 / 上位机命令

### 阶段 0~4 已落地改动清单
- **阶段 0（硬件排雷）**：实测 `esp-sdr` 锁定的 ESP-IDF 6.2 快照在 ESP32-C5 上官方 `rmt_tx` 会触发 `flush timeout`；改用 **240 MHz RISC-V 周期计数器 (`esp_cpu_get_cycle_count`) 纯 GPIO27 精确时序直驱 WS2812B**（单次刷新仅 ~30 µs，且包裹在 `board_rf_quiet_begin/end` 静默窗内，实机点亮变色验证通过）。
- **阶段 1（双板配置与 4MB 分区）**：新增 `partitions_4mb.csv`、`sdkconfig.defaults.c5_mini_kit`，在 `main/Kconfig.projbuild` 新增双板选择宏 `CONFIG_JD_BOARD_C5_MINI_KIT` 及 RGB/蜂鸣器引脚配置。
- **阶段 2（无屏 BSP + RGB + 硬件 PWM 蜂鸣器）**：新增 `components/board/board_c5_mini.c`（轻量射频静默互斥锁）、`main/status_led.c`（6 状态灯效：`AUTORANGE`/`LEARNING`/`NORMAL`/`SUSPECT`/`ALARM`/`FAILED` + 报警蜂鸣触发）、`main/audio.c`（硬件 LEDC PWM 蜂鸣器后端）。
- **阶段 3（无 PSRAM 双频内存适配）**：`main/detect.h` 在无 PSRAM 模式下自动设定 `DETECT_CELL_BINS = 16`（625 kHz 网格 × 16 = 10 MHz 等效单元）、`DETECT_LEARN_MAX = 6`；`main/sweep.c` 与 `main/detector.c` 自动回退 `MALLOC_CAP_DEFAULT`，并将 ~10 KB 基线保存缓冲改为按需分配/释放。
- **阶段 4（主程序解耦 + 串口频谱流 + PC 上位机）**：`main/app_main.c` 跳过 LVGL/TF 卡并启动 `status_led`；`main/link.c` 解耦远程 `set`、新增 `autorange` 与零堆开销 `spec`（100 点下采样频谱）指令；新增电脑端实时频谱/瀑布图上位机 `tools/pc_viewer.py`。
- **真机实测日志（COM5，ESP32-C5HF4 v1.2，4 MB Flash，0 PSRAM）**：
  - `headroom below capture guard 22760 B; internal heap free 8320 (largest 7680, min ever 8320), PSRAM free 0`
  - `2.4 GHz`: 2 slices (134 bins @ 625 kHz), 35 ms/sweep (2.66 sweeps/s), auto-range gain 41
  - `5 GHz`: 16 slices (1192 bins @ 625 kHz), 676 ms/sweep (1.33 sweeps/s), auto-range gain 79

### 阶段 6：串口 OTA + A/B 双分区 + Bootloader 掉电安全回滚

> 无屏设备唯一固件分发通道就是 USB 串口。本阶段把"升级"做成一个在任何
> Unhappy Path（拔线 / 坏帧 / 断电 / 坏固件）下都**不变砖**的闭环。

**固件侧（`main/ota_upgrade.c`，协议详见 `docs/LINK.md`）**
* **分区表重排（`partitions_4mb.csv`）**：`ota_0` @0x10000 与 `ota_1` @0x150000 各 1280 KB（现 app 仅 831 KB），`otadata` 8 KB；**`jd_nvs` 钉在原偏移 0x290000，已学基线零丢失迁移**，且旧 factory 就在 0x10000——存量设备只需刷入新分区表，原 app 无缝变为 ota_0 继续运行。
* **传输 = JSON 控制面 + 二进制帧数据面**：`ota_begin(v=镜像字节数)` 后链路切换为 `[u32 seq][u32 len][u32 crc32][payload≤448B]` 小端二进制帧，**严格停等**（460 B 帧 ≤ 链路 512 B RX 环形缓冲；`esp_ota_begin` 预擦除期间 CPU 冻结数秒，begin 的 ack 在擦除后才发出，上位机等 60 s）。逐帧 CRC32（zlib 兼容），终验交给 `esp_ota_end` 的镜像 SHA-256——传输层只抓误码，完整性是 bootloader 级的。
* **安全态门禁**：任一频段处于 `ALARM`（等价医疗场景"正在刺激/采集中"）时 `ota_begin` 直接拒绝（`active alarm`），报警状态机绝不被升级打断。
* **回滚（bootloader 裁决）**：`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`；新固件以 `PENDING_VERIFY` 启动，`ota_boot_check()` 起 10 s 自检任务，`sweep_state()==SWEEP_RUNNING` 才调 `esp_ota_mark_app_valid_cancel_rollback()`（或上位机 `ota_confirm` 立即转正）。**坏固件崩溃/挂死 → 无人转正 → 下次复位 bootloader 自动回滚旧槽并标坏槽 INVALID**。
* **扫频联动**：升级期间 `sweep_pause(true)` 段间停车，abort/done 后恢复或随重启结束；帧间 5 s 静默自动 abort。LCD 板（factory-only）代码可编译、运行期 no-op。

**上位机（`tools/pc_upgrader.py`）**
按 `docs/LINK.md` 的 DTR/RTS 组合开端口避免误复位；ping 同步丢弃陈旧缓冲行；停等发帧 + 每帧 3 次重试 + 进度/ETA；`done` 后等设备重启、自动回连 `hello` 验证新版本与槽位并发送 `ota_confirm`；`--corrupt-at N` 支持单帧坏字节故障注入。

**真机验收清单（Unhappy Path 逐项过）**
1. 只刷新分区表 → 旧 app 变 ota_0 正常跑、基线保留（看 `hello` 的 `ota.img`）。
2. `pc_upgrader.py` 完整升级 → ota_1 `pending` → confirm/自检 → `valid`；再升回 ota_0，A/B 交替。
3. `--corrupt-at 50`：第 50 帧被 CRC 拦截，设备回 `oack err=crc` 并中止，扫频与 RGB 灯效恢复。
4. 升级到 50% 直接拔 USB：5 s 静默超时自动 abort，设备继续正常检测。
5. 升级写 flash 中途拔电源：重上电仍从旧槽启动（otadata 未切换）。
6. 人为刷"开机即死"固件且不复位不复位：下次复位 bootloader 回滚旧槽。

**面试考点速记（每条都能对着代码讲）**
* 为什么回滚必须由 bootloader 完成、otadata 如何原子切换（双 sector 轮换写）。
* 为什么停等而非滑动窗口：单核 CPU 在 flash 写/擦时冻结，环形缓冲仅 512 B。
* 升级中断电的三种时刻（擦除中/写帧中/otadata 切换后）分别发生什么。
* 安全态门禁为什么放在设备端而不是上位机侧。
* 扩展点（已留文档未启用）：Secure Boot V2 签名链；`CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` + eFuse `security_version` 防降级——**eFuse 不可逆，hobby 板默认不烧**。

### 快速编译与烧录（PowerShell）
```powershell
. D:\esp\esp-idf-espsdr\export.ps1
$env:IDF_TARGET = "esp32c5"
idf.py -B build-c5-mini -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults.c5_mini_kit" -D "SDKCONFIG=build-c5-mini/sdkconfig" build
idf.py -B build-c5-mini -p COM5 flash
```

### 启动电脑端实时频谱 / 瀑布图上位机
```powershell
pip install pyserial numpy matplotlib
python tools/pc_viewer.py --port COM5
```
