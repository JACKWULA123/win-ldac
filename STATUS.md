# STATUS

最近更新：2026-05-12

## 当前里程碑
**M3.1 完成 ✅** (commit `b978a99`)
**M3.2 软件就绪 ⏳** — 等你跑硬件测试听 Spotify

## 本次会话做了什么

### M3.1 commit
- commit `b978a99`：a2dp_sine_test + exit 修复（HCI_STATE_OFF → exit(0)）
- 你之前那次 60 秒 sine 测试硬件验收已通过

### M3.2：WASAPI loopback 接入
新增文件：
- `core/src/audio/wasapi_loopback.h` — C 可调用 API（init / start / read / available / stop）
- `core/src/audio/wasapi_loopback.cpp` — COM 实现，独立 capture 线程，mutex 保护的 ring buffer
- `core/src/tools/a2dp_wasapi_test.c` — M3.2 验收工具（复刻 a2dp_sine_test 但音频源换成 WASAPI）

**关键设计要点**：
1. **采样率对齐**：先启动 WASAPI 拿系统 mixer 的采样率（44.1 或 48 kHz），把这个值作为 A2DP `preferred_sampling_frequency` 推给 XM5 协商。这样不需要重采样器。
2. **拒绝高采样率**：如果系统 mixer 是 88.2/96/192 kHz，wasapi_loopback_init 返回 -2 并提示用户去 Windows 声音设置里改成 44.1k 或 48k。SBC/LDAC 都不支持那些率。
3. **格式探测**：接受 IEEE float (32-bit) 和 s16 两种采样格式，其它（s24/s32-int）当前不支持。如果遇到 wFormatTag = WAVE_FORMAT_EXTENSIBLE，正确解析 SubFormat。
4. **AUDCLNT_BUFFERFLAGS_SILENT 处理**：Windows 没人播放时 WASAPI 仍然按 10 ms 节奏发空包，flag 里带 SILENT。我们要把这些当成静音帧 push 进 ring buffer，**保持时间轴稳定**；否则 SBC 编码端会饥饿。
5. **欠载兜底**：audio_timer 从 ring buffer 读，如果不够就补零并累加 `underrun_total_samples` 计数器，运行结束打印。健康状态下应该是 0。
6. **线程模型**：单 producer（WASAPI capture 线程）+ 单 consumer（BTstack run loop），mutex ring buffer 完全够用（48k stereo s16 ≈ 192 KB/s，mutex 开销 < 1 μs）。

### 沙箱验证
- 构建干净（只一个 WIN32_LEAN_AND_MEAN 重复定义 warning，无害）
- 沙箱里跑 `a2dp_wasapi_test.exe` 报 `GetDefaultAudioEndpoint failed: 0x80070490` 退出码 2 ——这是预期的：沙箱进程没有交互式 audio session 权限。**正常用户 PowerShell 跑应该能成功 init**。

## 卡在哪
**用户的 hung sine_test (PID 3780) 仍占用 `a2dp_sine_test.exe` 文件锁**，我这边没 admin 权限 kill。**不影响 M3.2 测试**（不同 exe），但下次想重编 M3.1 之前你得手动 Ctrl+C 或关那个 PowerShell 窗口。

## 下次接手应该做什么（M3.2 硬件验收）

### 前置（如果还没做）
1. 确保 dongle 仍是 WinUSB 驱动（M2 已做过应该还在）
2. **检查 Windows 声音设置的采样率**：
   - 系统托盘右键音量图标 → 声音设置 → 设备属性 → 高级
   - 看默认格式：必须是 **44100 Hz** 或 **48000 Hz**（位深 16 或 24 都行）
   - 96000/192000 Hz 我们当前版本不支持，会拒绝运行并提示

### 跑测试
```
D:\claude\ldac\build\core\Release\a2dp_wasapi_test.exe
```

测试程序会：
1. 启动 WASAPI loopback，打印 mixer 采样率和声道数
2. 启动 BTstack，加载 Realtek 固件，连 XM5
3. A2DP 协商完毕后开始抓系统音频 → SBC 编码 → 推到 XM5
4. **此时你打开 Spotify / B 站 / 浏览器播放音乐，应该从 XM5 听到声音**
5. 跑 120 秒后自动停止；想提前退出按 Ctrl+C

### 预期成功输出
```
a2dp_wasapi_test - M3.2 acceptance
==================================
[OK] WASAPI loopback: 48000 Hz, 2 channels (system mixer format)
[..] Realtek firmware folder: D:\claude\ldac\firmware
Realtek: ...
[OK] HCI up. Local 50:3D:D1:56:FF:0B. Connecting to XM5 88:C9:E8:F7:D5:F3 ...
[..] SSP user confirmation from 88:C9:E8:F7:D5:F3 — auto-accept
[OK] A2DP signaling established with 88:C9:E8:F7:D5:F3 (cid 0x...)
[OK] SBC negotiated: 48000 Hz, blocks=16, subbands=8, bitpool=53
[OK] A2DP stream open; starting playback...
[OK] Stream started. Play something on Windows now — Spotify, B 站, anything.
    Test will run for 120 s; press Ctrl+C to stop early.

(此时打开 Spotify 播一首歌，XM5 应该出声)

[ PASS ]  M3.2 test duration elapsed. Total WASAPI underrun samples: 0
HCI off. Bye.
```

### 验收
- ✅ **PASS**：能从 XM5 听到 Spotify / 浏览器视频 / 任何 Windows 系统声音
- ✅ underrun_total_samples 越接近 0 越好（少量是正常的 timing jitter，几百几千以下都 OK）
- ❌ 完全没声：检查 Windows 是不是真在播音乐、是不是默认输出设备（Spotify 可能输出到了别的设备）
- ❌ 听到但卡顿严重：underrun 数会很大，可能是 SBC 编码不够快或线程调度问题

### 如果失败常见诊断

| 现象 | 原因 | 修法 |
|---|---|---|
| `wasapi_loopback_init returned -2` | 系统采样率不是 44.1/48k | Windows 声音设置 → 设备属性 → 高级，改 16-bit 48000 Hz |
| `wasapi_loopback_init returned -3` | 格式不是 float 或 s16 | 同上，确保位深 16 或 24（24-bit 内部其实是 float） |
| WASAPI OK 但 A2DP 阶段失败 | 同 M3.1 失败模式 | 检查 XM5、检查配对 |
| 听到声但音调不对（变调） | WASAPI rate ≠ SBC rate（极少见，因为我们传递了 preferred rate） | 检查 stdout 里 `[WARN]` 行 |
| underrun 巨大（数十万） | ring buffer 没正常生产或消费 | 排查 wasapi 线程是否真跑起来 |

### M3.2 通过后

下一站：**M4 + M5**——LDAC capability 协商 + 实际推 LDAC 音频（不再是 SBC）。这是真正的项目目标。

预估工作：
- M4：编 LDAC vendor codec capability 字节，注册非 SBC 的 stream endpoint，让 XM5 协商时选 LDAC 而不是 SBC（~1 天）
- M5：移植 AOSP `a2dp_vendor_ldac_encoder.cc` 的 RTP 封包逻辑，替换 SBC 编码路径（~2-3 天）
- 验收：Sony Headphones Connect APP 里查看 codec 显示 LDAC，正常播放音乐 5 分钟无中断

## 本次会话引入的新依赖 / 配置 / 决策

- 新增 `core/src/audio/` 目录（WASAPI 模块）
- WASAPI capture 线程 + mutex ring buffer 架构定型
- 系统采样率约束：必须 44.1k 或 48k（高码率支持留给 M6+ 加 resampler）
- M3.2 不做 link key 持久化（M3.3 推到 M6）

## 仓库当前状态

```
ldac/
├── 已 commit:
│   5be03b4 (M1+M2)
│   b978a99 (M3.1)
├── 未 commit:
│   ├── STATUS.md
│   ├── core/CMakeLists.txt
│   ├── core/src/audio/wasapi_loopback.{h,cpp}
│   └── core/src/tools/a2dp_wasapi_test.c
└── 构建产物：
    ├── build/core/Release/a2dp_wasapi_test.exe (~275 KB)
    └── build/core/Release/wasapi_loopback.lib
```
