# STATUS

最近更新：2026-05-12

## 当前里程碑
**M4 完成 ✅** — LDAC capability 注册 + 协商在硬件上跑通
**下一站**：M5 — 集成 libldac 编码器，真实推送 LDAC 音频流

## 本次会话做了什么

### M3.2 commit
- commit `afceb36`: WASAPI loopback → ring buffer → SBC → XM5
- 用户硬件实测：Spotify 通过 XM5 出声，无明显故障
- 记录设计权衡：XM5 不出现在 Windows 音频菜单中（loopback 架构的固有副作用，详见 commit message）

### M4: LDAC capability 协商
- 新文件：`core/src/tools/a2dp_ldac_test.c`
- 关键设计：**只注册 LDAC SEP，不注册 SBC**——绕开 BTstack 默认 SBC auto-pick 逻辑
- 流程：
  1. 启动 BTstack + Realtek 固件
  2. 连 XM5（hardcode BD_ADDR）
  3. 在 A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CAPABILITY 事件里检测 LDAC（vendor 0x012D + codec 0x00AA）
  4. 计算交集 → 选最高采样率 → 调 `a2dp_source_set_config_other` 暂存 SET_CONFIGURATION payload
  5. BTstack 在 CAPABILITIES_COMPLETE 后真正发 AVDTP_SET_CONFIGURATION
  6. XM5 ACCEPT → A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CONFIGURATION → 解析 vendor/codec/采样率/声道 → PASS

### 这一轮发现的关键坑（已写进 a2dp_ldac_test.c 注释）

1. **BTstack 的 SBC auto-pick 路径只对 SBC SEP 生效**——为 LDAC 必须显式调 `a2dp_source_set_config_other`
2. **`a2dp_source_set_config_other` 存指针不存字节**——传入 buffer 必须有静态存储期。我们第一次跑栈变量 → BTstack 后续从悬空指针读出垃圾 → XM5 沉默丢弃。修复：把 8 字节 cfg 挪到 `state_t`（static）。**这个坑在 BTstack 文档里没明说，是从源码里抠出来的**
3. **`set_config_other` 不立即发送 SET_CONFIGURATION**——只暂存 + 标记 `have_config = true`；BTstack 在 CAPABILITIES_COMPLETE 之后才真正构造并发出 AVDTP 包

### 验收结果
```
[OK] XM5 advertises LDAC: sr_bitmap=0x3C cm_bitmap=0x07
     intersection (us & xm5): sr=0x3C cm=0x01
     chosen: 96000 Hz, STEREO

[OK] AVDTP vendor codec configuration negotiated:
       vendor_id    = 0x0000012D  (Sony)
       codec_id     = 0x00AA      (LDAC)
       sample_rate  = 96000 Hz
       channel_mode = STEREO

[ PASS ]  M4 acceptance met — LDAC selected by XM5.
```

- **XM5 advertised 4 个采样率**（44.1/48/88.2/96）和 3 个声道模式（MONO/DUAL/STEREO）
- 我们的偏好算法（高采样率优先 + STEREO）选了 **96 kHz STEREO**——LDAC 最高规格
- XM5 ACCEPT 这个配置

### 重要的可重用数据
- XM5 LDAC capability：`sr_bitmap=0x3C, cm_bitmap=0x07`
- 协商出的 SET_CONFIGURATION：`2D 01 00 00 AA 00 04 01`（96 kHz STEREO）
- XM5 接受 96 kHz——M5 时可以直接走 LDAC HQ @ 96k（不过得看 Windows mixer 是否能给 96k，多数是 48k）

## 卡在哪
无。M4 干净通过。

## 下次接手应该做什么（M5 开始前的研究）

M5 是真正的 LDAC 编码器集成。预估工作量 2-3 天。

### M5 工作分解

1. **新建 `core/src/ldac/ldac_wrapper.{h,cpp}`**：libldac 句柄管理 + ABR
   - `ldacBT_get_handle` / `ldacBT_init_handle_encode(mtu, eqmid=HQ, cm, fmt, sr)` / `ldacBT_encode` / `ldacBT_free_handle`
   - ABR：`ldac_ABR_get_handle` / `ldac_ABR_Init` / `ldac_ABR_Proc`

2. **新建 `core/src/a2dp_ldac/`**：移植 AOSP `a2dp_vendor_ldac_encoder.cc`
   - `a2dp_ldac_caps.{cpp,h}` — vendor codec capability/config 字节助手（M4 已有逻辑可提取）
   - `a2dp_ldac_source.{cpp,h}` — 主驱动：PCM → libldac → AVDTP MEDIA PDU
   - `rtp_pack.{cpp,h}` — RTP 头 + LDAC frame 打包

3. **AVDTP MEDIA PDU 格式**（关键，照 AOSP 抄）：
   - 1 byte 帧数 / fragmentation flag
   - N 个 LDAC frame 数据
   - 受 MTU 约束（典型 679 bytes），超过要分片

4. **写 `core/src/tools/a2dp_ldac_play_test.c`**（M5 工具）：
   - 类似 a2dp_wasapi_test 但 codec 用 LDAC
   - 关键差异：WASAPI 采样率必须等于协商的 LDAC 采样率（96k 或 48k）
   - 如果用户 Windows mixer 是 48k 但 XM5 协商出 96k，要降级或重采样
   - **建议**：M5 第一版强制协商 48 kHz LDAC（不要 96k），匹配 Windows 主流采样率，省得加重采样器

5. **采样率协商策略调整**：M4 的"挑最高" 算法在 M5 不合适——会跟 WASAPI 不对齐。M5 改为"挑与 WASAPI 一致的最低支持率"

### 验收标准
- 跑 `a2dp_ldac_play_test.exe`
- 同时播 Spotify
- **XM5 听到 Spotify**
- Sony Headphones Connect APP 显示 codec = **LDAC**（不是 SBC）
- 持续 5 分钟无明显卡顿

## 本次会话引入的新依赖 / 配置 / 决策

- LDAC capability/config 字节布局已固化在代码中：
  ```
  byte 0-3: vendor ID 0x012D (little-endian: 2D 01 00 00)
  byte 4-5: codec ID 0x00AA (little-endian: AA 00)
  byte 6:   sample rate bitmap (capability) / single bit (config)
  byte 7:   channel mode bitmap (capability) / single bit (config)
  ```
- BTstack 调用约定：`set_config_other` 传入指针的字节必须有静态存储期
- M5 决策：先做 48 kHz LDAC（最常见，跟 Windows mixer 自然匹配），96 kHz 留给 M6+ 加重采样器后

## 仓库当前状态

```
ldac/
├── 已 commit:
│   ├── 5be03b4 (M1+M2)
│   ├── b978a99 (M3.1)
│   └── afceb36 (M3.2)
├── 未 commit:
│   ├── STATUS.md
│   ├── core/CMakeLists.txt
│   └── core/src/tools/a2dp_ldac_test.c
└── 构建产物：
    └── build/core/Release/a2dp_ldac_test.exe (~275 KB)
```

## 用户那边的 hung 进程

`a2dp_sine_test.exe` PID 3780 还在跑（M3.1 退出 bug 留下的，已修复但旧 exe 没被替换）。不影响 M4 测试，但建议手动 Ctrl+C 或关掉那个 PowerShell 窗口清理。
