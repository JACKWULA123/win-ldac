# STATUS

最近更新：2026-05-12

## 当前里程碑
**M2 完成 ✅**（包括 M2b 硬件验收），准备开 M3（实际从 PC 出声到 XM5）。

## 本次会话做了什么

### M2b 硬件验收 — 全部通过
- 用户：UB4A 已用 Zadig 换成 WinUSB 驱动，Memory Integrity 已关
- 运行 `hci_scan_test.exe`，输出包含：
  - Realtek 固件加载成功：`Received key id 0xef` + `FW/CONFIG total length is 30210`
  - HCI 起来：`Local BD_ADDR = 50:3D:D1:56:FF:0B`（这就是你 UB4A dongle 的蓝牙 MAC）
  - 找到耳机：`88:C9:E8:F7:D5:F3 = "WH-1000XM5"`
  - Verdict：`[ PASS ]  WH-1000XM5 discovered. M2 acceptance met.`
- 整个 Realtek 固件加载流程 + HCI + GAP Inquiry + Remote Name Request 在 Windows / WinUSB / TP-Link UB4A 上**已实测可用**。

### 顺手修复中文控制台编码
- 现象：源码 UTF-8 输出经 Windows 中文版控制台（codepage 936/GBK）解读，box-drawing 字符 (`─`) 和点 (`●`) 变成 "鈥?鈹€" 乱码
- 修复：`main()` 开头调 `SetConsoleOutputCP(CP_UTF8)` 一行搞定
- 同样的修复将来 core 主程序也要做（HANDOFF.md §8 #12 已记）

### 关键事实记录（M3+ 会用到）
- **本机 dongle BD_ADDR**：`50:3D:D1:56:FF:0B`（我们作为 A2DP source 的本端地址）
- **XM5 BD_ADDR**：`88:C9:E8:F7:D5:F3`（M3/M5 连接目标，可以先 hardcode 作为开发期省事手段，后面再做 GUI 选设备）
- **XM5 Class of Device**：`0x240404`（Audio/Video → Wearable Headset Device，符合预期）
- Realtek 固件加载延迟：从 power_on 到 HCI working 大约 1-2 秒，比 CSR 慢一点但可接受

## 卡在哪
无。M2 完整通过，准备开 M3。

## 下次接手应该做什么（M3 任务详细分解）

**目标**：第一次从 Windows PC 听到 XM5 的声音，**用 SBC**（不是 LDAC）。这是为了**先打通整条 A2DP source 链路**，把 codec 风险隔离开——M5 才换 LDAC。

参考样板：[`vendor/btstack/example/a2dp_source_demo.c`](vendor/btstack/example/a2dp_source_demo.c)
另外重要：[`vendor/btstack/src/classic/a2dp_source.h`](vendor/btstack/src/classic/a2dp_source.h)

### M3 子任务（建议拆 3 步走）

#### M3.1：复刻 a2dp_source_demo，用内置正弦波 → XM5 出声
**目的**：把 A2DP source profile + SBC encoder + pairing + 连接打通，**信号源用 BTstack 自带的正弦波 generator**（demo 里有现成的）。
- 新文件：`core/src/tools/a2dp_sine_test.c`（或类似名）
- 关键 API：
  - `a2dp_source_init()`
  - `a2dp_source_create_stream_endpoint(AVDTP_AUDIO, AVDTP_CODEC_SBC, ...)`
  - `gap_set_class_of_device(0x200404)` (A/V → loudspeaker)
  - `hci_set_link_key_db(btstack_link_key_db_tlv_get_instance(...))` 配对密钥落盘
  - `a2dp_source_establish_stream(target_addr, &local_seid)` 连接
- Hardcode target = `88:C9:E8:F7:D5:F3`（M2 抓到的 XM5）
- **验收**：XM5 戴上能听到 440Hz 正弦波，>30 秒不卡

#### M3.2：换成 WASAPI loopback 真实系统音频
**目的**：把信号源从内置正弦波换成 Windows 系统音频混音输出。
- 新文件：`core/src/audio/wasapi_loopback.{cpp,h}`
- 关键 API：
  - `IMMDeviceEnumerator::GetDefaultAudioEndpoint(eRender, ...)` 拿系统默认输出
  - `IAudioClient::Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, ...)`
  - `IAudioCaptureClient::GetBuffer/ReleaseBuffer`
  - 拿到的格式通常是 32-bit float 48 kHz stereo → 转 s16 给 SBC encoder
- Ring buffer 在 WASAPI 线程和 BTstack 线程之间过 PCM
- **验收**：放 Spotify / B 站，XM5 出对应的声音

#### M3.3：配对 + link key 持久化 + 自动重连
**目的**：重启程序不用每次 XM5 进入配对模式。
- 把 link key 存到 `linkkeys.json`（已在 `.gitignore`）
- 调 `btstack_link_key_db_tlv_get_instance()` 走 TLV 持久化
- 启动时若有已知 device 就直接 page (`gap_connect`)，不再 Inquiry
- **验收**：第一次配对；关程序；重开程序；XM5 不在配对模式但直接连上

### 如果要先简化 M3
做 M3.1 就够"M3 验收通过"。M3.2 和 M3.3 可以并入 M4/M5/M6 的工作再分摊。

### 在开 M3 之前的建议
- **提交一次 git commit**（用户授权后）：把 M1+M2 的成果固化
- M2b 跑通后 XM5 已经和我们 dongle 配过对了，**link key 留在 XM5 端**。M3.1 第一次连接如果失败，可以让 XM5 删除 win-ldac dongle 的配对记录重来

## 本次会话引入的新依赖 / 配置 / 决策
- 控制台 UTF-8 输出统一约定：所有 main() 加 `SetConsoleOutputCP(CP_UTF8)`
- M3 信号源开发策略：先内置正弦波（M3.1）跑通 A2DP，再换 WASAPI loopback（M3.2）
- 配对约定：第一次开发用 hardcode BD_ADDR，M6 之后做"扫描+选择"

## 仓库当前状态

```
ldac/
├── HANDOFF.md / STATUS.md / README.md
├── CMakeLists.txt + cmake/{btstack.cmake, win_compat/}
├── firmware/  (RTL8761BU fw + config + LICENCE + README)
├── core/
│   ├── CMakeLists.txt
│   └── src/
│       ├── main.cpp
│       └── tools/
│           ├── ldac_encode_test.cpp  (M1 ✅ PASS)
│           └── hci_scan_test.c        (M2 ✅ PASS, 含中文编码修复)
├── vendor/ldacBT/  (submodule)
├── vendor/btstack/  (用户拉)
└── build/
```
