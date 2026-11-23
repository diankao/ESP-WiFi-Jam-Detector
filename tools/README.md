# `tools/` — 仿真数据生成器与上位机工具

[⬅ 返回根目录文档](../README_CN.md)

本目录包含 `sim_spectra.py`（合成频谱仿真）与 `pc_viewer.py` / `pc_upgrader.py`（USB 串口上位机）。

---

## 📂 文件说明

* **`sim_spectra.py`**：纯 Python 3 标准库编写（零外部依赖）。
* **`pc_viewer.py`**：实时频谱 / 瀑布图上位机（`pip install pyserial numpy matplotlib`），用法见其文件头注释。
* **`pc_upgrader.py`**：串口固件升级上位机（OTA A/B 双分区 + 回滚，协议见 [`../docs/LINK.md`](../docs/LINK.md)）。

### 仿真物理模型细节
为了确保仿真数据与真机高度一致，`sim_spectra.py` 精确复刻了真机的采样时间窗：
* **频段交替权重**：2.4 GHz 扫 2 次、5 GHz 扫 1 次（`SCHEDULE = (0, 0, 1)`）。
* **调谐与快照间隔**：
  * 每个频点连续采样 $K=4$ 次快照，快照间隔 `5.25 ms`。
  * 2.4 GHz 每次换频耗时 `20 ms`，5 GHz 每次换频耗时 `35 ms`，DSP 处理耗时 `13 ms`。
* **信号模型类**：
  * `Tone`：单音载波（考虑 Hann 窗主瓣在相邻两个 312.5 kHz 网格间的能量分配）。
  * `WiFi`：具有 802.11 OFDM 频谱掩模、服从双状态马尔可夫过程（Markov on/off bursts）的突发数据流。
  * `BLE`：随机跳频的窄带短脉冲。
  * `Wideband`：覆盖指定频率范围的高斯白噪声底噪抬升。
  * `SweptTone`：按指定周期在频段内单向锯齿扫频的移动强峰。
  * `Microwave`：受 50 Hz 正弦半波门控（每 20 ms 周期仅约一半时间导通）、且载波在 2450 MHz 附近来回扫动的磁控管泄漏模型。

---

## ▶️ 命令行用法

```bash
# 生成全部 7 个场景的 .bin 频谱文件到指定目录
python3 tools/sim_spectra.py --out test_host/build/sim

# 仅生成指定场景（如 microwave 和 swept），并指定随机种子测试鲁棒性
python3 tools/sim_spectra.py --out /tmp/sim --seed 42 --only microwave swept
```

### 输出二进制格式（`JDSIM2`）
每个 `.bin` 文件以 8 字节魔数 `b"JDSIM2\0\0"` + `uint32` 记录总数开头，随后每条扫频记录包含：
* 频段 ID (`0=2.4G`, `1=5G`)、`n_bins`、扫频起止毫秒时间戳 `t_start_ms` / `t_ms`、增益档位、削波快照数、总快照数。
* 紧跟三个长度为 `n_bins` 的 `int16` 数组：`avg_cdb`、`min_cdb`、`max_cdb`（单位 `0.01 dB`）。

---

## ▶️ 串口固件升级（`pc_upgrader.py`）

把 A/B 分区固件（`sdkconfig.defaults.c5_mini_kit` 构建）经 USB Serial/JTAG 推进设备：
停等二进制帧 + CRC32 逐帧校验，设备端 `esp_ota_end` 做镜像 SHA-256 终验，
新固件以 PENDING_VERIFY 启动，10 秒自检（或 `ota_confirm`）后转正，否则 bootloader 回滚旧槽。

```bash
pip install pyserial

# 自动探测端口，升级最近一次 build_c5_mini 的产物，重启后自动回连验证并 confirm
python tools/pc_upgrader.py

# 指定端口与镜像
python tools/pc_upgrader.py --port COM5 --bin build-c5-mini/esp-wifi-jammer-detector.bin

# 故障注入测试：第 50 帧故意翻转 1 字节，设备应回 oack err=crc 并中止升级
python tools/pc_upgrader.py --corrupt-at 50
```

升级前置条件与失败行为（`active alarm` 门禁、拔线 5 秒自动中止、断电保持旧固件运行等）
详见 [`../docs/LINK.md`](../docs/LINK.md) 的 "Serial OTA upgrade" 一节。
