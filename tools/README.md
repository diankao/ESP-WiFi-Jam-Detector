# `tools/` — 合成射频频谱仿真生成器

[⬅ 返回根目录文档](../README_CN.md)

本目录包含 `sim_spectra.py`，用于在**不发射任何真实无线电干扰信号**的前提下，按照 ESP32-C5 真实的扫频时序物理模型生成测试频谱数据，驱动 [`../test_host/test_detect.c`](../test_host/README.md) 验证检测器算法。

---

## 📂 文件说明

* **`sim_spectra.py`**：纯 Python 3 标准库编写（零外部依赖）。

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
