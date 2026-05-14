# STATUS

最近更新：2026-05-14

## 当前里程碑
**M7 Phase A 完成 ✅** — 后端重构 + 引擎库 + 所有非 GUI 改进
**下一步**：M7 Phase B — Dear ImGui + DirectX 11 GUI（在新会话开展）

## 已完成的里程碑

| 里程碑 | 完成内容 |
|---|---|
| **M1** | libldac 通编（990 kbps HQ 合成测试） |
| **M2** | BTstack HCI + Realtek RTL8761BU 固件加载 |
| **M3.1** | A2DP source SBC + 正弦波 → XM5 |
| **M3.2** | WASAPI loopback → SBC → XM5（系统音频流） |
| **M4** | LDAC capability 协商（Sony vendor 0x12D codec 0x00AA） |
| **M5** | LDAC 真实出声（HQ 48k 990 kbps，链式 CAN_SEND_NOW） |
| **M6.1** | 链路密钥持久化（重启免重新配对） |
| **M6.2** | 断连自动重连（XM5 关机/超距 → 5s 重试） |
| **M7 Phase A** | F32 PCM 端到端、ABR、Effective kbps 测量、引擎库、静音自动挂起、Windows 格式变更动态重建 |

## M7 Phase A 已完成（本会话）

提交链：
- `734ad5e` — M7 phase A: end-to-end float32 PCM, ABR, engine library
- `f790b33` — M7 phase A polish: silence pause, WASAPI rebuild, bit-depth fix

完成项：
- ✅ **F32 PCM 端到端**：WASAPI float32 → ldac_wrapper (LDACBT_SMPL_FMT_F32) → 编码。24-bit Hi-Res 源不再被 s16 截断
- ✅ **ABR (自适应码率)**：`ldac_wrapper_abr_*` 包装 libldac/abr/，TX 队列深度代理由 outstanding_packets 计算
- ✅ **Effective kbps 测量**：1 秒滑窗，反映实际链路带宽，曲线偏离 nominal 反映 RF 状况
- ✅ **设备位深读取**：`PKEY_AudioEngine_DeviceFormat` + `WAVEFORMATEXTENSIBLE.wValidBitsPerSample`，正确显示 16/24/32
- ✅ **WASAPI device-invalidated 处理**：Windows 改采样率/位深 → 自动重建 WASAPI → 重连 → 新协商
- ✅ **cid 0x0002 corner case 修复**：跨重连保留 negotiated_sr_hz/cm_bit
- ✅ **静音自动挂起**：连续 2 秒静音 → AVDTP SUSPEND → 第一个非零 PCM → AVDTP START，XM5 进入低功耗
- ✅ **core_engine 库**：`engine_init/run/shutdown` + `engine_get_status_snapshot` + `engine_post_*` 跨线程命令。所有 BTstack/WASAPI/LDAC 装配封装在内
- ✅ **`engine_persistent_test`** CLI 工具：~80 行，演示 engine 使用，含 `--adaptive` / `--fixed` 模式开关

硬件实测确认所有功能正常。

## 下一步：M7 Phase B — GUI

### 任务列表

1. **依赖准备**（用户操作）
   - 主仓库 clone 这两个 submodule 到 `third_party/`：
     ```
     git clone https://github.com/ocornut/imgui.git third_party/imgui
     git clone https://github.com/epezent/implot.git third_party/implot
     ```
   - 新 worktree 里建 junction（与 vendor/ 类似）：
     ```
     cmd /c "mklink /J third_party D:\claude\ldac\third_party"
     ```

2. **GUI 框架集成**（CMake）
   - 新建 `gui/CMakeLists.txt`
   - 链接 ImGui (core + DX11 backend + Win32 backend) 静态编进 exe
   - 链接 ImPlot
   - Win32 + DirectX 11 设备初始化
   - 主消息循环 + 关闭逻辑

3. **窗口布局**（来自主对话 mockup）
   ```
   ┌────────────────────────────────────────┐
   │  win-ldac                  [_][□][×]   │
   ├────────────────────────────────────────┤
   │  [用户提供的配图位置]                  │
   │                                        │
   │  🎧 WH-1000XM5         ● Connected     │
   │  88:C9:E8:F7:D5:F3                     │
   │  ──────────────────────────────────    │
   │  Codec        LDAC                     │
   │  Sample rate  48000 Hz · 24-bit        │
   │  ──────────────────────────────────    │
   │  Bitrate              ~975 kbps        │
   │  [ImPlot 折线图，60s 滑窗，Y 轴 0-990] │
   │                                        │
   │  Mode  ⦿ Fixed     ○ Adaptive          │
   │                                        │
   │  [Disconnect]  [Settings...]           │
   └────────────────────────────────────────┘
   ```
   **用户准备了一张配图**，会在 GUI 主要工作完成后插入到主界面合适位置。具体位置/大小由那张图决定。

4. **数据流**
   - GUI 主线程跑 ImGui frame loop（~60 Hz）
   - 引擎跑在另一线程（BTstack run loop）
   - 每帧：`engine_get_status_snapshot()` 读快照（线程安全，CRITICAL_SECTION）
   - 用户操作（mode 切换、disconnect）：`engine_post_*` 异步派发到 BT 线程

5. **折线图**
   - ImPlot::PlotLine
   - 数据：环形缓冲，每 100 ms 采样一次 effective_kbps
   - 60 秒滑窗 = 600 个数据点
   - Y 轴固定 0..990，画 330/660/990 参考线（虚线）
   - X 轴：相对秒（last 60s）

6. **GUI 状态映射**
   - "Connected/Disconnected/Reconnecting" 文字 ← `engine_status_t.state`
   - 采样率 + 位深 ← `sample_rate_hz` + `wasapi_bit_depth`
   - 实时码率 ← `effective_kbps`（大数字）
   - EQMID 标签：HQ/SQ/MQ ← `eqmid`（0/1/2）
   - 模式 radio button ← `bitrate_mode` ↔ `engine_post_set_bitrate_mode`
   - "Idle (paused)" 状态：检查 `a2dp_ldac_source_is_idle_paused()`（暂不在 snapshot 中暴露，可加）

7. **WinMain 入口**
   - 替换 `core/src/main.cpp` 占位为真正的 WinMain
   - `engine_init` + 启动引擎线程（`std::thread` 跑 `engine_run`）
   - GUI 主循环
   - 退出时 `engine_request_stop` → 等线程 join → `engine_shutdown`

8. **不做的（推迟到 M8）**
   - 设备扫描 / 配对流程（XM5 BD_ADDR 暂仍 hardcode）
   - 系统托盘
   - 开机自启
   - 配置文件持久化
   - "显示所有蓝牙音频设备"高级选项

### 关键参考点

- 引擎接口在 `core/src/engine/engine.h`
- 状态 snapshot 字段都齐了：state, link_up, target_addr, sample_rate_hz, wasapi_bit_depth, effective_kbps, nominal_kbps, eqmid, bitrate_mode, underrun_samples, reconnect_attempts
- 跨线程命令：`engine_post_set_bitrate_mode`, `engine_post_set_target`, `engine_request_stop`
- `engine_persistent_test.c` 是引擎调用的最小例子（~80 行）

### 设计决策（来自 M7 路线图讨论）

| 决策 | 选择 | 理由 |
|---|---|---|
| 单 exe vs core/gui 双进程 | **单 exe** | 用户改变了原 HANDOFF 路线，简化部署 |
| GUI 框架 | **Dear ImGui + DX11** | 轻量、单 exe、足以做状态面板 |
| 配对流程 | **只识别 Sony XM 系列**（默认）+ "显示全部" 隐藏开关 | M8 实现 |
| 采样率/位深 GUI 控制 | **只显示不能改** | WASAPI loopback 不能改设备格式，给"打开 Windows 声音设置"按钮 |
| 码率模式 | 仅 **Fixed (990 kbps)** vs **Adaptive** 两档 | 砍掉 SQ/MQ 手动选项 |
| 码率显示 | **实际发送 kbps** + 60s 折线图 | 不是 nominal |
| 静音处理 | **2 秒静音自动 AVDTP SUSPEND** | 已实现 |

### 用户态服务模型澄清（重要背景）

用户在路线图讨论时说明了"我以为这是驱动"的误解。澄清后的方向：
- 我们写的是普通用户态程序
- 目标体验 = "桌面应用 + 系统托盘 + 开机自启" = 像 Logitech G Hub、Sony Headphones Connect
- 内核驱动 / 系统服务都是范围外（驱动要 EV 证书；系统服务有 session 隔离问题）
- 这条路线写进了 M9（系统集成 + 托盘 + 自启）

## 仓库当前状态

```
ldac/
├── 已 commit (main 分支)：到 M6.2 (13a9f64)
├── 已 commit (claude/m7-gui-engine 分支)：
│   ├── 734ad5e  M7 phase A: float32, ABR, engine library
│   └── f790b33  M7 phase A polish: silence pause, WASAPI rebuild, bit-depth fix
└── 待 merge：上面两个到 main（本会话结束前完成）
```

## Worktrees

每个里程碑用独立 worktree，避免互相干扰：
- `claude/recursing-cray-c9d142` (M5 + M6.1) — 已 merge
- `claude/m6.2-auto-reconnect` (M6.2) — 已 merge
- `claude/m7-gui-engine` (M7 phase A) — 本会话末 merge
- **下次开 GUI 用新 worktree**：建议 `claude/m7-gui-mvp`

每个 worktree 第一次 build 前需要 vendor/ldacBT 和 vendor/btstack 的 junction：
```powershell
cmd /c "mklink /J vendor\ldacBT D:\claude\ldac\vendor\ldacBT"
cmd /c "mklink /J vendor\btstack D:\claude\ldac\vendor\btstack"
```

git 操作时需要 rmdir 掉这两个 junction（否则 git 看到 submodule 链接会报错），git 完事再 mklink 回来。

## 已知问题 / 未来工作

- **ABR 阈值未调优**：libldac ABR 用默认 thresholds，可能对我们的 outstanding_packets 代理值不敏感。如果 Adaptive 模式不响应链路压力，调 `ldac_ABR_set_thresholds`（位置：`ldac_wrapper_abr_enable` 之后）
- **AVDTP 静音挂起的边界场景**：用户暂停 Spotify → 2s 后挂起。如果用户应用持续输出"接近静音但非零"音频（如 EQ 底噪），不会触发挂起。检测策略目前是字面 `== 0.0f`，符合 WASAPI 静音 flag 的行为，但可能要做幅度阈值
- **AVDTP RECONFIGURE 未使用**：Windows 改采样率走"完全断开 + 重连"路径（~5s 中断）。如果想要更平滑切换，可以试 BTstack 的 `avdtp_source_reconfigure_stream`，但 XM5 是否支持未测试
