# third_party/

UI dependencies for `win-ldac.exe`. Both are tracked as **git submodules**
(see [`.gitmodules`](../.gitmodules)) — after `git clone --recursive` (or
`git submodule update --init --recursive`) you should see populated
directories here.

## Contents

| Path | Upstream | Licence | Pinned commit |
|---|---|---|---|
| `imgui/`  | [ocornut/imgui](https://github.com/ocornut/imgui)   | MIT | `b2546a5c` (post-v1.92.8) |
| `implot/` | [epezent/implot](https://github.com/epezent/implot) | MIT | `1351ab2c` |

Pinned to the commits the project was developed against. Bumping
either is straightforward:

```powershell
cd third_party/imgui
git fetch
git checkout <new-commit>
cd ../..
git add third_party/imgui
git commit -m "Bump imgui to <new-commit>"
```

Just be aware that ImGui 1.92's dynamic-font-size `PushFont(font, size)`
API is a hard requirement and ImPlot's `ImPlotSpec` constructor pattern
is too — both pre-2024 releases will break the build.

## Why not bundled directly

Same logic as `vendor/btstack/`: keep the repo lean. Cloning the
project pulls them automatically via submodule, so there's no extra
manual step for end users.
