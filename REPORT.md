# game_scarlett_master.exe on Windows: boot findings

Build: `GTA6.Xbox.v1.0.2.1` (Scarlett / Xbox Series X|S GDK), `game_scarlett_master.exe`
SHA-256 `E95020B2FA4A721DB3DF59FDECF75621730AC559D8835252F6C3B658D5A08993`, 120 MB, PE32+ built 2026-11-13.
All offsets below are RVAs (`exe+0x...`) in that binary.

## Result

With stand-in DLLs, no patches to the exe on disk, and a small debugger harness, the exe on Windows 11 (Ryzen 7900X, RX 7800 XT):

- loads, initialises the CRT, memory, Xbox game runtime, audio and networking
- streams `shaders.rpf` and `core_bvh4.rpf` through an emulated DirectStorage (zlib verified)
- gets past sign-in with a local offline user (nothing is sent to Xbox Live)
- runs its full frame loop: about 100k command-list calls and 70k `ExecuteCommandLists` per 5 s, `PresentX` at about 600 fps against an instant fake GPU
- plays intro-video audio

**Nothing is drawn.** The title's shaders are native RDNA2 machine code (see the end of this report), so pixels need an RDNA2 -> DXIL/SPIR-V translator plus a real D3D12.X -> D3D12 renderer. This report covers everything up to that wall.

## Static imports that do not exist on Windows

`xmem.dll`, `d3d12_xs.dll`, `xg_xs.dll`, `XCRDAPI.dll`, `xapu.dll`, `AcpHal.dll`, `chad.dll`. Bundled DLLs that also need them: `Bink2Scarlett`, `XboxBvhBuilder`, `amd_fidelityfx_upscaler_xbox` (xmem, d3d12_xs). `xgameruntime.dll` is loaded dynamically, and the PC Gaming Services copy works.

## Package root

The title opens its package files as `G:\...` (for example `G:\appdata0.rpf`). Redirect `G:` to the package folder; we patch the exe IAT for the 11 file APIs it imports. A missing `appdata0.rpf` shows up as an empty-array assert at `exe+0x2237178`.

## xmem

| Function | What worked |
|---|---|
| `XMemVirtualAlloc(addr,size,type,a4,a5)` | type `0x40000` = reserve placeholder, `0x4000` = replace placeholder (VirtualAlloc2). Low byte of a5 = page protection. The title reserves 2 x 24 GB and uses it on demand, so commit-on-touch via a vectored exception handler (VEH) is needed. |
| `XMemAllocatePhysicalPages(flags,&count,pages)` | 64 KB page numbers; return TRUE, count unchanged |
| `XMemMapPhysicalPages(addr,count,pages)` | returns addr; pages==NULL means unmap |
| `XMemMapPhysicalPagesScatter(addrArray,count,pages)` | commit each address; must return nonzero (assert `exe+0xF2CC4B`) |
| `XMemAlloc/XMemFree` | must call the exe's exported `__XMemAllocationHooks__` {alloc `exe+0x4DEB420`, free `exe+0x4E38EC0`}. Hooks route on allocator id = attrs bits 16-23. The render-target budget (`exe+0x56A2A60`, assert `exe+0x247AF2D`) only fills through allocator `0x80` with `(attrs & 0xC000)==0x8000`; we use `0x808000` for D3D resources. |

## XCRDAPI (DirectStorage)

`DStorageGetFactory(riid, ppv)`. Factory: slot 4 `OpenFile(path,riid,ppv)`, 5 `CreateStatusArray(cap,name,riid,ppv)`, 8 `SetStagingBufferSize`, 10 `CreateQueue(desc,riid,ppQueue,?)`. Queue: 3 `EnqueueRequest`, 4 `EnqueueStatus`, 5 `EnqueueSignal`, 6 `Submit`. Status: 3 `IsComplete`, 4 `GetHResult`.

Request layout: `+00 options, +08 dest ptr, +18 dest size u32, +20 IDStorageFile*, +28 file offset u64, +30 src size u32`.

| options low byte | Meaning |
|---|---|
| `0` | raw |
| `1` | zlib (verified) |
| `2` and `0x0E` | Seen on `data/ttdb/*.ttpf`. Not zlib/deflate; probably the Series hardware codecs (BCPack). Undecoded. |

## d3d12_xs

`D3D12XboxCreateDevice(NULL, params, riid {5405C344...}, ppDevice)`. `D3D12SerializeVersionedRootSignature` can be forwarded to PC d3d12 after masking the Xbox flag bits `0x3000000` (flags `& 0xFFF`). Interface IDs are the public ones (queue `0EC870A6`, allocator `6102DEE4`, list `5B160D0F`, resource `696442BE`, heap `8EFB471D`, fence `0A753DCF`, PSO `765A30F3`, root sig `C54A6B66`, query heap `0D9658AE`, command signature `C36A797C`).

Device vtable: slots 0-43 match public `ID3D12Device`. Xbox additions:

| Slot | Meaning (riid argument index) |
|---|---|
| 48 | queue creator (1) |
| 55 | PSO creator `(?, ?, desc, riid, ppv)` (3); the title caches the result (`exe+0x2479F40`) |
| 57, 67, 86 | PSO creators `(desc, riid, ppv)` (1) |
| 62 | PSO creator `(desc, 0, 0, riid, ppv)` (3), used by XboxBvhBuilder |
| 70 | **not a creator**: 3 args, fills an out-struct |
| 73 | command-list creator (3) |
| 78, 79 | allocator creators (4) |
| 108 | `GetResourceAllocationInfo1` (struct return) |
| 127 | `CreateCommittedResource` variant, 10 args (8) |
| 133 | placed resource `(gpuAddress, desc, ?, ?, riid, ppv)` (4); arg0 is the memory |

- **Struct returns:** rcx = out buffer, rdx = this (for example `GetCustomHeapProperties`, `GetResourceAllocationInfo`).
- **Descriptor heaps:** the desc carries a title-chosen address at `+0x10`, and the heap start must equal it.
- **Sizes:** `GetResourceAllocationInfo` must use real format sizes and mip chains. Over-estimating fills the title's fixed GPU heaps and hits its OOM handler at `exe+0x2219620`.
- **Fences:** slot 12 returns the **address** of the completed value, which the title polls.
- **Queue:** slot 16 = `Signal(fence,value)`, 35 = `ExecuteCommandLists`, 32 = `PresentX(planeCount, planes, params)` (plane `+0x10` -> back-buffer resource array, two 3840x2160; params `+0x28` = rect), 31 = tile-mapping update.
- **DXGI chain:** `QI(IDXGIDevice)` -> `GetAdapter` (slot 7) -> `EnumOutputs(0)` (slot 7) -> output slot 21 mode list (the title looks for 120 Hz; 0 modes is accepted).
- **Object count:** the title creates more than 53k D3D objects during boot; size stand-ins accordingly.

## xg_xs, audio

- **xg_xs:** `XGCreateTextureComputer(desc, pp)` must return an object; a no-op one is enough to get past boot.
- **APU memory:** `ApuCreateHeap(cached, noncached)` must succeed (assert `exe+0x2425DF4`). `ApuAlloc(va*, pa*, size, align, flags)` uses 32-bit APU addresses.
- **Shape contexts:** `AcpHalAllocateShapeContexts(ctx)` reads six u32 counts at `+0x00..+0x14` and fills six array pointers at `+0x18..+0x40`; the title memsets `count*64` at pointer 4.
- **Bink:** `Bink2Scarlett` decodes on the GPU; the CPU-side buffers hold bitstream, not YUV.

## xgameruntime

The title calls `QueryApiImpl(prov, iface, ppv)` for each API.

- **XUser** `{01ACD177-91F9-4763...}` follows the public `XUser.h` order: 3 Duplicate, 4 Close, 5 Compare, 7 AddAsync, 8 AddResult, 9 GetLocalId, 10 FindByLocalId, 11 GetId, 12 FindById, 13 IsGuest, 14 GetState, 15 Gamertag, 19 AgeGroup, 20 CheckPrivilege, 23 token/signature.
- **Gamertag:** a second interface `{CEF4FAC0...}`, slot 3 = `GetGamertag(user, component, size, buf, &used)`.
- **Save provider:** `{1BFFF3AF...}` slot 4. Reporting failure is handled: the title continues without saves.
- **Store:** `{B09D803C...}`. We return "unavailable" and do not emulate licensing.
- **Proxy note:** interfaces have more than 64 methods; proxies need at least 128.

## Known problems

- **Submit race:** `exe+0x24821FE` dereferences a submit entry whose completion object is null. Triggered by the instant fake GPU, after 30-120 s.
- **Shaders:** the title's PSO descs (slot 67) point at native RDNA2 code (s_endpgm `0xBF810000`, SMEM `0xF4......` words; no DXBC/DXIL). Only XboxBvhBuilder's PSOs (slot 62) contain `DXBC`.

## Tooling

C stand-ins (MSVC), a debugger harness, and capstone-based scripts. The harness logs loads, exceptions and debug output; supports breakpoints with register and string dumps, assert skipping, CPU affinity, and hardware write-watchpoints armed from inside the process. Source is available on request.
