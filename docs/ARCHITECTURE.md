# win-ldac Architecture & Design Notes

> 本文件是项目的**架构 / 设计参考**（前身：`HANDOFF.md`）。涵盖完整的组件清单、协议要点、构建依赖、license 约束。新接手的人应先读 [`README.md`](../README.md) 了解项目概况，再读本文件了解技术细节，最后看 [`STATUS.md`](../STATUS.md) 当前进度。

---

## 0. 项目目标与范围

**目标**：在 Windows 10 (x64) 上实现一个用户态工具，让蓝牙耳机（首要目标设备：Sony WH-1000XM5）能以 **LDAC 编码**接收来自 PC 的音频，从而获得最高 990 kbps 的高码率无线音质。

**项目所有者**希望将源码分享给有自行编译能力的朋友使用——不分发预编译二进制，分发源码 + 编译说明。

**范围内**：
- Windows 用户态程序（C++17，CMake 构建）
- 接管独立 USB 蓝牙 dongle（不动 Windows 内置蓝牙）
- WASAPI loopback 抓系统音频 → LDAC 编码 → A2DP source 发送
- 配对、连接、密钥持久化、自动重连、ABR 自适应码率、错误恢复
- **(次要)** 极简 GUI 显示当前 codec / 码率 / 状态

**范围外**：
- 不写 Windows 内核驱动
- 不替换 Windows 自带蓝牙栈
- 不支持 aptX / aptX HD / AAC（架构预留接口，初版只做 LDAC + SBC 兜底）
- 不做 A2DP sink、不做 HFP、不做 AVRCP 初版
- 不分发预编译二进制（受 BTstack license 约束，见 §9）

**优先级**：**LDAC codec 功能（M1-M6）是核心目标**，必须先完成。GUI 与 IPC（M7-M9）是次要可选，打包（M10）按需。

---

## 1. 当前状态与下一步动作

**阶段**：📋 规划完成，待开工 M1

**已完成**：
- [x] 技术路线调研与可行性确认
- [x] 关键依赖 license 复核
- [x] 双进程架构决策（core / gui 分离，IPC 解耦）
- [x] 路线 2 锁定：BTstack 作为用户自拉依赖，本项目代码 Apache 2.0
- [x] 硬件需求确定（独立 USB BT dongle，推荐 CSR8510）
- [x] 本交接文档定稿（v2）

**下一步**（接手 AI 请从这里开始）：参见 §7 M1 任务。

**变更记录**：
- 2026-05-12：v1 文档（基于 BTstack 个人自用方案）
- 2026-05-12：v2 文档——切换到路线 2（源码分发）；增加双进程架构与 IPC 协议；GUI 标为次要；里程碑扩到 10 个

---

## 2. 五分钟上手（给新接手的 AI）

如果你完全没读过前面的讨论，照这个顺序读：

1. §3 看整体架构图，了解数据流
2. §4 看每个组件是什么、为什么选它
3. §7 看当前应该做哪个里程碑（**M1-M6 是核心，必须先完成**）
4. §8 知道哪些坑会让你浪费一天
5. §11 所有引用都是经过验证的官方/可信源，可以直接拿来读

**一句话总结**：一个 C++ 程序（core，无窗口），用 BTstack 接管外接 USB 蓝牙 dongle，用 libldac 把 WASAPI loopback 抓来的 PCM 编码后通过 A2DP source 发给耳机。**Windows 自带蓝牙栈不动**。另有一个**可选的** GUI 子进程（Dear ImGui），通过 named pipe 从 core 读状态，纯展示，core 在没有 GUI 的时候也能独立工作。

---

## 3. 架构与数据流

```
┌────────────────────────────────────────────────────────────────┐
│                    Windows 10 用户态                            │
│                                                                 │
│  ┌─────────────────────────────────────────┐                   │
│  │  win-ldac-core.exe（必需，无窗口）        │                   │
│  │                                          │                   │
│  │  [系统音频] ──► WASAPI loopback ──┐      │                   │
│  │  (IAudioCaptureClient, shared)    │      │                   │
│  │  AUDCLNT_STREAMFLAGS_LOOPBACK     │      │                   │
│  │                                   ▼      │                   │
│  │                          ┌─────────────┐ │                   │
│  │                          │ PCM ring buf│ │                   │
│  │                          └──────┬──────┘ │                   │
│  │                                 │        │                   │
│  │   libldac ◄─── ldacBT_encode() ─┤        │                   │
│  │   (Apache 2.0)                  ▼        │                   │
│  │                          ┌─────────────┐ │                   │
│  │                          │ A2dpLdac    │ │                   │
│  │                          │ glue (我们)  │ │                   │
│  │                          └──────┬──────┘ │                   │
│  │                                 │ RTP    │                   │
│  │                                 ▼        │                   │
│  │   ┌─────────────────────────────────┐    │                   │
│  │   │  BTstack（用户自拉到 vendor/）    │    │                   │
│  │   │  ├─ AVDTP source                │    │                   │
│  │   │  ├─ A2DP signaling (vendor)     │    │                   │
│  │   │  ├─ L2CAP                       │    │                   │
│  │   │  ├─ HCI                         │    │                   │
│  │   │  └─ windows-winusb port         │    │                   │
│  │   └────────────────┬────────────────┘    │                   │
│  │                    │ libusb / WinUSB     │  IPC server       │
│  └────────────────────┼─────────────────────┼──── ▲ ────────────│
│                       │                     │     │ named pipe  │
│                       ▼                     └──── │ JSON line   │
│             [USB BT dongle, WinUSB 驱动]          │ 10 Hz       │
│                       │                           │             │
│                       │ Bluetooth 2.4 GHz          │             │
│                       ▼                           ▼             │
│             [Sony WH-1000XM5]      ┌───────────────────────┐    │
│                                    │ win-ldac-gui.exe（可选）│   │
│                                    │ Dear ImGui + DX11      │   │
│                                    │ 纯展示，不影响音频      │   │
│                                    └───────────────────────┘    │
└────────────────────────────────────────────────────────────────┘
```

**关键边界**：
- **core 是独立完整的**：CLI 运行即可，不需要 GUI 也能跑通整个 LDAC 链路
- BTstack 提供 HCI/L2CAP/AVDTP/A2DP-signaling，**不**提供 codec
- libldac 只是纯编码器，**不**懂蓝牙
- A2dpLdac glue 是我们写的粘合层（移植自 AOSP）
- GUI 通过 named pipe 读 core 的状态、写命令，**与 core 进程完全独立**，崩了/关掉都不影响音频

---

## 4. 组件清单

### 4.1 libldac — LDAC 编码器

| 字段 | 值 |
|---|---|
| 用途 | PCM → LDAC 编码帧 |
| 上游 | [android.googlesource.com/platform/external/libldac](https://android.googlesource.com/platform/external/libldac) |
| 集成方式 | 通过 [EHfive/ldacBT](https://github.com/EHfive/ldacBT) 提供的 CMake 包装，作为 git submodule |
| License | **Apache 2.0**（Sony 版权，允许 SRC 端免费使用） |
| 语言 | C |
| 在仓库的位置 | `vendor/ldacBT/`（submodule，由项目自动 clone） |
| 编译目标 | `libldacBT_enc.lib`、`libldacBT_abr.lib` |

**关键 API**（从 `ldacBT.h`）：

```c
HANDLE_LDAC_BT ldacBT_get_handle(void);
void           ldacBT_free_handle(HANDLE_LDAC_BT hLdacBT);

int ldacBT_init_handle_encode(
    HANDLE_LDAC_BT hLdacBT,
    int mtu,                  // A2DP 协商后的 MTU
    int eqmid,                // LDACBT_EQMID_HQ/SQ/MQ (990/660/330 kbps)
    int channel_mode,         // STEREO/DUAL/MONO
    LDACBT_SMPL_FMT_T fmt,    // S16/S24/S32/F32
    int sampling_freq);       // 44100/48000/88200/96000

int ldacBT_encode(
    HANDLE_LDAC_BT hLdacBT,
    void *p_pcm,
    int  *pcm_used,
    unsigned char *p_stream,
    int  *stream_sz,
    int  *frame_num);

int ldacBT_get_bitrate(HANDLE_LDAC_BT);
int ldacBT_get_error_code(HANDLE_LDAC_BT);

// ABR (自适应码率)
HANDLE_LDAC_ABR ldac_ABR_get_handle(void);
int ldac_ABR_Init(HANDLE_LDAC_ABR, unsigned int interval_ms);
int ldac_ABR_Proc(HANDLE_LDAC_BT, HANDLE_LDAC_ABR,
                  size_t txq_len, unsigned int flag_enable);
```

调用序列：`get_handle → init_handle_encode → loop{ encode } → free_handle`

---

### 4.2 BTstack — 用户态蓝牙协议栈（**用户自拉依赖**）

| 字段 | 值 |
|---|---|
| 用途 | HCI + L2CAP + AVDTP + A2DP signaling + windows-winusb 传输 |
| 上游 | [github.com/bluekitchen/btstack](https://github.com/bluekitchen/btstack) |
| License | **个人/非商业免费**；商业需向 BlueKitchen GmbH 付费授权 |
| 语言 | C |
| 使用的 port | `port/windows-winusb/` |

**⚠️ 关于本项目如何对待 BTstack 的 license（路线 2 关键约定）**：

- 我们的仓库**不包含** BTstack 源码或任何编译产物
- 我们的 `CMakeLists.txt` 假定 `vendor/btstack/` 已存在，但**不**在 `.gitmodules` 里声明 BTstack（避免视为本项目的一部分）
- README 写"用户须自行 clone BTstack 到 vendor/btstack/"，附 BTstack 上游链接和 license 文件
- 每位最终用户在自己机器上 build，是 BTstack 个人许可下的合法行为
- **本项目不分发预编译二进制**

**关键 API**：
- `hci_init()` / `hci_power_control()` — 启动 HCI
- `l2cap_init()`、`avdtp_source_init()`、`a2dp_source_init()` — 协议层
- `avdtp_register_media_codec_category(..., AVDTP_CODEC_NON_A2DP=0xFF, media_codec_info, info_len)` — **关键**：注册 LDAC 流端点
- `a2dp_source_stream_send_media_payload_rtp()` — 推送编码后的媒体数据

**LDAC vendor codec capability 字节布局**（来源：AOSP `a2dp_vendor_ldac_constants.h`）：

```
偏移  长度  字段                值
0     4    Vendor ID           0x0000012D  (Sony, little-endian)
4     2    Vendor codec ID     0x00AA      (LDAC, little-endian)
6     1    采样率支持位图       e.g. 0x34 (48k|88.2k|96k)
7     1    声道模式支持位图     e.g. 0x07 (Mono|DualCh|Stereo)
```

---

### 4.3 LDAC ↔ A2DP 胶水层（我们的代码）

| 字段 | 值 |
|---|---|
| 用途 | 把 libldac 输出按 AVDTP MEDIA PDU 规范封包，处理 MTU、RTP 时间戳、帧聚合、欠载补静音、ABR 反馈 |
| 参考实现 | AOSP [`a2dp_vendor_ldac_encoder.cc`](https://android.googlesource.com/platform/packages/modules/Bluetooth/+/refs/heads/main/system/stack/a2dp/) (~650 行) |
| License | **Apache 2.0**（我们的代码） |
| 语言 | C++17 |
| 在仓库的位置 | `core/src/a2dp_ldac/` |

**移植要点**：
- `a2dp_vendor_ldac_send_frames()` — 主循环，按 20 ms 间隔编码并发送
- AVDTP MEDIA PDU 头：1 byte 帧数（高位 fragmentation 标志）+ N 个 LDAC 帧
- RTP 时间戳按采样率累加（48 kHz 时每 20ms 加 960）
- PCM 欠载时补静音帧而不是阻塞（保证间隔稳定）
- ABR：把 `a2dp_source_t.txq_len` 喂给 `ldac_ABR_Proc()` 调整 EQMID

---

### 4.4 WASAPI loopback — 系统音频抓取

| 字段 | 值 |
|---|---|
| 用途 | 抓 Windows 默认输出设备的混音流 |
| 文档 | [Loopback Recording (MS Docs)](https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording) |
| API | `IMMDeviceEnumerator → IMMDevice → IAudioClient → IAudioCaptureClient` |
| 语言 | C++ (COM) |
| 在仓库的位置 | `core/src/audio/wasapi_loopback.cpp` |

**关键参数**：
- 模式：`AUDCLNT_SHAREMODE_SHARED`（loopback 不支持 exclusive）
- 标志：`AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK`
- 拿到的格式：Windows 混音引擎决定（通常 32-bit float 48 kHz stereo）→ 需要转格式后再喂 libldac
- 必须设置事件 handle，Windows 每次有数据 signal 该事件

---

### 4.5 USB 蓝牙 Dongle 与 Zadig

**支持的芯片家族**：

| 家族 | 例子 | 固件 | 支持状态 |
|---|---|---|---|
| **Realtek RTL8761B / RTL8761BU** | TP-Link UB4A / UB400 v2 / UB500、Orico BTA-508、EDUP EP-9619、Ugreen 80889 | 需要每次启动加载 `.bin`（**已 bundle 在 `firmware/`**） | ✅ **首选**——市面主流，便宜易买 |
| **CSR8510 (Cambridge Silicon Radio)** | CSR 4.0 nano dongle、TP-Link UB400 v1（旧版）、各种白牌"CSR 4.0" | 烧在 ROM，零额外文件 | ✅ 也支持，但 CSR 已停产，越来越难买 |
| RTL8821C / RTL8822C / RTL8723D | 同款 OEM 各家 dongle | 同 Realtek，需要对应 `.bin` 文件放入 `firmware/`（见 `firmware/README.md`） | 🟡 BTstack 支持但本项目未集成测试 |
| Intel AX200 / AX210 | Wi-Fi+BT 组合卡 | 复杂的多阶段 firmware load | ❌ 未支持，工作量大 |
| Broadcom BCM2070x | 旧 Apple/MacBook BT 模块 | 类似 Realtek | ❌ 未支持 |

**Windows 驱动**：所有 dongle 都必须通过 [Zadig](https://zadig.akeo.ie/) 把驱动从 Microsoft Bluetooth 切到 **WinUSB**。

**TP-Link UB4A 特别说明**：
- 芯片：RTL8761BU；USB 描述符：VID `2357` PID `0604`（TP-Link 自家品牌的 ID）
- BTstack 的 Realtek 芯片表里**没有** `0x0604`，但有 `0x8771`（Realtek 参考板 RTL8761BU PID）
- 我们的代码硬编码用 `0x8771` 查表 → 找到正确的 `rtl8761bu_fw` + `rtl8761bu_config` 文件，固件正常加载

**前置必做**：
- 🔧 **关闭 Windows "Memory Integrity / Core Isolation"**（设置 → 隐私和安全性 → Windows 安全中心 → 设备安全性 → 内核隔离），否则 Zadig 装 WinUSB 会失败

首次设置流程：
1. 关闭 Memory Integrity，重启
2. 插上 dongle
3. 打开 Zadig，Options → List All Devices
4. 选中 dongle（**注意 VID/PID 别选错成内置蓝牙**）
5. Driver 选 WinUSB，点 Replace Driver
6. 跑我们的程序——core 进程会自动从 `firmware/` 目录加载 RTL 固件

**程序在哪里找 firmware 目录**：
- 从 exe 所在目录起，**向上找最多 5 层**，找到第一个名为 `firmware/` 的目录就用
- 这样 `build/core/Release/hci_scan_test.exe` 能找到 `D:\...\ldac\firmware`，分发出去的打包版（exe 同目录有 firmware）也能找到

---

### 4.6 IPC 协议（core ↔ gui，**M7 才实现，但协议先定**）

| 字段 | 值 |
|---|---|
| 传输 | Windows Named Pipe |
| 路径 | `\\.\pipe\win-ldac` |
| 服务端 | `win-ldac-core.exe`（启动时 CreateNamedPipe） |
| 客户端 | `win-ldac-gui.exe`（启动时 ConnectNamedPipe） |
| 编码 | UTF-8，**一行一个 JSON 对象**（NDJSON），换行符 `\n` |
| 频率 | core → gui：100 ms / 次状态推送，状态变化时额外推 |
| 库 | [nlohmann/json](https://github.com/nlohmann/json)（MIT，header-only）|

**core → gui 消息**（status 事件）：

```json
{
  "type": "status",
  "ts_ms": 1234567890,
  "core": {
    "version": "0.1.0",
    "uptime_s": 342
  },
  "dongle": {
    "present": true,
    "bd_addr": "00:1A:7D:DA:71:13",
    "chipset": "CSR8510"
  },
  "link": {
    "connected": true,
    "device_name": "WH-1000XM5",
    "device_addr": "AC:80:0A:00:00:00",
    "rssi_dbm": -52,
    "mtu": 679,
    "txq_len": 3
  },
  "stream": {
    "active": true,
    "codec": "LDAC",
    "eqmid": "HQ",
    "bitrate_kbps": 990,
    "sample_rate_hz": 48000,
    "bit_depth": 16,
    "channels": 2,
    "channel_mode": "STEREO",
    "latency_ms_est": 118,
    "underruns_total": 0
  }
}
```

**core → gui 消息**（log 事件，可选）：

```json
{"type":"log","ts_ms":1234567890,"level":"info","msg":"Connected to WH-1000XM5"}
```

**gui → core 命令**：

```json
{"type":"cmd","id":42,"cmd":"set_eqmid","value":"ABR"}
{"type":"cmd","id":43,"cmd":"connect","bd_addr":"AC:80:0A:00:00:00"}
{"type":"cmd","id":44,"cmd":"disconnect"}
{"type":"cmd","id":45,"cmd":"scan","duration_s":10}
{"type":"cmd","id":46,"cmd":"quit"}
```

**core → gui 命令响应**：

```json
{"type":"cmd_result","id":42,"ok":true}
{"type":"cmd_result","id":43,"ok":false,"error":"page_timeout"}
```

**重要原则**：
- core 在没有 gui 连接时**正常运行**，状态事件丢弃即可
- gui 启动若发现 pipe 不存在，可选择 `spawn` core 进程（参数 `--no-spawn-gui`）
- 协议向前兼容：未知字段忽略，未知命令返回 `{"ok":false,"error":"unknown_cmd"}`

---

### 4.7 GUI（**次要、可选**）

| 字段 | 值 |
|---|---|
| 用途 | 展示当前 codec / 码率 / 设备 / 实时曲线；提供少量控制 |
| 优先级 | **次要**——只有 M1-M6 全部完成后才动 |
| 实现 | [Dear ImGui](https://github.com/ocornut/imgui) (MIT) + [ImPlot](https://github.com/epezent/implot) (MIT) + DirectX 11 + Win32 backend |
| 字体 | Segoe UI（系统自带）作 fallback，可选嵌入 Inter |
| 体积预算 | 单 exe 不超过 3 MB |
| 进程独立 | ✅ 完全独立进程，崩溃不影响 core 音频 |
| 在仓库的位置 | `gui/`（独立 CMake target，独立 exe） |

GUI 通过 §4.6 IPC 与 core 通信，**不**直接调用 core 任何函数。

---

## 5. 仓库目录布局

```
ldac/
├── README.md               ← GitHub 首页 / 用户面向 build & setup
├── README.zh.md            ← 中文版 README
├── LICENSE                 ← Apache 2.0
├── NOTICE                  ← 第三方组件版权 / 许可
├── STATUS.md               ← 滚动里程碑状态
├── build.ps1               ← 一键 build 脚本
├── CMakeLists.txt          ← 顶层
├── .gitignore
├── .gitmodules             ← vendor/ldacBT + third_party/imgui + third_party/implot
│                              (不含 BTstack — license 限制)
├── docs/
│   └── ARCHITECTURE.md     ← 本文件（前身 HANDOFF.md）
│
├── firmware/               ← Realtek BT 固件（可分发，见 firmware/LICENCE...txt）
│   ├── rtl8761bu_fw        ← RTL8761BU 主固件 (注意无 .bin 后缀, 见 §8 坑 13)
│   ├── rtl8761bu_config    ← RTL8761BU 板级配置
│   └── LICENCE.rtlwifi_firmware.txt
│
├── vendor/
│   ├── ldacBT/             ← submodule: EHfive/ldacBT (Apache 2.0)
│   └── btstack/            ← 用户自拉，仓库不含 (见 §4.2)
│
├── third_party/
│   ├── imgui/              ← submodule: ocornut/imgui (MIT)
│   ├── implot/             ← submodule: epezent/implot (MIT)
│   └── nlohmann_json/      ← submodule (MIT)
│
├── core/                   ← 核心程序 (win-ldac-core.exe)
│   ├── CMakeLists.txt
│   └── src/
│       ├── main.cpp
│       ├── app/                  ← 状态机、配对管理、配置持久化
│       ├── audio/
│       │   ├── wasapi_loopback.{cpp,h}
│       │   ├── pcm_convert.{cpp,h}   ← f32↔s16/s24/s32, 重采样
│       │   └── ring_buffer.h
│       ├── ldac/
│       │   └── ldac_wrapper.{cpp,h}  ← libldac handle 生命周期 + ABR
│       ├── a2dp_ldac/
│       │   ├── a2dp_ldac_caps.{cpp,h}    ← vendor codec capability 字节
│       │   ├── a2dp_ldac_source.{cpp,h}  ← 移植自 AOSP 的胶水层
│       │   └── rtp_pack.{cpp,h}
│       ├── bt/
│       │   ├── btstack_run_loop.{cpp,h}
│       │   ├── pairing.{cpp,h}       ← link key 持久化 (JSON file)
│       │   └── device_db.cpp
│       ├── ipc/                   ← M7 才实现
│       │   ├── named_pipe_server.{cpp,h}
│       │   ├── protocol.h         ← JSON schema 常量
│       │   └── status_emitter.{cpp,h}
│       └── tools/
│           ├── ldac_encode_test.cpp  ← M1
│           ├── hci_scan_test.cpp     ← M2
│           └── sbc_source_test.cpp   ← M3
│
├── gui/                    ← M8 才开始（可选）
│   ├── CMakeLists.txt
│   └── src/
│       ├── main.cpp
│       ├── ipc/
│       │   └── named_pipe_client.{cpp,h}
│       ├── ui/
│       │   ├── status_window.{cpp,h}
│       │   ├── bitrate_chart.{cpp,h}
│       │   └── theme.cpp
│       └── platform/
│           └── win32_dx11.cpp
│
└── docs/
    ├── PROTOCOL_NOTES.md       ← AVDTP/A2DP 协议要点
    ├── PORTING_NOTES.md        ← AOSP 移植决策
    └── IPC_SPEC.md             ← §4.6 协议详细 schema
```

---

## 6. 构建与运行

### 一次性环境准备
- **Visual Studio 2022**（C++ Desktop workload + CMake）
- **Windows 11 SDK** ≥ 10.0.22621
- **CMake** ≥ 3.20
- **Git**
- **Zadig** ≥ 2.8（[zadig.akeo.ie](https://zadig.akeo.ie/)）
- 一只 **CSR8510** USB BT dongle
- **关闭 Memory Integrity**（见 §4.5）

### 编译命令
```bash
git clone <repo> ldac && cd ldac
git submodule update --init --recursive

# 用户须手动 clone BTstack（见 §4.2，本仓库不带）
git clone https://github.com/bluekitchen/btstack.git vendor/btstack

cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# 运行 core（先把 dongle 用 Zadig 换成 WinUSB 驱动）
build/core/Release/win-ldac-core.exe

# 可选：运行 GUI（M8 之后）
build/gui/Release/win-ldac-gui.exe
```

---

## 7. 里程碑与验收标准

### **核心路径（必须按顺序完成）**

#### M1：libldac 通编 ⏳
- [ ] 仓库骨架 + 顶层 CMake 跑通
- [ ] `vendor/ldacBT` 编出 `libldacBT_enc.lib`
- [ ] `core/src/tools/ldac_encode_test.cpp` 把 `test.wav`（48k/16bit/stereo）编成 LDAC bit stream，dump 到 `out.ldac`
- **验收**：输出码率落在 990 kbps ± 1%

#### M2：BTstack HCI 通路
- [ ] 用户手动 clone BTstack 到 `vendor/btstack/` 后能编出 `libbtstack.lib`
- [ ] `hci_scan_test.exe` 启动 HCI、做 Inquiry，stdout 打印附近 BT 设备
- **验收**：XM5 处于配对模式时能扫到，输出包含 `WH-1000XM5` 和 BD_ADDR

#### M3：SBC 链路打通（兜底 codec，验证整条链路）
- [ ] 实现 WASAPI loopback 抓音频
- [ ] 用 BTstack 自带 SBC 编码器走 A2DP source 连 XM5
- [ ] **能从 PC 播一首歌，XM5 出声**
- **验收**：Spotify 桌面版 → win-ldac-core → XM5，>30 秒无明显卡顿

#### M4：LDAC capability 注册 + 协商
- [ ] 写 `a2dp_ldac_caps.cpp`，构造 LDAC vendor codec capability 字节
- [ ] `avdtp_register_media_codec_category(..., NON_A2DP, ...)` 注册 LDAC SEP
- **验收**：HCI log 里 `AVDTP SET_CONFIGURATION` 包含 Vendor ID `0x12D` + Codec ID `0x00AA`，XM5 回 ACCEPT；Sony Headphones Connect app 显示 LDAC

#### M5：LDAC 真正出声 🎯 **核心目标达成点**
- [ ] M3 的 SBC 编码路径替换为 libldac
- [ ] RTP 封包按 AOSP 参考实现
- [ ] 处理 MTU、欠载、frame count 字段
- **验收**：XM5 显示 LDAC，正常播放音乐 5 分钟无中断

#### M6：ABR + 错误恢复（**核心硬化，到此 core 功能完整**）
- [ ] 接入 `ldac_ABR_Proc`，按 txq 长度自适应调整 EQMID
- [ ] 断连自动重连
- [ ] 配对密钥持久化（重启程序不用重新配对）
- **验收**：故意走出蓝牙范围再走回来，能自动恢复播放；重启电脑也无需重新配对

---

### **可选扩展（GUI / IPC / 打包）**

#### M7：IPC server（core 侧）
- [ ] 实现 named pipe server，按 §4.6 协议推送 status 事件（100 ms / 次）
- [ ] 用 `nc`、PowerShell `Get-Content \\.\pipe\win-ldac`、或自写小 client 验证
- **验收**：连接 pipe 后能收到 well-formed NDJSON 状态流；core 在无客户端连接时仍正常播放

#### M8：GUI MVP（只读展示）
- [ ] ImGui + DX11 backend 跑通空窗口
- [ ] 实现 named pipe client，订阅 core 状态
- [ ] 显示：设备名、连接状态、codec、EQMID、码率、采样率、TXQ
- **验收**：开 GUI 看到实时状态；关 GUI / kill GUI 不影响 core 播放

#### M9：GUI 双向控制 + 历史曲线
- [ ] ImPlot 画 60 秒码率曲线
- [ ] 设置面板：EQMID 模式切换、断开/重连按钮、扫描设备
- [ ] core 实现命令分发
- **验收**：从 GUI 切换 EQMID 立即生效；从 GUI 触发重连成功

#### M10：分发打包
- [ ] 顶层 README 双语化（中文优先 + 英文 build 步骤）
- [ ] 添加 LICENSE (Apache 2.0)
- [ ] BTstack license 警示页（防止用户误以为 BTstack 也是 Apache 2.0）
- [ ] 一键 build 脚本 `build.ps1`
- [ ] 测试朋友能照 README 在干净 Windows 上 build 出来
- **验收**：朋友 build 一次成功

---

## 8. 已知坑（提前知道能省一天）

1. **Memory Integrity 必须关**：见 §4.5。症状是 Zadig 替换驱动后设备管理器里设备带感叹号。
2. **WASAPI loopback 必然是 32-bit float**：Windows 混音引擎统一用 float32，要在送 libldac 前转 s16/s24。**别试图设置 `IAudioClient::Initialize` 的 format**，loopback 模式下设备格式由系统决定，强制设置拿到 `AUDCLNT_E_UNSUPPORTED_FORMAT`。
3. **BTstack run loop 单线程**：所有 BTstack 调用必须在 run loop 线程。WASAPI 和 libldac 在其它线程，跨线程喂数据用 `btstack_run_loop_execute_on_main_thread()`。
4. **AVDTP MEDIA PDU 第一个字节是帧数 + 标志**：不是简单 LDAC 帧拼接，要按规范包。照 AOSP `a2dp_vendor_ldac_encoder.cc` 移植。
5. **MTU 协商出来通常 ~672 或 1021**：一帧 LDAC 加 RTP 头超过 MTU 要分片（fragmentation flag）。别假设单帧总能塞下。
6. **LDAC 采样率不能任意**：只支持 44.1/48/88.2/96 kHz。WASAPI 给什么就用什么，**不要重采样**（除非系统是奇葩的 22.05k）。
7. **link key 存储**：BTstack 默认存内存，重启丢。要 hook `btstack_link_key_db_t` 接口落盘到 JSON。
8. **XM5 配对**：长按耳机右侧电源键 7 秒进入配对模式，蓝灯快闪。
9. **不要碰内置蓝牙**：Zadig 列设备时**仔细看 VID/PID**。误换内置蓝牙驱动很难恢复。
10. **Sony LDAC vendor ID = 0x012D，codec ID = 0x00AA，little-endian**：错了 XM5 直接拒连，没好错误码。
11. **不要把 BTstack 源码 commit 进我们仓库**：见 §4.2 路线 2 约定。`.gitignore` 已经把 `vendor/btstack/` 屏蔽。
12. **IPC 中文路径慎用**：Named pipe 名固定 ASCII，但日志输出经过 Windows console 时中文要 `SetConsoleOutputCP(CP_UTF8)`。
13. **Realtek 固件文件名不带 `.bin`**：从 [linux-firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/rtl_bt) 下载的文件叫 `rtl8761bu_fw.bin`，但 BTstack 的 `btstack_chipset_realtek.c` 内部表里写的是不带后缀的 `rtl8761bu_fw`。直接 `fopen()` 就会失败。我们的 `firmware/` 里已经做好了 rename。
14. **chipset setup 必须在 `hci_init()` 之后**：之前在前面调 `hci_set_chipset()` 会段错误（看似 BTstack 内部状态没初始化）。参考 BTstack 自己 `port/libusb/main.c` 的顺序：先 `hci_init`，再加 event handler，再 `hci_set_chipset` + `hci_enable_custom_pre_init`，最后 `hci_power_control(HCI_POWER_ON)`。
15. **TP-Link 给 Realtek dongle 用了自家品牌 USB ID**：UB4A 的 `VID:PID = 2357:0604`，**不在** BTstack 的 Realtek 表里。我们用"假报"：调 `btstack_chipset_realtek_set_product_id(0x8771)` 当作参考板 RTL8761BU，固件就对了。其它 OEM rebrand 的 RTL dongle 同理处理。
16. **BTstack 的 `port/windows-winusb` 没有原生 Realtek 集成**：libusb port 有，windows-winusb port 没有。我们的 `cmake/btstack.cmake` 把 `chipset/realtek/*.c` 加进编译，并由我们的代码（`hci_scan_test.c`、未来的 `core/src/app/`）显式 setup。

---

## 9. License 与合规（路线 2）

| 组件 | License | 在本项目中的处理 |
|---|---|---|
| **我们写的代码** | Apache 2.0 | LICENSE 文件放仓库根；每个源文件可加 SPDX-License-Identifier 头 |
| libldac | Apache 2.0 | submodule，可分发 |
| BTstack | "个人/非商业免费"，商业另购 | **不**进 submodule，**不**进二进制，用户自拉自 build |
| Dear ImGui | MIT | submodule，可分发 |
| ImPlot | MIT | submodule，可分发 |
| nlohmann/json | MIT | submodule，可分发 |
| AOSP `a2dp_vendor_ldac_encoder.cc` 移植 | Apache 2.0 | 移植代码保留 AOSP 版权声明 + Apache 2.0 header |
| Realtek BT firmware (`firmware/rtl8761bu_*`) | Realtek 二进制再分发许可（见 [`firmware/LICENCE.rtlwifi_firmware.txt`](firmware/LICENCE.rtlwifi_firmware.txt)） | ✅ **允许** bundle 进仓库分发，须保留版权声明（已写入 `firmware/README.md`） |

**分发策略明确说明**（也会写进 README 显眼位置）：
- ✅ 本仓库源代码可任意公开、fork、修改
- ✅ 个人朋友 clone 后自行 build 自用
- ❌ **不发布预编译二进制**（任何形式：release、CI artifact 都不行）
- ❌ **不为他人代 build 后传文件**
- ❌ **不收钱**——任何商用要先解决 BTstack 商业许可

**如果将来想真正能分发二进制**：升级到"路线 1"——自写蓝牙栈替换 BTstack。这是一项独立大工程，~6-8 周额外工作量。届时新建分支 `route-1-own-stack` 推进。

---

## 10. 设计决策日志

| 决策 | 选项 | 选定 | 理由 |
|---|---|---|---|
| 总体路线 | A. 内核驱动 / B. 用户态接管 USB / C. WSL2 / D. 付费软件 | **B** | A 要 EV 证书；C 已证不可行；D 不学技术 |
| 蓝牙栈 | BTstack / Zephyr / Bluedroid / 自写 | **BTstack** | Zephyr 在 Windows 用户态没有现成 port，Bluedroid 绑 Android；BTstack 唯一现成可用 |
| 分发模型 | 路线 1 自写栈 / **路线 2 用户自拉 BTstack** / 路线 3 仅自用 | **路线 2** | 用户朋友能自己编译，license 风险可控，工程量可控 |
| LDAC 库 | AOSP 原版 / EHfive 包装 | **EHfive/ldacBT** | 同一份 Sony 代码 CMake 化，Windows 能编 |
| Dongle 芯片 | CSR8510 / RTL8761B / 多家族都支持 | **多家族（首选 RTL8761B）** | CSR8510 停产，RTL8761B 系列（TP-Link UB4A/UB400v2/UB500、Orico、EDUP 等）市面主流；BTstack 已有 Realtek 驱动；固件 35 KB bundle 进仓库不影响分发 |
| 语言 | C / C++ / Rust | **C++17** | libldac/BTstack/ImGui 都好接；WASAPI 是 COM，C++ 最顺 |
| Codec 路径 | 直接 LDAC / 先 SBC 再 LDAC | **先 SBC（M3）再 LDAC（M5）** | SBC 是 BTstack 直接支持的兜底，先打通链路再换 codec |
| 架构 | 单进程 GUI / **双进程 core+gui** | **双进程** | GUI 崩了不影响音频；可无头运行；可扩展多前端；解耦开发节奏 |
| IPC | Named pipe / 本地 TCP / 共享内存 / COM | **Named Pipe** | Windows 原生、低延迟、零防火墙弹窗、足够 100 ms 频率 |
| GUI 框架 | Dear ImGui / Qt / WinUI 3 / WebView2 | **Dear ImGui** | MIT 全 permissive，单 exe <3MB，状态面板足够好看 |
| GUI 优先级 | 与 core 并行 / **core 完成后再做** | **后做** | 用户明确表示 GUI 次要，core LDAC 功能优先 |

---

## 11. 已验证的官方/可信参考资料

**协议与规范**
- [Bluetooth SIG: A2DP 1.4 Spec](https://www.bluetooth.com/specifications/specs/advanced-audio-distribution-profile-1-4/)
- [Bluetooth SIG: AVDTP 1.3 Spec](https://www.bluetooth.com/specifications/specs/audio-video-distribution-transport-protocol-1-3/)

**编码器**
- [AOSP libldac 上游](https://android.googlesource.com/platform/external/libldac)
- [EHfive/ldacBT CMake 包装](https://github.com/EHfive/ldacBT)
- [AOSP A2DP LDAC 编码器参考](https://android.googlesource.com/platform/packages/modules/Bluetooth/+/refs/heads/main/system/stack/a2dp/)

**蓝牙栈**
- [BTstack GitHub](https://github.com/bluekitchen/btstack)
- [BTstack profiles 文档](https://bluekitchen-gmbh.com/btstack/profiles/)
- [BTstack windows-winusb port](https://github.com/bluekitchen/btstack/tree/master/port/windows-winusb)

**Windows API**
- [MS Docs: Loopback Recording](https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording)
- [MS Docs: Application loopback sample](https://learn.microsoft.com/en-us/samples/microsoft/windows-classic-samples/applicationloopbackaudio-sample/)
- [MS Docs: IAudioClient::Initialize](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-initialize)
- [MS Docs: Named Pipes](https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipes)

**Linux 端参考实现**（仅读不抄）
- [PipeWire bluez5 module](https://gitlab.freedesktop.org/pipewire/pipewire/-/tree/master/spa/plugins/bluez5)
- [bluez-alsa](https://github.com/arkq/bluez-alsa)

**GUI**
- [Dear ImGui](https://github.com/ocornut/imgui)
- [ImPlot](https://github.com/epezent/implot)
- [nlohmann/json](https://github.com/nlohmann/json)

**工具**
- [Zadig USB driver installer](https://zadig.akeo.ie/)

---

## 12. 交接清单（每次会话结束前请勾选）

把当前会话做的事写进 `STATUS.md`。模板：

```markdown
# STATUS

最近更新：YYYY-MM-DD

## 当前里程碑
M? — 描述

## 本次会话做了什么
- ...

## 卡在哪
- ...

## 下次接手应该做什么（具体到第一个命令）
- ...

## 本次会话引入的新依赖 / 配置 / 决策
- ...
```

接手 AI 的开场动作：
1. 读 `docs/ARCHITECTURE.md`（本文件）
2. 读 `STATUS.md`
3. `git status` 看工作区
4. `git log --oneline -20`
5. 按 `STATUS.md` 的"下次接手应该做什么"开干

---

**文档结束。**
