# GTA6-BootTest

Stand-in DLLs and a debugger harness that boot the Xbox Series X|S build of GTA VI (`game_scarlett_master.exe`, GDK) on Windows, as far as the point where real GPU work is needed.

**This repository contains no game code or data.** You need your own copy of the build. Nothing here is playable: the game boots, streams its archives, runs audio and its frame loop, and presents black frames, because the title's shaders are native RDNA2 code and no translator exists yet.

The technical findings are in [REPORT.md](REPORT.md).

## What it does

| Part | Role |
|---|---|
| `src/xmem.c` | Xbox memory API: placeholders, physical pages, commit-on-touch VEH, title allocation hooks. Also installs IAT hooks: `G:` package-root redirect, window made borderless fullscreen, `QueryApiImpl` proxies, Bink hooks. |
| `src/d3d12_xs.c` | Probe D3D12.X device: logging COM objects, creator-method table, resource memory, fences, `PresentX` -> GDI blit with a status line. |
| `src/XCRDAPI.c` + `src/inflate.h` | DirectStorage queue with synchronous reads and zlib. |
| `src/xg_xs.c` | Texture-computer probe. |
| `src/audio_stubs.c` | APU heap and shape contexts; no-op XApu/XDsp (silent). |
| `src/xgrproxy.h` | `xgameruntime` interface proxies and a local offline user (no Xbox Live). |
| `src/harness.c` | Debugger: loads, exceptions, debug output, breakpoints, assert skipping, `-cores=N`, hardware write-watchpoints (`GTA6_WATCH`). |
| `tools/*.py` | capstone helpers: function disassembly via `.pdata`, rip-relative and call xrefs, import-thunk resolution, log summaries. |

## Build and run

Needs Visual Studio 2022 (C toolset) and Gaming Services installed (for `xgameruntime.dll`).

```
src\build.bat
powershell -ExecutionPolicy Bypass -File go.ps1 -Game "X:\path\to\game_scarlett_master.exe" -Timeout 120 -Flags "-skip"
run.bat "X:\path\to\game_scarlett_master.exe"
```

Or set `GTA6_GAME_EXE` once instead of passing the path. Logs go to `logs\harness.log` and `logs\stubs.log`.

Environment switches: `GTA6_WINDOWED=1`, `GTA6_NO_XGR_PROXY=1`, `GTA6_NO_FAKE_USER=1`, `GTA6_RACE_GUARD=1`, `GTA6_WATCH=<regionSizeHex>:<offsetHex>`, `GTA6_ROOT=<package dir>`.

## Not done

- RDNA2 -> DXIL/SPIR-V shader translation and a real renderer
- DirectStorage formats `0x02` / `0x0E` (Series texture codecs)
- Saves, input mapping, audio output
- The submit race at `exe+0x24821FE`
