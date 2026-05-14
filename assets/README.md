# assets/

Runtime assets for `win-ldac.exe`. **Not committed** to the repo
(font has its own licence, watermark is per-user); place the files
yourself before building, or the GUI will fall back to defaults.

## Files the GUI looks for

The GUI walks up from the `win-ldac.exe` directory (max 5 levels) until
it finds an `assets/` folder, then loads:

| File | Purpose | Fallback if missing |
|---|---|---|
| `MapleMono-Regular.ttf` | Body / header font | ImGui built-in ProggyClean |
| `monkey.png` | Chart-area watermark | Plain transparent chart |

## Where to get them

### `MapleMono-Regular.ttf`

Maple Mono (rounded monospace, SIL OFL 1.1 licence). From the upstream
release page <https://github.com/subframe7536/Maple-font/releases>,
download the latest TTF archive and copy
`MapleMono-Regular.ttf` into this directory (any "Regular" weight from
the Maple Mono / Maple Mono NL / Maple Mono CN variants works — just
rename it to `MapleMono-Regular.ttf`).

If you'd rather skip it, the GUI uses ImGui's default ProggyClean
13-pixel bitmap font and everything still renders.

### `monkey.png`

The chart-area watermark. Any reasonably horizontal PNG works (the
loader applies CSS `object-fit: cover` so it fills the chart and
crops the protrusion). Recommended size: ≤ 600 px wide; bigger
just wastes memory because the watermark is alpha-blended at ~20%.

## Worktree note

If you build from a `.claude/worktrees/*/` worktree, create a junction
to the main repo's `assets/` so the exe finds the files:

```powershell
cmd /c "mklink /J assets D:\path\to\ldac\assets"
```
