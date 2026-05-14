# win-ldac

**LDAC for Windows.** Stream system audio from your PC to Sony WH-1000XM-class
(or any LDAC-capable) Bluetooth headphones at up to **990 kbps**, the same
quality you'd get from a modern Android phone.

> 🇨🇳 中文 README：[README.zh.md](README.zh.md)

Windows's built-in Bluetooth stack only does SBC (~328 kbps). LDAC support has
historically only existed in expensive closed-source software. `win-ldac` is
the open-source alternative.

| Status | M9 complete — full GUI + system tray + auto-start |
|---|---|

## How it works

```
┌─────────────────────────────────────────────────────────────────────┐
│  Windows 10/11 user-space                                           │
│                                                                     │
│   [ System audio ] ──► WASAPI loopback ──► libldac ──► A2DP source  │
│                                              (Sony,        │        │
│                                              Apache 2.0)   ▼        │
│                                                         BTstack     │
│                                                         (user-fetch)│
│                                                            │        │
│                                                            ▼        │
│                                            [ USB Bluetooth dongle ] │
│                                            (WinUSB driver via Zadig)│
└────────────────────────────────────────────┼────────────────────────┘
                                             │
                                             ▼  Bluetooth 2.4 GHz
                                  [ Sony WH-1000XM5 ]
```

- A **separate USB Bluetooth dongle** is used, with its driver swapped
  from Microsoft's stack to **WinUSB** via [Zadig](https://zadig.akeo.ie/).
- A user-space C++ program drives the dongle through
  [BTstack](https://github.com/bluekitchen/btstack), encodes the audio
  with [libldac](https://github.com/EHfive/ldacBT), and pushes RTP-framed
  LDAC over A2DP.
- **Your built-in Bluetooth is untouched** — keep using your mouse and
  keyboard normally.

## Screenshot

> _GUI built with [Dear ImGui](https://github.com/ocornut/imgui) + DirectX 11.
> A live screenshot will be added once the repo is public._

## Licence & distribution constraints — read this first

This repository's source code is **Apache 2.0**. You can fork, modify,
self-build, and share the source freely.

**But you may NOT distribute compiled binaries.**

`win-ldac` links against **[BTstack](https://github.com/bluekitchen/btstack)**,
which is free for personal / non-commercial use only. Commercial or
redistribution use requires a paid licence from BlueKitchen GmbH. To stay on
the safe side this project ships **source only** — each user clones the
repository and builds locally for their own machine.

In practice:

- ✅ Clone, build, use for yourself — fine.
- ✅ Fork, modify, push your fork to GitHub — fine.
- ✅ Show a friend the repo so they can build it themselves — fine.
- ❌ Upload `win-ldac.exe` anywhere public.
- ❌ Sell it.
- ❌ Bundle it with any redistributable installer.

See [`NOTICE`](NOTICE) for the full licence map.

## Hardware requirements

### USB Bluetooth dongle

You **must** have a USB BT dongle dedicated to `win-ldac` (the driver gets
swapped to WinUSB, so it can't be used by Windows in parallel).

| Chipset family | Examples | Status |
|---|---|---|
| **Realtek RTL8761B / RTL8761BU** | TP-Link UB4A / UB400 v2 / UB500, Orico BTA-508, EDUP EP-9619, Ugreen 80889 | ✅ **First choice** — cheap, common, firmware bundled in this repo |
| **CSR8510** | "CSR 4.0" nano dongles, old TP-Link UB400 v1 | ✅ Works — but CSR is discontinued |
| RTL8821C / RTL8822C / RTL8723D | Various OEM | 🟡 BTstack supports them; firmware not bundled. Add the relevant `.bin` to `firmware/` |
| Intel AX200 / AX210 | WiFi+BT combo cards | ❌ Multi-stage firmware load not implemented |
| Built-in laptop / PC Bluetooth | — | ❌ **Do not use.** Driver swap may co-occupy Wi-Fi antennas and be hard to recover. |

### Headphones

Any A2DP sink that advertises the LDAC vendor codec:

- Sony WH-1000XM3 / XM4 / XM5 / XM6 (the test target)
- Sony WF-1000XM3 / XM4 / XM5
- Sony LinkBuds, ULT Wear, etc.
- Most non-Sony Hi-Res-certified IEMs that licence LDAC (FiiO, Anker, etc.)

If the headphones speak LDAC, this should work.

## Build from source

### Prerequisites

- **Windows 10 (x64)** or newer
- **Visual Studio 2022** with the *Desktop development with C++* workload
  (includes MSVC, the Windows SDK ≥ 10.0.22621, and CMake ≥ 3.16)
- **Git** with submodule support
- **Zadig** ≥ 2.8 ([zadig.akeo.ie](https://zadig.akeo.ie/))
- A supported BT dongle
- **Memory Integrity / Core Isolation turned OFF**
  (Settings → Privacy & security → Windows Security → Device security →
  Core isolation → Memory integrity = Off, reboot)

### Clone + dependencies

```powershell
git clone --recursive https://github.com/JACKWULA123/win-ldac.git
cd win-ldac

# BTstack is user-fetched on purpose — see "Licence" above.
git clone https://github.com/bluekitchen/btstack.git vendor/btstack
```

`--recursive` pulls libldac, Dear ImGui, and ImPlot via git submodules.

### One-click build (recommended)

```powershell
.\build.ps1
```

The script verifies dependencies, runs CMake, and produces
`build\gui\Release\win-ldac.exe`.

### Manual build

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

After build:

- `build\gui\Release\win-ldac.exe` — the GUI app
- `build\core\Release\*.exe` — CLI tools (`engine_persistent_test` is the
  headless equivalent of the GUI)

## First-time setup on the target machine

1. **Plug in the dongle.**
2. **Run Zadig.** Options → List All Devices. Pick your dongle (double-check
   VID/PID — don't touch built-in Bluetooth). Driver = WinUSB. Replace driver.
3. **Put headphones in pairing mode.** Sony XM-series: long-press the right
   power button ~7 s until the light flashes blue.
4. **Run `win-ldac.exe`.** First launch shows *Not paired*. Click
   *Pair new device* → *Scan now* → wait ~10 s → select your headphones →
   *Pair*.
5. Subsequent launches **auto-connect** as soon as Windows plays audio.

## Day-to-day use

- Closing the window (`X`) **hides** to the system tray. The app keeps
  running and audio keeps flowing.
- Double-click the tray icon to bring the window back.
- Right-click the tray icon for *Open / Start with Windows / Quit*.
- Switch between **Fixed (990 kbps)** and **Adaptive** in the main UI.
  Adaptive lowers the bitrate (to 660 / 330) when the radio link is weak.
- **Connection Refresh** force-reconnects when audio glitches.

## Troubleshooting

| Symptom | Fix |
|---|---|
| GUI shows *Unsupported sample rate* | LDAC only accepts 44.1 / 48 / 88.2 / 96 kHz. Open `mmsys.cpl` → right-click your default output → Properties → Advanced → set Default Format. Restart `win-ldac.exe`. |
| Scan finds nothing | Make sure headphones are in **pairing mode** (fast blue blink), not just on. The first inquiry occasionally misses devices — click *Scan now* again. |
| Dongle shows ⚠ in Device Manager after Zadig | Memory Integrity is still on. Turn it off, reboot, re-run Zadig. |
| Connects but no sound | Make sure Windows's *default playback device* is the speaker you want to mirror — `win-ldac` captures whatever Windows is playing. |
| Audio stutters | Move closer to the dongle / switch to **Adaptive** mode / click *Connection Refresh*. |
| "VCRUNTIME140.dll missing" on a fresh machine | Install [Microsoft Visual C++ Redistributable 2015–2022](https://aka.ms/vs/17/release/vc_redist.x64.exe). |

## Project structure

```
win-ldac/
├── README.md            ← you are here
├── README.zh.md         ← Chinese version
├── LICENSE              ← Apache 2.0
├── NOTICE               ← third-party copyrights
├── STATUS.md            ← milestone progress
├── build.ps1            ← one-click build
├── CMakeLists.txt
├── cmake/               ← BTstack + MSVC compat shims
├── core/                ← engine library + CLI tools
│   ├── CMakeLists.txt
│   └── src/{a2dp_ldac,app,audio,bt,engine,ldac,tools}/
├── gui/                 ← win-ldac.exe (Dear ImGui + DX11)
│   ├── CMakeLists.txt
│   ├── win-ldac.rc.in
│   └── src/{ui/}
├── assets/              ← font, watermark, icon
├── firmware/            ← Realtek RTL8761BU firmware
├── docs/
│   └── ARCHITECTURE.md  ← deep-dive design doc (formerly HANDOFF.md)
├── third_party/         ← imgui, implot (git submodules)
└── vendor/
    ├── ldacBT/          ← Sony libldac (git submodule)
    └── btstack/         ← BTstack (user-fetched, NOT a submodule)
```

## Deeper documentation

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — component-by-component
  design notes, A2DP/AVDTP protocol details, library API references, and
  the laundry list of bugs / gotchas already worked around.
- [`STATUS.md`](STATUS.md) — milestone-by-milestone progress log.
- [`assets/README.md`](assets/README.md) — UI asset details + licence
  attributions.
- [`firmware/README.md`](firmware/README.md) — Realtek firmware bundling
  notes.

## Contributing

This is a personal project. PRs welcome but please open an issue first
describing the change. Don't post compiled binaries in releases or
artifacts — that violates BTstack's licence (see *Licence & distribution
constraints* above).

## Acknowledgements

- **Sony** for releasing the LDAC encoder under Apache 2.0
- **EHfive** for the [CMake-friendly libldac wrapper](https://github.com/EHfive/ldacBT)
- **BlueKitchen GmbH** for [BTstack](https://github.com/bluekitchen/btstack)
- **Omar Cornut** for [Dear ImGui](https://github.com/ocornut/imgui)
- **Evan Pezent** for [ImPlot](https://github.com/epezent/implot)
- **subframe7536** for [Maple Mono](https://github.com/subframe7536/Maple-font)
- The **AOSP Bluetooth team** for the LDAC A2DP reference implementation
- **Linux kernel firmware project** for the Realtek RTL8761BU blobs
