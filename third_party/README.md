# third_party/

User-fetched UI dependencies. **Not committed**: both libraries are
MIT-licensed and small, but mirroring the BTstack arrangement keeps
this repo's own footprint clean.

The `gui/` CMakeLists.txt looks for the two folders below; if either
is missing the GUI target is silently skipped (the CLI tools still
build).

## Setup

From the repository root:

```powershell
git clone https://github.com/ocornut/imgui.git    third_party/imgui
git clone https://github.com/epezent/implot.git   third_party/implot
```

Versions known to work:
- Dear ImGui ≥ **1.92** (needs the dynamic-size `PushFont(font, size)` API)
- ImPlot **HEAD as of 2026-05** (the `ImPlotSpec` constructor pattern)

## Worktree note

For builds out of `.claude/worktrees/*/`, create a junction back to the
main repo's `third_party/`:

```powershell
cmd /c "mklink /J third_party D:\path\to\ldac\third_party"
```
