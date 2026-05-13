# STATUS

最近更新：2026-05-12

## 当前里程碑
**M3.1 软件就绪 ⏳**，等用户做硬件测试验证 XM5 出声。

## 本次会话做了什么

### Commit M1 + M2
- 第一次正式 commit：`5be03b4 — M1 + M2: libldac integration + BTstack HCI with Realtek firmware`
- 含 18 个文件（不含 BTstack，按约定用户自拉）

### M3.1：A2DP source + SBC + sine wave 工具
- 新文件：`core/src/tools/a2dp_sine_test.c`（~350 行 C）
- 结构参考 `vendor/btstack/example/a2dp_source_demo.c`，剥掉 AVRCP / modplayer / stdin 命令交互，只保留"连上耳机 → 协商 SBC → 持续推 sine wave 60 秒"
- 关键设计：
  - Hardcode 连 `88:C9:E8:F7:D5:F3`（M2 抓到的 XM5）
  - SBC capability 字节：44.1 / 48k stereo，block/subbands/alloc 全 0xFF（让 XM5 自选）
  - PreferredSamplingFrequency = 44100（SBC 在 44.1 上音质最稳）
  - SSP "Just Works"：`IO_CAPABILITY_NO_INPUT_NO_OUTPUT` + `auto_accept=true`，配对无需人工
  - Legacy PIN fallback：如果 XM5 突然走 legacy 配对，回 "0000"
  - 10 ms 定时器驱动 SBC 编码 + 包累积 + can_send_now 触发推送
  - 60 秒后自动 power off + 退出（避免无限循环）
- 复用 M2 已实现的 Realtek 固件加载逻辑（同一个 firmware/ 自动定位函数）
- 构建产物：`build/core/Release/a2dp_sine_test.exe` (~270 KB)，构建干净无 warning/error

### 没做什么（M3.2 / M3.3 留给后续）
- ❌ WASAPI loopback 抓系统音频（M3.2）
- ❌ link key 持久化（M3.3）—— 当前每次启动都得 XM5 重新接受配对
- ❌ AVRCP（可推到 v2）

## 卡在哪
**等用户在硬件上跑测试**。需要：
- UB4A dongle 插着 + WinUSB 驱动（M2 已做）
- XM5：**不在配对模式**也可以试（已配对过一次），如果连不上则进入配对模式再试

## 下次接手应该做什么（具体到第一个命令）

### 用户做硬件测试

XM5 戴上头上，pc 端跑：

```
D:\claude\ldac\build\core\Release\a2dp_sine_test.exe
```

### 预期输出（成功路径）

```
a2dp_sine_test - M3.1 acceptance
================================
Target: XM5 at 88:C9:E8:F7:D5:F3
[..] Realtek firmware folder: ...
Realtek: Using firmware ...
Realtek: Received key id 0xef
Realtek: FW/CONFIG total length is 30210, max patch size id 40960
[OK] HCI up. Local 50:3D:D1:56:FF:0B. Connecting to XM5 88:C9:E8:F7:D5:F3 ...
[..] SSP user confirmation from 88:C9:E8:F7:D5:F3 — auto-accept
[OK] A2DP signaling established with 88:C9:E8:F7:D5:F3 (cid 0x0001)
[OK] SBC negotiated: 44100 Hz, 2 ch, blocks=16, subbands=8, bitpool=53, alloc=1, ch_mode=3
[OK] A2DP stream open; starting playback...
[OK] Stream started — 441 Hz tone should be audible on XM5 for 60 seconds.

(60 秒持续不动，耳机里听到稳定的 441 Hz "嘟──" 声)

[ PASS ]  M3.1 test duration elapsed. Shutting down.
[..] Stream released
[..] Signaling connection released
```

### 验收
- **耳机里能听到稳定的纯音 60 秒**：M3.1 PASS
- 短促播放后就断：M3.1 部分 PASS，需要调试 timing/packet 推送
- 完全没声音但 stream started 出来了：可能是 codec 配置问题或定时器问题
- 在 SSP 阶段挂住：可能 XM5 还记着旧配对失败，**用 Sony 耳机 APP 或在 XM5 上长按按钮删除我们 dongle 的配对**重试
- 在 establish_stream 之前就报错：XM5 没准备好连接（戴上头上、不在配对模式但能被 page）

### 如果失败常见诊断

| 输出特征 | 可能原因 | 建议 |
|---|---|---|
| `a2dp_source_establish_stream rc=0x...` | XM5 不可达 | XM5 戴上头上、断开其它设备的连接 |
| 连了上但 SBC negotiated 后没下文 | XM5 拒绝了 SBC config（少见） | 抓 hci log，看 AVDTP 流程 |
| Stream started 但耳机没声 | 定时器 / can_send_now 没触发 | 加 printf 看每包发送时机 |
| 卡在 SSP user confirmation | 配对失败 | XM5 端清理 dongle 配对记录 |
| 听到声但 5-10 秒后断 | XM5 没看到媒体包流，超时关连接 | timing 问题，提高定时器频率或排查 sbc_storage 逻辑 |

### M3.1 通过 → 决定下一步

- **M3.2**（WASAPI loopback 取代 sine）—— 让 Spotify 真能出声，这才是用户最直观的"它工作了"
- 或先 commit M3.1 再去 M5（LDAC 协商和真出 LDAC 声音）
- 或者把 M3.3（link key 持久化）插进去

推荐顺序：**M3.1 PASS → commit → M3.2（WASAPI）→ commit → 直接跳到 M4+M5（LDAC）**。M3.3 link key 持久化可以放到 M6"硬化"阶段。

## 本次会话引入的新依赖 / 配置 / 决策

- A2DP source 角色 + bluedroid SBC encoder 已通过 `cmake/btstack.cmake` 集成（M2 时就编进了 `btstack.lib`，本轮第一次实际链接使用）
- 配对策略：SSP Just Works + 自动接受，避免人工交互；legacy PIN 0000 兜底
- 测试期间 hardcode BD_ADDR；正式 core 程序会做扫描+设备选择（M6+）
- 决策：M3.1 后立即开 M3.2（WASAPI），不在 M3.1 上做 link key 持久化

## 仓库当前状态

```
ldac/
├── 已 commit: 5be03b4 (M1+M2)
├── 未 commit:
│   ├── STATUS.md (本文件，描述 M3.1)
│   ├── core/CMakeLists.txt (加 a2dp_sine_test target)
│   ├── core/src/tools/a2dp_sine_test.c (M3.1 新文件)
│   └── core/src/tools/hci_scan_test.c (M2 时的 UTF-8 修复)
└── 构建产物：
    └── build/core/Release/a2dp_sine_test.exe (~270 KB)
```
