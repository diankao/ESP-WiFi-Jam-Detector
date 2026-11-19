# `test_host/` — 宿主机离线单元测试套件

[⬅ 返回根目录文档](../README_CN.md)

本目录提供了一套**完全不依赖 ESP-IDF 和开发板硬件**、直接在电脑（Linux / macOS / Windows WSL 或 MSYS2）上使用标准 C17 编译器（`gcc` / `clang`）编译运行的单元测试。

---

## 📂 文件结构

```text
test_host/
├── Makefile            # 一键编译 5 组测试程序并调用 tools/sim_spectra.py 生成合成频谱回放
├── stub/
│   └── esp_err.h       # 极简的 esp_err_t 桩头文件（仅有少量共享头文件需要）
├── test_bandplan.c     # 频段规划测试：验证 2.4G (268 bins) 与 5G (2384 bins) 无缝拼接、无空洞、无越界
├── test_dsp.c          # 定点 DSP 测试：验证 Q15 Hann 窗 + FFT 精度、dsp_cdb 对数换算误差 (<0.03 dB)、DC 陷波、分位数
├── test_detect.c       # 检测器核心测试：回放 7 种各 160 秒的合成射频场景，验证基线学习、慢速漂移跟踪与 4 类干扰状态机
├── test_plot.c         # 光栅化渲染器测试：验证频谱曲线、峰值线、坐标轴及瀑布图 RGB565 像素条带绘制
└── test_link.c         # USB 协议测试：验证 JSON Lines 消息格式化、命令解析与边界错误处理
```

---

## ▶️ 如何运行测试

只需系统装有 C 编译器（`cc`/`gcc`）和 `python3`（仅用标准库，无需安装任何第三方 pip 包）：

```bash
make -C test_host
```

执行流程：
1. 编译 `test_dsp`、`test_bandplan`、`test_plot`、`test_link`、`test_detect` 五个可执行文件到 `test_host/build/`。
2. 自动运行 `../tools/sim_spectra.py --out build/sim` 生成 7 种典型射频环境的双频合成频谱数据。
3. 依次运行 5 个测试程序，并用 Python `json.loads` 校验 `test_link` 输出的所有 JSON 样本格式合法。

---

## 🧪 `test_detect.c` 回放的 7 大射频验证场景

每个场景模拟 **160 秒**真实扫频时间轴（`0~60 s` 基线学习期，`70~130 s` 注入事件，`130~160 s` 恢复期）：

| 场景名称 | 模拟信号构成 | 2.4 GHz 预期结果 | 5 GHz 预期结果 |
| :--- | :--- | :---: | :---: |
| **`clean`** | 正常路由器信标 (Beacons)、低占空比 BLE 跳频、固定本振杂散 | `NORMAL` | `NORMAL` |
| **`busy_wifi`** | 多信道 `iperf` 级高负载 Wi-Fi 突发流量（80~90% 信道占用率） | `SUSPECT (pulsed)`，**绝不误报 ALARM** | `NORMAL` |
| **`wideband`** | 全频段宽带底噪持续抬升（2.4G +15 dB / 5G +12 dB） | 约 7 秒后触发 **`ALARM (wideband)`** | 约 8 秒后触发 **`ALARM (wideband)`** |
| **`cw`** | 定频单音连续波干扰（2.4G +25 dB / 5G +20 dB） | 约 11 秒后触发 **`ALARM (narrowband)`** | 约 12 秒后触发 **`ALARM (narrowband)`** |
| **`swept`** | 锯齿波扫频干扰（2.4G 20s 周期 / 5G 40s 周期） | 约 19 秒后触发 **`ALARM (swept)`** | 约 32 秒后触发 **`ALARM (swept)`** |
| **`microwave`** | 50 Hz 交流电半波门控、中心频率缓慢漂移的磁控管宽带辐射 | `SUSPECT (pulsed)`，**不误报 ALARM** | `NORMAL` |
| **`drift`** | 全频段底噪在 100 秒内缓慢温漂 +4 dB | EMA 平滑跟踪，保持 `NORMAL` | 保持 `NORMAL` |
