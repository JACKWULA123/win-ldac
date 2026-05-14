# STATUS

最近更新：2026-05-14

## 当前里程碑
**M7 Phase B 完成 ✅** — Dear ImGui + DirectX 11 GUI 上线
**下一步**：M8 — 配对流程 + 配置持久化（解锁分发场景）

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
| **M7 Phase B** | Dear ImGui + DX11 GUI（设备卡片、ImPlot 60s 曲线、Mode 分段控件、How2Use 弹窗） |

## M7 Phase B 已完成（本会话）

提交：本会话末统一 commit。

### 引擎侧补充
- **`engine_status_t.idle_paused`**：暴露 a2dp_ldac_source idle-paused 状态给 GUI
- **`engine_post_disconnect()`**：跨线程主动断链 + 触发 supervisor 重连，给 GUI 的 "Connection Refresh" 按钮用
- **`btstack_run_loop_trigger_exit()` 替换 `exit(0)/exit(3)`**：让 `engine_run()` 干净返回，`engine_shutdown()` 能正常执行，进程不再硬切

### GUI 模块（`gui/`）
- **CMake**：把 imgui (core + DX11 + Win32 backends) 和 implot 编成静态库，链进 `win-ldac.exe`；如果 `third_party/imgui` 或 `implot` 不在则 GUI target 安静地 skip，CLI 工具仍能构建
- **WinMain**：固定 500×360 client，`WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX`（去掉缩放和最大化）；DX11 设备 + 消息泵
- **引擎线程**：`std::thread` 跑 `engine_init/run/shutdown`，与 GUI 主线程通过 `engine_get_status_snapshot()` (CRITICAL_SECTION 加锁) 和 `engine_post_*` 命令交互
- **字体**：ImGui 1.92 的 dynamic font sizing，单 TTF 文件渲染任意像素尺寸；Maple Mono 用户自放在 `assets/`，否则回退 ProggyClean
- **图片**：WIC 加载 PNG → DX11 ShaderResourceView → ImDrawList::AddImage 渲染；猴子水印 480×167 PNG，alpha 50/255，CSS object-fit: cover 算法填满图表卡片

### UI 布局（500×360 客户区）
- **设备卡片** (56h)：左 "WH-1000XM5 ● Connected" + BD_ADDR；右大字 "988 kbps" + 小字 "LDAC HQ 96000 Hz 24-bit"，用 ImGui Table 55:45 分栏 + `right_aligned_text` 帮助函数
- **图表卡片** (196h)：透明 ImPlot frame + 猴子背景；曲线蓝色 1.8 粗，参考线 330/660/990 虚淡
- **Mode 行** (30h)：自写 `segmented_button` 实现分段开关（选中蓝填充、未选中浅灰），右侧 `How2Use?` 次级白按钮
- **底部按钮行** (26h)：`Connection Refresh`（蓝主按钮）/ `Settings?`（禁用占位）/ `Quit`
- **How2Use 弹窗**：`BeginPopupModal` 420×240 居中弹出，正文目前是占位

### 资源管理
- **`.gitignore`** 新增 `third_party/imgui`、`third_party/implot`、`assets/*`（除 README）
- **`assets/README.md`**：说明从哪下载 Maple Mono、放在哪、worktree 怎么建 junction
- **`third_party/README.md`**：说明 clone imgui/implot 的命令

硬件实测：所有功能正常（连接、流式、Adaptive 切换、Connection Refresh、Quit 干净退出）。

## 下一步：M8 — 配对流程 + 配置持久化

### 目标
解锁"分享给朋友"的场景。当前 GUI 把 XM5 BD_ADDR 硬编码在 `gui/src/main.cpp` 的 `TARGET_DEVICE_ADDR`，朋友想用必须改源码重编。M8 让程序在首次运行时扫描周边蓝牙设备、用户选一台后把 BD_ADDR 存到 JSON 配置文件，之后启动自动读取。

### 任务列表

1. **HCI Inquiry 扫描 API**（引擎侧）
   - `engine_post_start_scan(duration_s)` / `engine_post_stop_scan()`
   - 扫描事件回调（`HCI_EVENT_INQUIRY_RESULT_*`、`HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE`）通过事件队列暴露给 GUI
   - 设备列表数据结构：`{bd_addr, name, rssi, cod, last_seen_ms}`，去重 + RSSI 衰减

2. **配置文件**（引擎侧）
   - `app/config_file.{c,h}`：读写 `config.json`（用 nlohmann/json 或更简单的手写 parser，避免引入新依赖）
   - 字段：`target_addr`、`target_name`、`bitrate_mode`、`local_name`、`reconnect_interval_ms`
   - 路径：`%APPDATA%\win-ldac\config.json`（或 exe 同目录，需决定）
   - 启动时读取，无配置则进入"未配对"状态（不启动 supervisor）

3. **GUI 配对面板**
   - 未配置 target 时：主界面变成 "Click Settings to pair a device"
   - `Settings?` 按钮（目前禁用）点开 → 弹出 modal：
     - 左侧设备列表（live update，扫描中带 spinner）
     - 右侧选中设备详情 + "Pair" 按钮
     - 默认只显示 CoD 包含 Audio Major Service Class 的设备；隐藏开关 "Show all devices"
   - Pair 流程：写配置 → `engine_post_set_target` → 关弹窗

4. **handoff 整理**
   - 写 `assets/.gitkeep` 或类似让目录存在
   - 更新 HANDOFF.md §7 加 M8 任务

### 关键参考
- BTstack `port/windows-winusb/main.c` 的 `start_scan()` / `stop_scan()` 实现可以抄
- HCI Inquiry 的 RSSI/EIR 模式已在 `engine_init()` 里设置 (`hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR)`)
- Sony XM 系列的 Major Service Class = 0x200000（Audio）

## 已知问题 / 未来工作

- **ABR 阈值未调优**：libldac ABR 用默认 thresholds，可能对我们的 outstanding_packets 代理值不敏感
- **AVDTP RECONFIGURE 未使用**：Windows 改采样率走"完全断开 + 重连"路径（~5s 中断）
- **静音挂起的幅度阈值**：当前 `== 0.0f` 严格判定，对持续接近静音但非零的音频不触发
- **GUI 教程文案**：`gui/src/ui/status_window.cpp::draw_how2use_popup` 中 `(Tutorial content to be written.)` 待用户填充
- **系统托盘 / 开机自启**：M9 实现

## 仓库当前状态

```
main 分支 commit chain:
  255823e  M7 phase A handoff: STATUS.md for phase B
  f790b33  M7 phase A polish
  734ad5e  M7 phase A: float32, ABR, engine library
  13a9f64  M6.2: auto-reconnect
  ...

当前 worktree (claude/naughty-johnson-262a49):
  待 commit: M7 Phase B GUI 全部改动
```

## Worktrees 维护

每个 worktree 第一次 build 前需要 4 个 junction：

```powershell
cmd /c "mklink /J vendor\ldacBT D:\claude\ldac\vendor\ldacBT"
cmd /c "mklink /J vendor\btstack D:\claude\ldac\vendor\btstack"
cmd /c "mklink /J third_party D:\claude\ldac\third_party"
cmd /c "mklink /J assets D:\claude\ldac\assets"
```

git 操作（commit/push/pull/log）时需要 rmdir 掉这四个 junction，git 完事再 mklink 回来。
