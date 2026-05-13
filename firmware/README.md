# Firmware blobs

This directory contains proprietary Realtek Bluetooth firmware loaded into
USB BT dongles based on the **RTL8761B / RTL8761BU** chipset (e.g. TP-Link
UB4A, UB400v2, UB500; many Orico, EDUP, Ugreen branded dongles).

The dongle's hardware ships without firmware — every time you plug it in or
the system powers up, the host must push these blobs to the chip via HCI
vendor commands before the radio can be used. `vendor/btstack/chipset/realtek/`
contains the upload protocol; this directory contains the data.

## Files

| File | Source | Purpose |
|---|---|---|
| `rtl8761bu_fw` | linux-firmware tree, `rtl_bt/rtl8761bu_fw.bin` | The actual firmware patch loaded into the RTL8761BU |
| `rtl8761bu_config` | linux-firmware tree, `rtl_bt/rtl8761bu_config.bin` | Per-board hardware config tweaks |
| `LICENCE.rtlwifi_firmware.txt` | linux-firmware tree, `LICENCE.rtlwifi_firmware.txt` | Realtek's redistribution licence |

> **Note**: the filenames here drop the `.bin` suffix. BTstack's
> `btstack_chipset_realtek.c` constructs file paths as `<folder>/<patch_name>`
> with no extension; the kernel firmware tree convention adds `.bin`. We
> rename to match BTstack's expectation.

## Origin

Files were downloaded on 2026-05-12 from:

- https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/rtl_bt/rtl8761bu_fw.bin
- https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/rtl_bt/rtl8761bu_config.bin
- https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/LICENCE.rtlwifi_firmware.txt

## Licence and attribution

The firmware is `Copyright (c) 2010, Realtek Semiconductor Corporation`,
redistributable in binary form under the terms in
[`LICENCE.rtlwifi_firmware.txt`](LICENCE.rtlwifi_firmware.txt). The license
**permits redistribution** with the requirement to reproduce the copyright
notice (which this README does) and the disclaimer (in
LICENCE.rtlwifi_firmware.txt). No reverse engineering / decompilation /
disassembly is permitted.

This redistribution is for the purpose of pairing the firmware with our
LDAC source application on Windows; no Realtek branding is used to promote
the project.

## Other chipsets

For dongles based on other chipsets, drop the corresponding firmware blobs
here under the names BTstack expects:

| Chipset | Files |
|---|---|
| RTL8761A | `rtl8761au_fw` + `rtl8761a_config` (or `rtl8761au8192ee_fw`) |
| RTL8723D | `rtl8723du_fw` + `rtl8723du_config` |
| RTL8821C | `rtl8821cu_fw` + `rtl8821cu_config` |
| RTL8822C | `rtl8822cu_fw` + `rtl8822cu_config` |

All are available in the same linux-firmware `rtl_bt/` directory.

CSR8510 / CSR4.0 dongles have firmware in ROM — no files needed here.
