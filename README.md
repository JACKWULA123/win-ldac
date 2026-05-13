# Windows LDAC

让 Windows 10 PC 通过外接 USB 蓝牙 dongle 用 **LDAC** 编码连接索尼 / 其它支持 LDAC 的蓝牙耳机，提供最高 990 kbps 无线音质。

> ⚠️ **项目仍在开发中**。当前进度见 [STATUS.md](STATUS.md)。完整技术文档见 [HANDOFF.md](HANDOFF.md)。

## 为什么需要这个

Windows 内置蓝牙栈只支持 SBC，最高 ~328 kbps。LDAC（Sony 990 kbps）在 Windows 上没有原生支持，市面上唯一可用的是付费闭源软件。本项目是其开源替代。

## 工作原理

- 一个独立的 **USB 蓝牙 dongle**，驱动由 Zadig 替换为 WinUSB
- 用户态 C++ 程序通过 BTstack 直接操作 dongle
- WASAPI loopback 抓系统音频 → libldac 编码 → A2DP source 发送
- **Windows 自带蓝牙不动**（你的鼠标键盘照常用）

## 硬件要求

- Windows 10 (x64) 或更高版本
- **一只独立 USB 蓝牙 dongle**（不能用笔记本/PC 内置的）
- 支持 LDAC 的蓝牙耳机（如 Sony WH-1000XM5）

### 支持的 USB BT Dongle

| 推荐度 | 芯片 | 例子（市面常见型号） | 备注 |
|---|---|---|---|
| 🌟 首选 | **Realtek RTL8761B / RTL8761BU** | TP-Link **UB4A** / UB400 v2 / UB500，Orico BTA-508，EDUP EP-9619 | 固件已 bundle 在 `firmware/` |
| ✅ 可用 | CSR8510 (CSR 4.0) | 各种白牌 "CSR 4.0 nano BT dongle"，TP-Link UB400 v1 | 零固件，但芯片已停产 |
| 🟡 实验性 | RTL8821C / RTL8822C / RTL8723D 等 | Realtek 其它型号 | 需自行下载对应固件到 `firmware/`（见 `firmware/README.md`） |
| ❌ 不支持 | Intel AX 系列、Broadcom 系列 | — | 固件加载协议复杂，未集成 |

> ⚠️ **绝对不要用笔记本/PC 内置的蓝牙模块**。本项目会接管 USB BT dongle 的驱动；如果接管的是内置蓝牙，可能影响 Wi-Fi 共天线，且很难恢复。Zadig 选错设备时一定检查 VID/PID。

## 编译方式

### 一次性环境
- **Visual Studio 2022**（含 C++ 桌面开发 + CMake）
- **Git**
- **Zadig** 2.8+：<https://zadig.akeo.ie/>
- **关闭 Windows "Memory Integrity"**（设置 → 隐私和安全性 → Windows 安全中心 → 设备安全性 → 内核隔离），否则 Zadig 替换驱动会失败。改完需重启。

### 拉代码并编译

```bash
git clone <repo-url> ldac
cd ldac
git submodule update --init --recursive

# BTstack 不在本仓库（license 约束，详见下方），请自行 clone：
git clone https://github.com/bluekitchen/btstack.git vendor/btstack

cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

### 替换 USB BT dongle 驱动

1. 插上 dongle
2. 打开 Zadig，菜单 **Options → List All Devices**
3. 在下拉框找到你的 dongle（**注意 VID/PID，不要选错成内置蓝牙！**）
4. 右侧驱动选 **WinUSB**，点 **Replace Driver**

### 运行

```bash
build\core\Release\win-ldac-core.exe
```

首次运行需要让耳机进入配对模式（XM5 是长按右侧电源键 7 秒，蓝灯快闪）。

## License

- **本项目自身代码**：Apache License 2.0
- **依赖项**：
  - [libldac](https://android.googlesource.com/platform/external/libldac) (Sony) — Apache 2.0
  - [Dear ImGui](https://github.com/ocornut/imgui) — MIT
  - [nlohmann/json](https://github.com/nlohmann/json) — MIT
  - **[BTstack](https://github.com/bluekitchen/btstack)** — **个人/非商业免费**，商业用途须向 [BlueKitchen GmbH](https://bluekitchen-gmbh.com/) 购买授权

### ⚠️ 分发限制

由于 BTstack 的非商业 license：

- ✅ 可以 fork、修改、自用、给朋友传源码
- ❌ **不要分发预编译二进制**——任何形式（GitHub Release、CI artifact、网盘分享）都不行
- ❌ **不要商用**

如果你需要分发二进制或商用，请联系 BlueKitchen 取得 BTstack 商业许可，或等本项目升级到"路线 1"（自写蓝牙栈，时间未定）。

## 贡献 / 反馈

项目仍在早期阶段。技术细节看 [HANDOFF.md](HANDOFF.md)。

## 致谢

- Sony 开源了 LDAC 编码器
- BlueKitchen 的 BTstack 提供了高质量的用户态蓝牙栈
- AOSP 的 `a2dp_vendor_ldac_encoder.cc` 是关键参考实现
