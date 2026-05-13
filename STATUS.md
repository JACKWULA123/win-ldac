# STATUS

最近更新：2026-05-13

## 当前里程碑
**M5 完成 ✅** — LDAC 真正出声，硬件实测通过
**下一站**：M6 — ABR 自适应码率 + 断连重连 + 配对密钥持久化

## 本次会话做了什么

### M5: libldac 编码器集成 + AVDTP MEDIA PDU 推送

新建模块（全部 C，全部走 extern "C" 接口）：

1. **`core/src/ldac/ldac_wrapper.{h,c}`** — libldac handle 生命周期 + 通道/采样率位映射
2. **`core/src/a2dp_ldac/a2dp_ldac_caps.{h,c}`** — LDAC vendor codec capability/configuration 字节构造与解析（从 M4 抽出复用）
3. **`core/src/a2dp_ldac/a2dp_ldac_source.{h,c}`** — 流式驱动：PCM → libldac → AVDTP MEDIA PDU
4. **`core/src/tools/a2dp_ldac_play_test.c`** — M5 验收 exe

### 关键设计决策

1. **采样率匹配 WASAPI**（不是 M4 的"挑最高"）
   - `a2dp_ldac_pick_sample_rate_for_hz()` 优先选 WASAPI 当前的 mixer rate
   - 实测 WASAPI 48 kHz + XM5 支持 48 kHz → 协商出 48 kHz，避免重采样
   - 若 WASAPI 不被 XM5 支持，落回 highest-common 并大声警告（音调会错）

2. **每个 AVDTP 包 1 个 LDAC 帧**
   - 简单，无碎片
   - HQ@48k 帧 ~330 字节，总能进 MTU ≥679
   - 5 位 frame count 字段最多支持 31 帧/包，留给以后批量化优化
   - 帧间无 fragmentation flags，所有 F/S/L 位为 0

3. **链式 `can_send_now` 是关键** ⭐
   - HQ@48k 需要 ~375 包/秒（每 2.67 ms 一包）
   - 5 ms 音频定时器只够发 1-2 包/tick，远低于需求
   - 解决：`on_can_send_now()` 发完一包立即尝试构造下一包并重新请求 `request_can_send_now`
   - 时间靠 BTstack 流控自然节流；定时器只负责往 `samples_owed` 中累加 PCM 配额

4. **5 ms 音频定时器**（SBC 测试用 10 ms） — 给 LDAC 高码率更多调度余量
5. **EQMID = HQ 固定** — ABR 留给 M6
6. **STEREO only** — Mono/Dual 留给以后（M6+）

### 采样率/帧大小细节

LDAC 在 ldacBT.h 描述的内部 frame 大小：
| Sample rate | Frame sample (per channel) |
|---|---|
| 44.1 / 48 kHz | 128 samples |
| 88.2 / 96 kHz | 256 samples |

但**输入到 `ldacBT_encode` 的 PCM 总是 128 sample/channel**。所以：
- 44.1/48 kHz：1 个 encode call = 1 个 LDAC transport frame
- 88.2/96 kHz：需要 2 次 encode call 才出 1 个 frame

RTP timestamp 累加 `frame_count * samples_per_ldac_frame`（128 or 256）。我们在 `a2dp_ldac_source.c::samples_per_frame_for_rate()` 编码这个规则。

### 硬件实测结果

第一次测试（测试前没播音乐）：
```
Underrun samples: 2129472   ← WASAPI 空转期的零填充
Current LDAC bitrate: 990 kbps
```

第二次测试（测试前先开 Spotify 播放）：
```
Underrun samples: 0
```

意味着：
- ✅ 编码 + 打包 + 发送 pipeline 完全跟上 realtime
- ✅ 5 ms 定时器 + 链式 can_send 节奏正确
- ✅ 用户**听到 LDAC 音频**，Sony Headphones Connect 显示 codec = LDAC

### 这一轮学到的（写进注释和文档）

1. **WASAPI 空转 ≠ 真实 underrun**：当没 app 播音频时，WASAPI 不投递包；ring 空；我们零填充。这是预期行为不是 bug。诊断 underrun 时**必须先确认测试期间真的在播音乐**。
2. **链式 can_send_now 是 LDAC 必备**：SBC 那种"timer fires + send 1 packet"的模式对 LDAC 不够快。任何高码率 codec（包括以后的 aptX HD）都得用这个模式。
3. **`a2dp_max_media_payload_size` 返回的是 BTstack 给我们的 payload 配额**，已经扣掉 RTP/AVDTP header。我们再扣 1 字节 LDAC media header，剩下给 libldac 作为 MTU。

## 卡在哪
无。

## 下次接手应该做什么（M6 工作分解）

M6 是核心硬化阶段。包含三件相对独立的事：

### 1. ABR 自适应码率
- 接入 `vendor/ldacBT/libldac/abr/`（`ldacBT_abr.h`）
- `ldac_ABR_get_handle` / `ldac_ABR_Init(interval_ms=20)` / `ldac_ABR_Proc(handle, abr_handle, txq_depth, enable=1)`
- **难点**：BTstack 没有公开 API 拿 A2DP TX queue 深度
  - 选项 A：在 BTstack 内部 hook（碰内部状态，不可移植）
  - 选项 B：自己维护一个发送窗口计数（已请求 can_send_now 但还没真发的包数 + 已发但还没 ack 的包数）
  - 推荐 B，简单且与 BTstack 解耦
- ABR 的输入是 packet 数，不是字节数；我们已经知道每秒 ~375 包（HQ）
- 测试方法：故意走远点，看 EQMID 是否会自动从 HQ 降到 SQ/MQ；走回来看是否回升

### 2. 断连自动重连
- 监听 `HCI_EVENT_DISCONNECTION_COMPLETE` 或 `A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED`
- 重连策略：指数退避，3 秒、9 秒、27 秒、然后周期 30 秒
- 重连成功后自动恢复流（state machine：disconnected → connecting → negotiating → streaming）
- 当前 M5 测试工具是 fire-and-forget，没有这个状态机。M6 需要构建状态机模块

### 3. Link key 持久化
- BTstack 默认链路密钥在内存，重启丢
- 需要 hook `btstack_link_key_db_t` 接口
- 实现：把每个 (bd_addr, link_key, link_key_type) JSON 序列化到 `%LOCALAPPDATA%\win-ldac\keys.json` 或仓库相对路径
- BTstack 提供 hook：`btstack_link_key_db_memory.c` 的 API，参考 `port/posix/btstack_link_key_db_fs.c`

### M6 验收
- 故意走出蓝牙范围（带笔记本走远点 5-10 米隔几堵墙），ABR 应降码率；走回来应升回
- Kill BT dongle 模拟断连，3 秒内 win-ldac-core 应自动重连
- 重启电脑，不用重新在 XM5 上按配对键，直接连上

### M6 工作量预估
- ABR：1 天
- 自动重连：1-2 天（需要做 state machine）
- Link key 持久化：半天
- 总：2.5-3 天

### M6 之前的小事
- 把 a2dp_ldac_play_test 的核心抽到 `core/src/app/` 作为 `win-ldac-core.exe` 的真实入口（替换 main.cpp 占位）
- 添加 CLI 参数（--bd-addr, --eqmid, --duration）

## 本次会话引入的新依赖 / 配置 / 决策

- M5 采样率策略**固化为"匹配 WASAPI"**（不是 M4 的"挑最高"）
- 5 ms 音频定时器 + 链式 can_send_now 是支撑 990 kbps LDAC 的关键
- 1 个 LDAC 帧/AVDTP 包是 M5 v1 的简化；以后批量化是可选优化
- AVDTP MEDIA PDU 第一字节布局已固化在代码（`a2dp_ldac_source.c::send_current_packet`）：
  ```
  bit 7 = F (fragmented),  bit 6 = S (start),  bit 5 = L (last)
  bits 4..0 = frame count (max 31)
  ```

## 仓库当前状态

```
ldac/
├── 已 commit:
│   ├── 5be03b4 (M1+M2)
│   ├── b978a99 (M3.1)
│   ├── afceb36 (M3.2)
│   └── ece1d26 (M4)
├── 未 commit (M5)：
│   ├── core/CMakeLists.txt（增加 M5 targets）
│   ├── core/src/ldac/ldac_wrapper.{h,c}
│   ├── core/src/a2dp_ldac/a2dp_ldac_caps.{h,c}
│   ├── core/src/a2dp_ldac/a2dp_ldac_source.{h,c}
│   └── core/src/tools/a2dp_ldac_play_test.c
└── 构建产物（worktree）：
    └── build/core/Release/a2dp_ldac_play_test.exe (~285 KB)
```

## Worktree 注意

本次工作在 git worktree `D:\claude\ldac\.claude\worktrees\recursing-cray-c9d142\`（分支 `claude/recursing-cray-c9d142`）。
- vendor/ldacBT 和 vendor/btstack 用 `mklink /J` 链接到主仓库 `D:\claude\ldac\vendor\` 的对应目录（worktree 一开始没有 submodule 内容，clone 又有网络问题）
- M5 commit 进 worktree 分支后需 merge 回 main
