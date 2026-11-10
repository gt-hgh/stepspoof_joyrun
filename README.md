# stepspoof_joyrun

纯 native Zygisk 模块，针对**闪动校园**（`com.huachenjie.shandong_school`），通过 AArch64 inline hook 拦截 `libsensor.so` 的 `SensorEventQueue::read`，改写加速度计波形和计步器偏移量，并通过 KSU WebUI 提供实时控制面板。

A pure-native Zygisk module targeting **ShanDong Campus** (`com.huachenjie.shandong_school`). It intercepts `SensorEventQueue::read` in `libsensor.so` via AArch64 inline hook, rewrites accelerometer waveforms and step-counter offsets, and provides a real-time control panel through KSU WebUI.

> **兼容性 / Compatibility**：Android 14+（`arm64-v8a`）；KernelSU + Zygisk（如 KernelSU Ultra / ZygiskNext）；**无需 LSPosed / No LSPosed required**。

---

## 特性 / Features

- **加速度波形仿真 / Accelerometer Waveform Spoofing** — 合成符合自研计步器带通滤波特征的正弦波形，按设定步频驱动。Synthesizes sine waveforms matching the band-pass filter characteristics of the app's built-in step detector, driven by the configured cadence.
- **步数偏移 / Step Offset** — STEP_COUNTER / STEP_DETECTOR 事件支持偏移模式和绝对值模式。Supports offset mode and absolute-value mode for STEP_COUNTER / STEP_DETECTOR events.
- **步频仿真线程 / Cadence Simulation Thread** — 独立线程每 500 ms tick；步频在 `[min, max]` 范围内随机锁定一次（整段稳定，模拟真实跑步节律）。A dedicated thread ticks every 500 ms; cadence is randomly locked once within `[min, max]` and held stable throughout the session, mimicking real running rhythm.
- **KSU WebUI** — 通过 KernelSU Manager "Open WebUI" 访问控制面板。Access the control panel via KernelSU Manager → Open WebUI.
- **HTTP API（`127.0.0.1:8888`）** — WebUI 双通道：直连 + `toybox nc` 回退，绕过 mixed-content 拦截。Dual-channel WebUI: direct fetch + `toybox nc` fallback, bypassing mixed-content blocking.
- **免 LSPosed / LSPosed-Free** — Zygisk 注入不触发目标 App 的 hook 框架检测。Zygisk injection does not trigger the target app's hook-framework detection.

---

## 技术原理 / Technical Overview

### 检测规避 / Detection Evasion

目标 App 集成的加固 SDK（易盾）在启动时校验：
The target app's packing SDK (NetEase Easy Shield) checks at startup:

- 进程 maps / fd 是否含 Frida / LSPlant / LSPosed 匿名可执行段
- `/proc/self/maps` 是否含可疑路径（`templib/`, `databases/`, `/linker`）
- ptrace 状态、`/proc/pid/mem` 访问特征

**不校验 / Not checked**：Zygisk 自身加载的 `.so`、`/data/adb` 路径、zygisk 字符串。

因此纯 native Zygisk 模块可规避此类检测，而任何 LSPosed 模块进入作用域则必然触发。

A pure-native Zygisk module evades these checks, whereas any LSPosed module entering the app's scope inevitably triggers detection.

### Hook 路径 / Hook Path

Android 14+ 的 Java `SensorManager` 通过共享内存路由传感器事件，实际调用链为：
On Android 14+, Java `SensorManager` routes sensor events through shared memory. The actual call chain is:

```
Java SensorManager
  → android::SensorEventQueue::read  (libsensor.so)
  → [AArch64 inline hook 拦截]
  → patch_event()  改写 ACC / STC / STD 事件
```

NDK 层的 `ASensorEventQueue_getEvents`（`libandroid.so`）不在此路径上，仅作兜底候选。

The NDK-level `ASensorEventQueue_getEvents` (`libandroid.so`) is not on this path; it serves only as a fallback candidate.

Hook 实现：16 字节 AArch64 跳转桩（`LDR X16, #8; BR X16; <8-byte addr>`），通过 `mprotect(RWX)` 写入 `.text`，失败则回退 `/proc/self/mem pwrite`。每个候选符号装入独立槽位，`thread_local` 深度守卫防止嵌套重入。

Hook implementation: a 16-byte AArch64 jump stub (`LDR X16, #8; BR X16; <8-byte addr>`) is written to `.text` via `mprotect(RWX)`, falling back to `/proc/self/mem pwrite` on failure. Each candidate symbol occupies an independent slot; a `thread_local` depth guard prevents nested re-entry.

### 加速度波形 / Accelerometer Waveform

严格对齐 [FuckRun](https://github.com/BieFan1029/FUCK-RUN-) 原设计：
Strictly aligned with the [FuckRun](https://github.com/BieFan1029/FUCK-RUN-) original design:

| 分量 / Component | 公式 / Formula |
|---|---|
| X | `0` |
| Y | `1.5 · cos(2πft) + rand(±0.15)` |
| Z | `9.81 + 3.5 · sin(2πft) + rand(±0.15)` |

`f = cadence / 60`（步频 Hz），`t` 取自事件 `timestamp`（纳秒转秒）。

> 此前用单向 s³ 尖峰波形——绝大多数样本贴着 9.81 重力基线，经带通滤波后能量不足，计步器判定为静止。完整正负正弦才能通过带通滤波并触发步态检测。
>
> The earlier single-direction s³ spike waveform kept most samples near the 9.81 gravity baseline; after band-pass filtering the energy was insufficient and the step detector classified it as stationary. A full positive-negative sine wave is required to pass the band-pass filter and trigger step detection.

---

## 构建 / Build

**依赖 / Prerequisites**：Android NDK r27d（默认路径 `~/android/android-ndk-r27d`，可用 `ANDROID_NDK_R27D` 覆盖）

```bash
# 在 WSL / Linux 下执行 / Run under WSL or Linux
bash scripts/build.sh
# 产出 / Output: dist/stepspoof_joyrun-v1.1.8.zip
```

---

## 安装 / Install

1. 将 `dist/stepspoof_joyrun-vX.X.X.zip` 传至设备 / Transfer the zip to the device
2. KernelSU Manager → 模块 → 从本地安装 / KernelSU Manager → Modules → Install from local
3. 重启设备 / Reboot the device
4. KernelSU Manager → 模块 → 闪动校园 传感器控制 → **Open WebUI**

---

## WebUI 使用 / WebUI Usage

| 控件 / Control | 说明 / Description |
|---|---|
| 启用模块 / Enable Module | 总开关；关闭后事件不改写 / Master switch; events are not rewritten when off |
| 加速度波形仿真 / Accel Waveform Spoof | 改写 ACC 事件；App 用 400 Hz 加速度自算步数时须开启 / Rewrites ACC events; must be on when the app computes steps from 400 Hz accelerometer data |
| 步频范围（步/分钟）/ Cadence Range (spm) | 仿真步频从 `[最低, 最高]` 锁定 / Simulation cadence is locked within `[min, max]` |
| 步长（cm）/ Stride Length (cm) | 用于推算参考配速，不影响传感器改写 / Used to estimate reference pace; does not affect sensor rewriting |
| 开始仿真 / 停止仿真 / Start / Stop Simulation | 切换仿真线程状态 / Toggles the simulation thread |

> **STD 0x12 / STC 0x13 恒为 0 是正常的**
> 闪动校园只注册 `0x01`（400 Hz 加速度计），不使用硬件计步器传感器，这两项是备用通道的诊断计数器，当 App 注册硬件计步器时才会增长。
>
> **STD 0x12 / STC 0x13 always showing 0 is normal.**
> The app only registers `0x01` (400 Hz accelerometer) and does not use hardware step sensors. These two counters are diagnostics for a backup channel; they will only increment if the app registers a hardware pedometer.

---

## 配置文件 / Configuration Files

模块主进程每 700 ms 轮询以下控制文件（由 WebUI 通过 `ksu.exec` 写入）：
The module's main process polls the following control file every 700 ms (written by WebUI via `ksu.exec`):

```
/data/data/com.huachenjie.shandong_school/files/stepspoof.conf
```

格式（每行 `key=value`） / Format (one `key=value` per line):

```ini
enabled=1
offset=0
absolute=0
absolute_value=0
sim_active=0
sim_min=80
sim_max=120
sim_accel=1
stride=110
```

状态文件（每 700 ms 刷新，WebUI 通过 `ksu.exec cat` 读取）：
State file (refreshed every 700 ms, read by WebUI via `ksu.exec cat`):

```
/data/data/com.huachenjie.shandong_school/files/stepspoof.state
```

---

## HTTP API

主进程监听 `127.0.0.1:8888` / The main process listens on `127.0.0.1:8888`:

| 路由 / Route | 方法 / Method | 说明 / Description |
|---|---|---|
| `/api/status` | GET | 返回运行状态 JSON（hooks / hook_hits / patched_events / sim_steps / acc / …） / Returns runtime status JSON |
| `/api/config` | POST | 批量更新配置（JSON body） / Batch update configuration (JSON body) |
| `/api/start` | POST | 启动仿真（`enabled=true, sim_active=true`） / Start simulation |
| `/api/stop` | POST | 停止仿真（`sim_active=false`） / Stop simulation |

---

## 文件结构 / File Structure

```
├── jni/
│   ├── main.cpp            # Zygisk 模块主逻辑：inline hook + HTTP API + 仿真线程
│   ├── Android.mk
│   ├── Application.mk
│   └── zygisk.hpp          # from topjohnwu/zygisk-module-sample (API v5)
├── webroot/
│   └── index.html          # KSU WebUI 控制面板 / KSU WebUI control panel
├── META-INF/com/google/android/
│   ├── update-binary       # 安装脚本（Magisk / KSU 兼容）/ Install script (Magisk / KSU compatible)
│   └── updater-script
├── scripts/
│   └── build.sh            # NDK 编译 + zip 打包 / NDK build + zip packaging
├── module.prop
└── README.md
```

---

## 参考 / References

- [topjohnwu/zygisk-module-sample](https://github.com/topjohnwu/zygisk-module-sample) — Zygisk API v5 header
- [BieFan1029/FUCK-RUN-](https://github.com/BieFan1029/FUCK-RUN-) — Accelerometer waveform design reference

---

## License

MIT
