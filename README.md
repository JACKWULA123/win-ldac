# win-ldac

**让你的WindowsPC能够通过LDAC编码连接到你的索尼耳机，以最高990kpbs码率享受品质音乐。**

<img width="627" height="490" alt="捕获" src="https://github.com/user-attachments/assets/7b0199b9-5360-426a-9d64-ea993e0829bc" />


English README: README_EN.md

本方案需要一个外置USB蓝牙。如果你不想用，请搜索"Alternative A2DP Driver"。

#你看到这里就可以了，剩下的交给AI大人。
---

本仓库源代码是 **Apache 2.0**。可以自由 fork / 修改 / 自编 / 分享源码,但禁止分发预编译二进制。

| 芯片家族 | 例子 | 状态 |
|---|---|---|
| **Realtek RTL8761B / RTL8761BU** | TP-Link UB4A / UB400 v2 / UB500，Orico BTA-508，EDUP EP-9619，Ugreen 80889 | ✅ **首选** —— 便宜常见，固件已 bundle |
| **CSR8510** | "CSR 4.0" nano dongle、TP-Link UB400 v1（老版） | ✅ 可用 —— 但 CSR 已停产 |
| RTL8821C / RTL8822C / RTL8723D | 各家 OEM | 🟡 BTstack 支持；固件未 bundle，需自行下载放入 `firmware/` |
| Intel AX200 / AX210 | WiFi+BT 组合卡 | ❌ 多阶段固件加载未实现 |
| 笔记本 / PC 内置蓝牙 | — | ❌ **不要用**。驱动替换可能与 Wi-Fi 共天线冲突，恢复困难 |

- **关闭 Memory Integrity / 内核隔离**
  （设置 → 隐私和安全性 → Windows 安全中心 → 设备安全性 → 内核隔离 → 内存完整性 = 关闭，重启）

### 拉代码 + 依赖

```powershell
git clone --recursive https://github.com/JACKWULA123/win-ldac.git
cd win-ldac

# BTstack 故意不放在仓库里（见上面"许可"一节），用户自行 clone：
git clone https://github.com/bluekitchen/btstack.git vendor/btstack
```

`--recursive` 会同时拉 libldac、Dear ImGui、ImPlot 三个 submodule。

### 一键编译（推荐）

```powershell
.\build.ps1
```

脚本会校验依赖、跑 CMake、生成 `build\gui\Release\win-ldac.exe`。

### 手动编译

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

编译产物：
- `build\gui\Release\win-ldac.exe` —— GUI 主程序
- `build\core\Release\*.exe` —— CLI 工具（`engine_persistent_test` 是 GUI 的无窗口等价物）

## 目标机器首次设置

1. **插上 dongle。**
2. **跑 Zadig。** Options → List All Devices。选你的 dongle（**注意 VID/PID 不要选错成内置蓝牙**），驱动选 WinUSB，点 Replace Driver。
3. 你需要一个除了你的耳机之外的声音输出设备，比如你的笔记本扬声器。确保它是静音的。在声音设置-设备属性-其他设备属性-高级-默认格式中选择你想要的声音质量。最高24bit-96000Hz。这将是无线传输前的采样率。
4. **耳机进配对模式。** Sony XM 系列：长按右侧电源键约 7 秒，蓝灯快闪。
5. **运行 `win-ldac.exe`。** 首次启动显示 *Not paired*。点 *Pair new device* → *Scan now* → 等 ~10 秒 → 选你的耳机 → *Pair*。
6. 之后启动会**自动连接**，Windows 一播音就开始传。

## 项目结构

```
win-ldac/
├── README.md            ← 英文版
├── README.zh.md         ← 你正在看的这个
├── LICENSE              ← Apache 2.0
├── NOTICE               ← 第三方组件版权
├── STATUS.md            ← 里程碑进度
├── build.ps1            ← 一键编译
├── CMakeLists.txt
├── cmake/               ← BTstack + MSVC compat shim
├── core/                ← 引擎库 + CLI 工具
├── gui/                 ← win-ldac.exe (Dear ImGui + DX11)
├── assets/              ← 字体、水印、图标
├── firmware/            ← Realtek RTL8761BU 固件
├── docs/
│   └── ARCHITECTURE.md  ← 详细架构 / 设计文档（前身 HANDOFF.md）
├── third_party/         ← imgui、implot（git submodule）
└── vendor/
    ├── ldacBT/          ← Sony libldac（git submodule）
    └── btstack/         ← BTstack（用户自拉，不是 submodule）
```

## 深入阅读

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) —— 按组件展开的设计笔记、A2DP/AVDTP 协议要点、库 API 参考、已知坑列表
- [`STATUS.md`](STATUS.md) —— 里程碑式的进度日志
- [`assets/README.md`](assets/README.md) —— UI 素材说明 + license 归属
- [`firmware/README.md`](firmware/README.md) —— Realtek 固件 bundling 说明

## 致谢

- **Sony** 把 LDAC 编码器开源到 Apache 2.0
- **EHfive** 的 [CMake 化 libldac 包装](https://github.com/EHfive/ldacBT)
- **BlueKitchen GmbH** 的 [BTstack](https://github.com/bluekitchen/btstack)
- **Omar Cornut** 的 [Dear ImGui](https://github.com/ocornut/imgui)
- **Evan Pezent** 的 [ImPlot](https://github.com/epezent/implot)
- **subframe7536** 的 [Maple Mono](https://github.com/subframe7536/Maple-font)
- **AOSP Bluetooth 团队** 的 LDAC A2DP 参考实现
- **Linux kernel firmware** 项目的 Realtek RTL8761BU 固件
