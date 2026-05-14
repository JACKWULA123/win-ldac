# assets/

UI assets bundled with `win-ldac.exe`. After M9 the project ships its
own defaults so a fresh checkout looks like the screenshots out of the
box. Replace any file in this directory to customise.

## Contents

| File | Purpose | Licence |
|---|---|---|
| `MapleMono-Regular.ttf` | Body / header font | SIL OFL 1.1 — see `OFL.txt` |
| `monkey.png` | Chart-area watermark (480×167, alpha-blended at ~20%) | Open-source asset, per project owner |
| `pic.png` | Tray / window icon source (256×256) | Open-source asset, per project owner |
| `win-ldac.ico` | Multi-size `.ico` embedded into the exe as a Win32 resource (rebuilt from `pic.png`) | derived from `pic.png` |
| `OFL.txt` | Full text of the SIL Open Font License 1.1 covering `MapleMono-Regular.ttf` | — |

## Loader behaviour

The GUI walks up from the `win-ldac.exe` directory (max 5 levels) until
it finds an `assets/` folder, then loads:

- `MapleMono-Regular.ttf` for the body font; falls back to ImGui's
  ProggyClean if absent.
- `monkey.png` as the chart watermark; falls back to no watermark if
  absent.

The tray / window icon is **embedded into `win-ldac.exe` at build time**
from `win-ldac.ico` via the Win32 resource compiler — it isn't loaded
from disk at runtime. To change the icon, regenerate `win-ldac.ico`
and rebuild.

## Regenerating `win-ldac.ico` from `pic.png`

```bash
python -c "from PIL import Image; im=Image.open('assets/pic.png'); im.save('assets/win-ldac.ico', sizes=[(16,16),(24,24),(32,32),(48,48),(64,64),(128,128),(256,256)], format='ICO')"
```

## Worktree note

For builds out of `.claude/worktrees/*/`, junction the main repo's
`assets/` so the runtime loader and the resource compiler both see the
same files:

```powershell
cmd /c "mklink /J assets D:\path\to\ldac\assets"
```
