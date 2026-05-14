# STATUS

最近更新：2026-05-14

## 当前里程碑
**M9 完成 ✅** — 系统托盘 + 关窗隐藏 + 开机自启 + 任务栏图标
**下一步**：M10 — README + 一键 build 脚本（朋友能照着在干净 Windows 上 build）

## 已完成的里程碑

| 里程碑 | 完成内容 |
|---|---|
| **M1** | libldac 通编（990 kbps HQ 合成测试） |
| **M2** | BTstack HCI + Realtek RTL8761BU 固件加载 |
| **M3.1** | A2DP source SBC + 正弦波 → XM5 |
| **M3.2** | WASAPI loopback → SBC → XM5 |
| **M4** | LDAC capability 协商（Sony vendor 0x12D codec 0x00AA） |
| **M5** | LDAC 真实出声（HQ 48k 990 kbps，链式 CAN_SEND_NOW） |
| **M6.1** | 链路密钥持久化（重启免重新配对） |
| **M6.2** | 断连自动重连 |
| **M7 Phase A** | F32 PCM 端到端、ABR、引擎库、静音自动挂起、Windows 格式变更动态重建 |
| **M7 Phase B** | Dear ImGui + DX11 GUI（设备卡片、ImPlot 60s 曲线、Mode 分段控件） |
| **M8** | 配对流程 + 配置文件持久化（解锁分发） |
| **M9** | 系统托盘 + 开机自启 + 任务栏图标 + 不支持率友好弹窗 |

## M8 完成内容（本会话）

### 引擎侧 (`core/src/engine/engine.{c,h}`)
- `engine_status_t.has_target` / `wasapi_unsupported_rate_hz` 字段
- 新增 scan API：`engine_get_scan_state`、`engine_post_start_scan` / `_stop_scan` / `_clear_scan_results`
- `engine_post_clear_target()`、`engine_post_set_target(zero)` 等价于清除
- HCI 事件处理新增 `GAP_EVENT_INQUIRY_RESULT` / `_COMPLETE` / `HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE`
- `engine_init` 在未配对时跳过 supervisor 启动；在 `HCI_STATE_WORKING` 时按需启动
- 不支持率（≠ 44.1/48/88.2/96 kHz）检测：init 和 runtime rebuild 两条路径均上报 `wasapi_unsupported_rate_hz`

### 配置文件 (`core/src/app/config_file.{c,h}`)
- 简单 `key=value` 文本格式，路径 `<exe_dir>\win-ldac-config.cfg`
- 字段：`target_addr` / `target_name` / `bitrate_mode`
- 原子写入 `.tmp` + `MoveFileEx`，未知 key 忽略

### GUI
- 启动读 config → 把 target 喂给 engine；callbacks (`on_pair` / `on_unpair` / `on_bitrate_persist`) 把改动落盘
- 设备卡片首行显示 `ui.target_name` 或 "Not paired" / "Paired device" fallback
- 配对 modal：`Scan now` + 设备列表 (Selectable) + `Show non-audio devices` 过滤 + `Unpair current` / `Cancel` / `Pair`
- 不支持率弹窗：`draw_unsupported_rate_popup` 检测到非零率自动弹出，含 mmsys.cpl 操作步骤说明
- Settings 按钮改名 `Pair new device`

## M9 完成内容（本会话）

### 系统托盘 (`gui/src/main.cpp`)
- `Shell_NotifyIconW(NIM_ADD)` 在窗口创建后安装
- 自定义消息 `WM_TRAY_CALLBACK = WM_APP + 1`
- 左键单击/双击 → `ShowWindow(SW_SHOW)` + `SetForegroundWindow`
- 右键弹出 `TrackPopupMenu`：Open / Start with Windows ✓ / Quit
- Tooltip 每帧从 `engine_status_t` 构建，`NIM_MODIFY` 内部 debounce 仅在文本变化时实际写

### 关窗行为
- `WM_CLOSE` 改成 `ShowWindow(SW_HIDE)` —— X 按钮不再退出，引擎继续在后台跑
- 退出走 `g_quit_requested`（托盘 Quit 菜单 / GUI Quit 按钮设置）→ 主循环 `DestroyWindow` → `WM_DESTROY` 里 `tray_remove` + `PostQuitMessage`

### 隐藏时的 CPU 节省
- `IsWindowVisible(hwnd)` 为 false 时跳过 ImGui frame loop + `Sleep(50)`
- 引擎线程继续跑、音频继续流；仅 GUI 不渲染

### 开机自启
- 注册表 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\win-ldac` = 加引号的 exe 完整路径
- 托盘菜单 `Start with Windows` 项 toggle，读 / 写 / 删通过 `RegOpenKeyExW` + `RegSetValueExW` / `RegDeleteValueW`

### 任务栏 / 标题栏 / 托盘图标
- `assets/pic.png` (1010×1010) → Pillow Lanczos 降到 256×256 → `win-ldac.ico` 多尺寸 (16/24/32/48/64/128/256)
- `gui/win-ldac.rc.in` + `configure_file` 把绝对路径 token 替换 → `IDI_TRAY_ICON ICON` 资源嵌入 exe
- `resources.h` 定义 `IDI_TRAY_ICON = 101`
- `WNDCLASSEX.hIcon` / `hIconSm` 用 `LoadIconW(hInst, MAKEINTRESOURCEW(IDI_TRAY_ICON))` → 标题栏 / Alt-Tab / 任务栏 / 托盘四处统一
- 找不到资源 → fallback 到 `IDI_APPLICATION`，dev 编译不会因缺图崩

### 资源 bundling
- `MapleMono-Regular.ttf` (SIL OFL 1.1)、`monkey.png`、`pic.png`、`win-ldac.ico`、`OFL.txt`、`assets/README.md` 全部 commit
- `.gitignore` 改成 allowlist：`assets/*` 默认忽略，白名单上述文件

## 下一步：M10

### 任务列表
1. 顶层 `README.md`（中文优先 + 英文 build steps）
2. `build.ps1` 一键脚本（cmake configure + build）
3. CI / hardware 文档：哪些 dongle 已验证、Zadig 步骤、Memory Integrity 关闭说明
4. docs/ARCHITECTURE.md §6 同步更新最新 build 命令

### 不做的
- 不打包预编译二进制（docs/ARCHITECTURE.md §9 license 约定）
- 不写自动 CI（朋友自编场景不需要）

## 已知问题
- ABR 阈值未调优
- AVDTP RECONFIGURE 未使用（Windows 改采样率走 disconnect+reconnect 路径，~5s 中断）
- 静音挂起的幅度阈值（`== 0.0f` 严格判定，接近静音但非零的源不触发）
- 不支持率（192k 等）从 192k 改回 48k 后**仍需手动重启 win-ldac**，没有自动恢复路径

## 仓库当前状态

```
main 分支：
  d6441e6  M7 phase B: Dear ImGui + DirectX 11 GUI
  255823e  M7 phase A handoff
  f790b33  M7 phase A polish
  734ad5e  M7 phase A
  ...

当前 worktree (claude/naughty-johnson-262a49)：
  待 commit: M8 + M9 + 资源 bundling 全部
```

## Worktrees 维护

每个 worktree 第一次 build 前需要 4 个 junction：

```powershell
cmd /c "mklink /J vendor\ldacBT D:\claude\ldac\vendor\ldacBT"
cmd /c "mklink /J vendor\btstack D:\claude\ldac\vendor\btstack"
cmd /c "mklink /J third_party\imgui  D:\claude\ldac\third_party\imgui"
cmd /c "mklink /J third_party\implot D:\claude\ldac\third_party\implot"
cmd /c "mklink /J assets D:\claude\ldac\assets"
```

注意 `third_party/` 现在是真实目录（含 `README.md`），所以 `imgui` / `implot` 是 **子级** junction。`assets/` 仍然整目录 junction（因为我们的素材也存在主仓库的同一位置）。

git 操作时需要 rmdir 掉这些 junction（vendor 整目录 + third_party 两子目录 + assets 整目录），git 完事再 mklink 回来。
