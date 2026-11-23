/* Real PC Direct3D 12 backend: device, queue and a flip-model swap chain on the game's window.
   Compiled into d3d12_xs.dll next to d3d12_xs.c (kept separate because the D3D12 headers clash with the stand-in exports). */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include "gpumem.h"

#define NBUF 3
static ID3D12Device* g_dev;
static ID3D12CommandQueue* g_q;
static IDXGISwapChain3* g_sc;
static ID3D12DescriptorHeap* g_rtvheap;
static UINT g_rtvinc;
static ID3D12Resource* g_bb[NBUF];
static ID3D12CommandAllocator* g_alloc[NBUF];
static ID3D12GraphicsCommandList* g_list;
static ID3D12Fence* g_fence;
static UINT64 g_fenceval[NBUF], g_fencenext = 1;
static HANDLE g_fev;
static int g_state;            /* 0 not tried, 1 ok, -1 failed */
static char g_err[200];

static void rlog(const char* fmt, ...) {
    char path[MAX_PATH], buf[600]; if (!GetEnvironmentVariableA("GTA6_STUB_LOG", path, MAX_PATH)) return;
    int n = snprintf(buf, 64, "%llu [realgpu] ", GetTickCount64());
    va_list ap; va_start(ap, fmt); n += vsnprintf(buf + n, sizeof buf - n - 3, fmt, ap); va_end(ap);
    if (n > (int)sizeof buf - 3) n = sizeof buf - 3; buf[n++] = '\r'; buf[n++] = '\n';
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, buf, n, &w, NULL); CloseHandle(h); }
}
#define CHK(x, what) do { HRESULT hr_ = (x); if (FAILED(hr_)) { snprintf(g_err, sizeof g_err, "%s failed 0x%08lX", what, hr_); rlog("%s", g_err); g_state = -1; return 0; } } while (0)

int rg_init(HWND hwnd) {
    if (g_state) return g_state == 1;
    if (!hwnd) return 0;
    IDXGIFactory4* fac = NULL;
    if (GetEnvironmentVariableA("GTA6_D3D_DEBUG", NULL, 0)) {          /* D3D12 debug layer (needs Windows "Graphics Tools") */
        ID3D12Debug* dbg = NULL;
        if (SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12Debug, (void**)&dbg))) { ID3D12Debug_EnableDebugLayer(dbg); ID3D12Debug_Release(dbg); rlog("D3D12 debug layer on"); }
        else rlog("D3D12 debug layer not available (install Graphics Tools)");
    }
    CHK(CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void**)&fac), "CreateDXGIFactory2");
    CHK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device, (void**)&g_dev), "D3D12CreateDevice");
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    CHK(ID3D12Device_CreateCommandQueue(g_dev, &qd, &IID_ID3D12CommandQueue, (void**)&g_q), "CreateCommandQueue");
    RECT rc; GetClientRect(hwnd, &rc);
    DXGI_SWAP_CHAIN_DESC1 sd = { 0 };
    sd.Width = rc.right ? rc.right : 1920; sd.Height = rc.bottom ? rc.bottom : 1080; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = NBUF; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* sc1 = NULL;
    CHK(IDXGIFactory4_CreateSwapChainForHwnd(fac, (IUnknown*)g_q, hwnd, &sd, NULL, NULL, &sc1), "CreateSwapChainForHwnd");
    CHK(IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void**)&g_sc), "QI IDXGISwapChain3");
    IDXGISwapChain1_Release(sc1);
    IDXGIFactory4_MakeWindowAssociation(fac, hwnd, DXGI_MWA_NO_ALT_ENTER);
    IDXGIFactory4_Release(fac);
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, NBUF };
    CHK(ID3D12Device_CreateDescriptorHeap(g_dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&g_rtvheap), "CreateDescriptorHeap");
    g_rtvinc = ID3D12Device_GetDescriptorHandleIncrementSize(g_dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE h; ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(g_rtvheap, &h);
    for (int i = 0; i < NBUF; i++) {
        CHK(IDXGISwapChain3_GetBuffer(g_sc, i, &IID_ID3D12Resource, (void**)&g_bb[i]), "GetBuffer");
        D3D12_CPU_DESCRIPTOR_HANDLE hi = { h.ptr + (SIZE_T)i * g_rtvinc };
        ID3D12Device_CreateRenderTargetView(g_dev, g_bb[i], NULL, hi);
        CHK(ID3D12Device_CreateCommandAllocator(g_dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void**)&g_alloc[i]), "CreateCommandAllocator");
    }
    CHK(ID3D12Device_CreateCommandList(g_dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], NULL, &IID_ID3D12GraphicsCommandList, (void**)&g_list), "CreateCommandList");
    ID3D12GraphicsCommandList_Close(g_list);
    CHK(ID3D12Device_CreateFence(g_dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&g_fence), "CreateFence");
    g_fev = CreateEventA(NULL, FALSE, FALSE, NULL);
    rlog("real D3D12 device + %ux%u swap chain on window %p", sd.Width, sd.Height, hwnd);
    g_state = 1;
    return 1;
}

/* ---- GPU memory pool (gpumem.h): the game's physical memory, read by the GPU in place ----
   Each 2 GB section of xmem's pool is opened as a heap with one raw buffer + SRV at descriptor index = section.
   The page table (dir + 64K-entry chunks) lives in an upload buffer, refreshed from xmem's table every frame. */
#define PT_SLOTS 64
static GpuMem* g_gmem; static int g_gmtried;
static ID3D12DescriptorHeap* g_srvheap; static UINT g_srvinc;
static ID3D12Resource* g_secbuf[GM_MAXSEC]; static int g_nsecbuf;
static ID3D12Resource* g_ptab; static uint32_t* g_ptabmap;
static int g_slot[65536]; static int g_nslot; static long g_seen[65536];
static volatile LONG g_secvis;           /* sections whose heap + SRV exist (published by the opener thread) */
static volatile LONG g_opener;
/* Opening a 2 GB section as a heap takes ~0.3 s; doing it inside PresentX stalls the game's frame and exposes a
   submit/completion race in the title (exe+0x24821FE), so sections are opened on a worker thread. */
static DWORD WINAPI gm_opener(LPVOID unused) {
    (void)unused;
    while (g_gmem && g_nsecbuf < g_gmem->nsec) {
        int s = g_nsecbuf; ID3D12Device3* d3 = NULL; ID3D12Heap* heap = NULL;
        if (FAILED(ID3D12Device_QueryInterface(g_dev, &IID_ID3D12Device3, (void**)&d3))) break;
        /* NOT OpenExistingHeapFromFileMapping: on this driver it leaks the whole section's commit permanently (until reboot),
           even after full teardown. A private full view of the section + OpenExistingHeapFromAddress is released cleanly,
           also when the process is killed (tools/heaptest3.c). */
        void* view = MapViewOfFile((HANDLE)g_gmem->sec[s], FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)GM_SECPAGES * GM_PAGE);
        if (!view) { rlog("gpumem: view of section %d failed %lu", s, GetLastError()); ID3D12Device3_Release(d3); break; }
        HRESULT hr = ID3D12Device3_OpenExistingHeapFromAddress(d3, view, &IID_ID3D12Heap, (void**)&heap);
        ID3D12Device3_Release(d3);
        if (FAILED(hr)) { rlog("gpumem: OpenExistingHeapFromAddress(section %d view) failed 0x%08lX", s, hr); break; }
        D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, (UINT64)GM_SECPAGES * GM_PAGE, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER };
        hr = ID3D12Device_CreatePlacedResource(g_dev, heap, 0, &rd, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void**)&g_secbuf[s]);
        if (FAILED(hr)) { rlog("gpumem: section %d buffer failed 0x%08lX", s, hr); break; }
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = { DXGI_FORMAT_R32_TYPELESS, D3D12_SRV_DIMENSION_BUFFER, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING };
        sv.Buffer.NumElements = (UINT)(rd.Width / 4); sv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        D3D12_CPU_DESCRIPTOR_HANDLE h; ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(g_srvheap, &h); h.ptr += (SIZE_T)s * g_srvinc;
        ID3D12Device_CreateShaderResourceView(g_dev, g_secbuf[s], &sv, h);
        g_nsecbuf++; InterlockedExchange(&g_secvis, g_nsecbuf);
        for (int d = 0; d < 65536; d++) g_seen[d] = -1;           /* republish chunks: entries of this section become visible */
        rlog("gpumem: section %d (2 GB) visible to the GPU", s);
    }
    InterlockedExchange(&g_opener, 0);
    return 0;
}
static void gm_sync(void) {
    if (!g_gmtried) {
        g_gmtried = 1;
        HMODULE x = GetModuleHandleA("xmem.dll"); typedef void* (*StFn)(void);
        StFn f = x ? (StFn)GetProcAddress(x, "xmem_gpu_state") : NULL; g_gmem = f ? (GpuMem*)f() : NULL;
        D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1024, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
        if (FAILED(ID3D12Device_CreateDescriptorHeap(g_dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&g_srvheap))) { rlog("gpumem: descriptor heap failed"); return; }
        g_srvinc = ID3D12Device_GetDescriptorHandleIncrementSize(g_dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, (65536ull + PT_SLOTS * 65536ull) * 4, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR };
        if (FAILED(ID3D12Device_CreateCommittedResource(g_dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&g_ptab))) { rlog("gpumem: page table buffer failed"); return; }
        ID3D12Resource_Map(g_ptab, 0, NULL, (void**)&g_ptabmap); memset(g_ptabmap, 0, (size_t)rd.Width);
        rlog("gpumem: %s", g_gmem ? "pool found in xmem, GPU reads game memory in place" : "no pool (set GTA6_GPUMEM=1): translated shaders read zeros");
    }
    if (!g_gmem || !g_ptabmap) return;
    if (g_nsecbuf < g_gmem->nsec && !InterlockedCompareExchange(&g_opener, 1, 0)) {
        HANDLE th = CreateThread(NULL, 0, gm_opener, NULL, 0, NULL); if (th) CloseHandle(th); else g_opener = 0;
    }
    for (int d = 1; d < 65536; d++) {                                     /* chunk 0 (below 4 GB) is built below, with the aliases */
        uint32_t* c = g_gmem->dir[d]; long g = g_gmem->chunkgen[d];
        if (!c || g == g_seen[d]) continue;
        if (!g_slot[d]) { if (g_nslot >= PT_SLOTS) { static int w; if (!w++) rlog("gpumem: page table full"); continue; } g_slot[d] = ++g_nslot; }
        uint32_t* dst = g_ptabmap + 65536 + (size_t)(g_slot[d] - 1) * 65536; uint32_t vis = (uint32_t)g_secvis;
        if (vis >= (uint32_t)g_gmem->nsec) memcpy(dst, c, 65536 * 4);
        else for (int k = 0; k < 65536; k++) { uint32_t e = c[k]; dst[k] = (e && (e >> 24) - 1 < vis) ? e : 0; }   /* hide pages of sections not opened yet */
        g_ptabmap[d] = (uint32_t)g_slot[d]; g_seen[d] = g;
    }
    /* chunk 0: low GPU aliases of descriptor heaps -> the pages backing their CPU memory (rebuilt every frame: cheap) */
    if (g_gmem->nalias || g_gmem->dir[0]) {
        if (!g_slot[0] && g_nslot < PT_SLOTS) g_slot[0] = ++g_nslot;
        if (g_slot[0]) {
            uint32_t* dst = g_ptabmap + 65536 + (size_t)(g_slot[0] - 1) * 65536; uint32_t vis = (uint32_t)g_secvis;
            static uint32_t tmp[65536]; if (g_gmem->dir[0]) memcpy(tmp, g_gmem->dir[0], sizeof tmp); else memset(tmp, 0, sizeof tmp);
            static int missing_logged;
            for (long i = 0; i < g_gmem->nalias; i++) {
                uint64_t gpu = g_gmem->alias[i].gpu & ~0xFFFFull, cpu = g_gmem->alias[i].cpu & ~0xFFFFull, end = g_gmem->alias[i].gpu + g_gmem->alias[i].size;
                for (; gpu < end; gpu += GM_PAGE, cpu += GM_PAGE) {
                    uint32_t* c = g_gmem->dir[(cpu >> 32) & 0xFFFF]; uint32_t e = c ? c[(cpu >> 16) & 0xFFFF] : 0;
                    if (!e && missing_logged < 4) { missing_logged++; rlog("gpumem: descriptor heap page cpu %llX is not pool memory (GPU sees zeros there)", cpu); }
                    tmp[(gpu >> 16) & 0xFFFF] = e;
                }
            }
            for (int k = 0; k < 65536; k++) { uint32_t e = tmp[k]; dst[k] = (e && (e >> 24) - 1 < vis) ? e : 0; }
            g_ptabmap[0] = (uint32_t)g_slot[0];
        }
    }
}
/* GPU read of 16 addresses through the page table (tools/gmtest.hlsl, path in GTA6_GMTEST_CSO); 1 = ok */
static int gm_gpuread(const uint64_t addr[16], uint32_t out[16]) {
    static ID3D12RootSignature* rs; static ID3D12PipelineState* pso; static ID3D12Resource *ob, *rb; static int failed;
    if (failed) return 0;
    if (!pso) {
        char cso[MAX_PATH]; if (!GetEnvironmentVariableA("GTA6_GMTEST_CSO", cso, MAX_PATH)) { failed = 1; rlog("gpumem self-test: GTA6_GMTEST_CSO not set, skipped"); return 0; }
        HANDLE f = CreateFileA(cso, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL); if (f == INVALID_HANDLE_VALUE) { failed = 1; rlog("gpumem self-test: %s missing", cso); return 0; }
        DWORD sz = GetFileSize(f, NULL), rdn = 0; void* code = malloc(sz); ReadFile(f, code, sz, &rdn, NULL); CloseHandle(f);
        D3D12_ROOT_PARAMETER1 rp[3] = { 0 };
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[0].Constants.Num32BitValues = 32;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC vd = { D3D_ROOT_SIGNATURE_VERSION_1_1 };
        vd.Desc_1_1.NumParameters = 3; vd.Desc_1_1.pParameters = rp; vd.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
        ID3DBlob *blob = NULL, *err = NULL;
        if (FAILED(D3D12SerializeVersionedRootSignature(&vd, &blob, &err)) || FAILED(ID3D12Device_CreateRootSignature(g_dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void**)&rs))) { failed = 1; rlog("gpumem self-test: root signature failed"); free(code); return 0; }
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd = { rs, { code, sz } };
        HRESULT hr = ID3D12Device_CreateComputePipelineState(g_dev, &cd, &IID_ID3D12PipelineState, (void**)&pso); free(code);
        if (FAILED(hr)) { failed = 1; rlog("gpumem self-test: pipeline failed 0x%08lX", hr); return 0; }
        D3D12_HEAP_PROPERTIES dh = { D3D12_HEAP_TYPE_DEFAULT }, rbh = { D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, 256, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS };
        ID3D12Device_CreateCommittedResource(g_dev, &dh, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource, (void**)&ob);
        rd.Flags = D3D12_RESOURCE_FLAG_NONE;
        ID3D12Device_CreateCommittedResource(g_dev, &rbh, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&rb);
        if (!ob || !rb) { failed = 1; rlog("gpumem self-test: buffers failed"); return 0; }
    }
    ID3D12CommandAllocator* al = NULL; ID3D12GraphicsCommandList* cl = NULL;
    ID3D12Device_CreateCommandAllocator(g_dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void**)&al);
    ID3D12Device_CreateCommandList(g_dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, pso, &IID_ID3D12GraphicsCommandList, (void**)&cl);
    if (!cl) return 0;
    ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 1, &g_srvheap);
    ID3D12GraphicsCommandList_SetComputeRootSignature(cl, rs);
    uint32_t c32[32]; for (int i = 0; i < 16; i++) { c32[2 * i] = (uint32_t)addr[i]; c32[2 * i + 1] = (uint32_t)(addr[i] >> 32); }
    ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(cl, 0, 32, c32, 0);
    ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(cl, 1, ID3D12Resource_GetGPUVirtualAddress(g_ptab));
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(cl, 2, ID3D12Resource_GetGPUVirtualAddress(ob));
    ID3D12GraphicsCommandList_Dispatch(cl, 1, 1, 1);
    D3D12_RESOURCE_BARRIER b = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION }; b.Transition.pResource = ob; b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
    ID3D12GraphicsCommandList_CopyBufferRegion(cl, rb, 0, ob, 0, 64);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
    ID3D12GraphicsCommandList_Close(cl);
    ID3D12CommandQueue_ExecuteCommandLists(g_q, 1, (ID3D12CommandList**)&cl);
    UINT64 v = g_fencenext++; ID3D12CommandQueue_Signal(g_q, g_fence, v); ID3D12Fence_SetEventOnCompletion(g_fence, v, g_fev); WaitForSingleObject(g_fev, 3000);
    ID3D12GraphicsCommandList_Release(cl); ID3D12CommandAllocator_Release(al);
    uint32_t* m = NULL; D3D12_RANGE r = { 0, 64 }; int ok = 0;
    if (SUCCEEDED(ID3D12Resource_Map(rb, 0, &r, (void**)&m)) && m) { memcpy(out, m, 64); D3D12_RANGE z = { 0, 0 }; ID3D12Resource_Unmap(rb, 0, &z); ok = 1; }
    else rlog("gpumem self-test: readback failed, device state 0x%08lX", ID3D12Device_GetDeviceRemovedReason(g_dev));
    return ok;
}
static void gm_report(const char* what, const uint64_t addr[16], const uint64_t cpuaddr[16]) {
    uint32_t cpu[16], gpu[16]; int bad = 0;
    for (int i = 0; i < 16; i++) cpu[i] = *(volatile uint32_t*)cpuaddr[i];
    if (!gm_gpuread(addr, gpu)) return;
    for (int i = 0; i < 16; i++) { if (gpu[i] != cpu[i]) bad++; if (i < 3 || gpu[i] != cpu[i]) rlog("%s: gpu addr %llX (cpu %llX)  cpu %08X  gpu %08X", what, addr[i], cpuaddr[i], cpu[i], gpu[i]); }
    rlog("%s: %s (%d of 16 differ; memory can change between the CPU and GPU reads)", what, bad ? "MISMATCH" : "OK", bad);
}
/* one-time checks: 16 non-zero words of pool memory, then 16 of a descriptor heap through its low GPU alias */
static void gm_selftest(void) {
    static int done, done2, tries2;
    if (!g_gmem || !g_gmem->nsec || g_secvis < g_gmem->nsec) return;
    if (!done) {
        done = 1; uint64_t addr[16]; int n = 0;
        for (int d = 1; d < 65536 && n < 16; d++) { uint32_t* c = g_gmem->dir[d]; if (!c) continue;
            for (int i = 0; i < 65536 && n < 16; i += 7) if (c[i] && (c[i] >> 24) - 1 < (uint32_t)g_secvis) {
                uint64_t base = ((uint64_t)d << 32) | ((uint64_t)i << 16);
                for (uint32_t o = 0; o < 0x10000; o += 0x40) if (*(volatile uint32_t*)(base + o)) { addr[n++] = base + o; break; } } }
        if (n == 16) gm_report("gpumem self-test (pool)", addr, addr); else rlog("gpumem self-test: only %d mapped pages found, skipped", n);
    }
    if (!done2 && g_gmem->nalias && (++tries2 % 60) == 0) {          /* heaps fill up over time: retry about once a second */
        uint64_t gpu[16], cpu[16]; int n = 0;
        uint64_t g0 = g_gmem->alias[0].gpu, c0 = g_gmem->alias[0].cpu, sz = g_gmem->alias[0].size;
        for (uint64_t o = 0; o + 4 <= sz && n < 16; o += 0x1000 + 4 * (uint64_t)n) {
            uint32_t* ce = g_gmem->dir[((c0 + o) >> 32) & 0xFFFF]; if (!ce || !ce[((c0 + o) >> 16) & 0xFFFF]) continue;
            if (*(volatile uint32_t*)(c0 + o)) { gpu[n] = g0 + o; cpu[n] = c0 + o; n++; }
        }
        if (n == 16) { done2 = 1; gm_report("gpumem self-test (descriptor heap via low GPU alias)", gpu, cpu); }
        else if (tries2 >= 60 * 60) { done2 = 1; rlog("gpumem alias test: heap 0 has only %d non-zero pool words after 60 s", n); }
    }
}

/* ---- DXIL draws: the game's own portable shaders run on the real GPU ----
   Root signature: param 0 = root SRV t0 space3 (vertex shader). Draws are queued per frame and run after the clear. */
typedef struct { const void* vs; const void* ps; ID3D12PipelineState* pso; } PsoCache;
static PsoCache g_psos[64]; static int g_npsos;
static ID3D12RootSignature* g_rs;
typedef struct { ID3D12PipelineState* pso; uint8_t data[4096]; uint32_t datalen; uint32_t verts, insts; float vp[6]; } QDraw;
static QDraw* g_q2; static int g_nq2, g_capq2;
static ID3D12Resource* g_upload[NBUF]; static uint8_t* g_upmap[NBUF];
#define UPLOAD_SIZE (16u << 20)

static ID3D12PipelineState* get_pso(const void* vs, size_t vslen, const void* ps, size_t pslen) {
    for (int i = 0; i < g_npsos; i++) if (g_psos[i].vs == vs && g_psos[i].ps == ps) return g_psos[i].pso;
    if (!g_rs) {
        D3D12_ROOT_PARAMETER p = { D3D12_ROOT_PARAMETER_TYPE_SRV }; p.Descriptor.ShaderRegister = 0; p.Descriptor.RegisterSpace = 3; p.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        D3D12_ROOT_SIGNATURE_DESC rd = { 1, &p, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ID3DBlob *blob = NULL, *err = NULL;
        HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
        if (FAILED(hr)) { rlog("serialize root signature failed 0x%08lX", hr); return NULL; }
        hr = ID3D12Device_CreateRootSignature(g_dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void**)&g_rs);
        ID3D10Blob_Release(blob);
        if (FAILED(hr)) { rlog("CreateRootSignature failed 0x%08lX", hr); return NULL; }
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = { 0 };
    d.pRootSignature = g_rs;
    d.VS.pShaderBytecode = vs; d.VS.BytecodeLength = vslen; d.PS.pShaderBytecode = ps; d.PS.BytecodeLength = pslen;
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = 0xFFFFFFFF;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; d.RasterizerState.DepthClipEnable = TRUE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1; d.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = NULL;
    HRESULT before = ID3D12Device_GetDeviceRemovedReason(g_dev);
    HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(g_dev, &d, &IID_ID3D12PipelineState, (void**)&pso);
    rlog("CreateGraphicsPipelineState from the game's DXIL (vs %zu bytes, ps %zu bytes): %s 0x%08lX (device state before 0x%08lX, after 0x%08lX)",
        vslen, pslen, SUCCEEDED(hr) ? "OK" : "FAILED", hr, before, ID3D12Device_GetDeviceRemovedReason(g_dev));
    {   ID3D12InfoQueue* iq = NULL;                                         /* debug-layer messages, if any */
        if (SUCCEEDED(ID3D12Device_QueryInterface(g_dev, &IID_ID3D12InfoQueue, (void**)&iq))) {
            UINT64 n = ID3D12InfoQueue_GetNumStoredMessages(iq);
            for (UINT64 k = 0; k < n && k < 10; k++) { SIZE_T len = 0; ID3D12InfoQueue_GetMessage(iq, k, NULL, &len);
                D3D12_MESSAGE* msg = (D3D12_MESSAGE*)malloc(len); if (msg && SUCCEEDED(ID3D12InfoQueue_GetMessage(iq, k, msg, &len))) rlog("d3d12 debug: %.400s", msg->pDescription); free(msg); }
            ID3D12InfoQueue_Release(iq);
        } }
    if (g_npsos < 64) { g_psos[g_npsos].vs = vs; g_psos[g_npsos].ps = ps; g_psos[g_npsos].pso = pso; g_npsos++; }
    return pso;
}
/* queue a draw of a DXIL pipeline for the current frame; srv = bytes of the vertex shader's t0 buffer; vp = game viewport */
void rg_queue_dxil_draw(const void* vs, size_t vslen, const void* ps, size_t pslen, const void* srv, uint32_t srvlen, uint32_t verts, uint32_t insts, const float* vp) {
    if (g_state != 1) return;
    ID3D12PipelineState* pso = get_pso(vs, vslen, ps, pslen); if (!pso) return;
    if (g_nq2 == g_capq2) { int nc = g_capq2 ? g_capq2 * 2 : 64; QDraw* n = (QDraw*)realloc(g_q2, sizeof(QDraw) * nc); if (!n) return; g_q2 = n; g_capq2 = nc; }
    QDraw* q = &g_q2[g_nq2++]; q->pso = pso; q->datalen = srvlen > sizeof q->data ? sizeof q->data : srvlen;
    memcpy(q->data, srv, q->datalen); q->verts = verts; q->insts = insts; memcpy(q->vp, vp, sizeof q->vp);
}
static void run_queued(UINT bi, D3D12_CPU_DESCRIPTOR_HANDLE rtv, float sx, float sy) {
    if (!g_nq2) return;
    if (!g_upload[bi]) {
        D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, UPLOAD_SIZE, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR };
        if (FAILED(ID3D12Device_CreateCommittedResource(g_dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&g_upload[bi]))) return;
        ID3D12Resource_Map(g_upload[bi], 0, NULL, (void**)&g_upmap[bi]);
    }
    D3D12_GPU_VIRTUAL_ADDRESS base = ID3D12Resource_GetGPUVirtualAddress(g_upload[bi]); size_t off = 0;
    ID3D12GraphicsCommandList_OMSetRenderTargets(g_list, 1, &rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(g_list, g_rs);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(g_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (int i = 0; i < g_nq2; i++) {
        QDraw* q = &g_q2[i];
        if (off + q->datalen + 256 > UPLOAD_SIZE) break;
        memcpy(g_upmap[bi] + off, q->data, q->datalen);
        D3D12_VIEWPORT v = { q->vp[0] * sx, q->vp[1] * sy, q->vp[2] * sx, q->vp[3] * sy, q->vp[4], q->vp[5] };
        D3D12_RECT sc = { (LONG)v.TopLeftX, (LONG)v.TopLeftY, (LONG)(v.TopLeftX + v.Width), (LONG)(v.TopLeftY + v.Height) };
        ID3D12GraphicsCommandList_RSSetViewports(g_list, 1, &v); ID3D12GraphicsCommandList_RSSetScissorRects(g_list, 1, &sc);
        ID3D12GraphicsCommandList_SetPipelineState(g_list, q->pso);
        ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(g_list, 0, base + off);
        ID3D12GraphicsCommandList_DrawInstanced(g_list, q->verts, q->insts ? q->insts : 1, 0, 0);
        off = (off + q->datalen + 255) & ~(size_t)255;
    }
    static int logged; if (logged++ < 3) rlog("frame ran %d DXIL draws on the real GPU", g_nq2);
    g_nq2 = 0;
}

/* ---- translated shaders (RDNA2 -> HLSL -> DXIL by tools/xlat_all.py), loaded from GTA6_XLAT_DIR by hash ----
   Root signature: param0 = root CBV b0 for the VS, param1 = root CBV b1 for the PS (each: 32 user SGPRs, draw block,
   8 texture bindings), param2 = root SRV t0 (pool page table); static sampler s0 (linear clamp).
   Pool sections and cached render targets are reached through ResourceDescriptorHeap. */
#define XPSO_MAX 8192
#define STAGE_CB 512                                  /* bytes per stage block in the upload ring */
typedef struct { uint64_t vh, ph; int topo; DXGI_FORMAT rtf; ID3D12PipelineState* pso; } XPso;
static XPso g_xpsos[XPSO_MAX]; static int g_nxpsos;
static ID3D12RootSignature* g_xrs;
/* ---- render target cache: game render targets (by address) -> real D3D12 textures with RTV + bindless SRV ---- */
#define RT_MAX 256
#define RT_SRV0 256                                   /* descriptor index of cache entry 0 (0..255 = pool sections) */
typedef struct { uint64_t addr; UINT w, h; DXGI_FORMAT fmt; ID3D12Resource* tex; D3D12_RESOURCE_STATES st; int drawn; } RtEnt;
static RtEnt g_rt[RT_MAX]; static volatile LONG g_nrt; static SRWLOCK g_rtlk = SRWLOCK_INIT;
static ID3D12DescriptorHeap* g_rtvh; static UINT g_rtvhinc;
static DXGI_FORMAT rt_typed(DXGI_FORMAT f) {          /* render-target-capable variant of typeless formats */
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_UNORM;
    case DXGI_FORMAT_R8G8_TYPELESS: return DXGI_FORMAT_R8G8_UNORM;
    default: return f;
    }
}
/* find or create the cached texture for a game render target; -1 if the format can't be a colour target */
int rg_rt(uint64_t addr, UINT w, UINT h, UINT fmt) {
    if (g_state != 1 || !addr || !w || !h || w > 16384 || h > 16384) return -1;
    DXGI_FORMAT f = rt_typed((DXGI_FORMAT)fmt);
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = { f };
    for (int i = 0; i < g_nrt; i++) if (g_rt[i].addr == addr && g_rt[i].w == w && g_rt[i].h == h && g_rt[i].fmt == f) return i;
    if (FAILED(ID3D12Device_CheckFeatureSupport(g_dev, D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs)) || !(fs.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET)) {
        static int nl; if (nl++ < 10) rlog("rt cache: format %u (%u) is not a colour render target, skipped", fmt, f); return -1; }
    AcquireSRWLockExclusive(&g_rtlk);
    int idx = -1;
    for (int i = 0; i < g_nrt; i++) if (g_rt[i].addr == addr && g_rt[i].w == w && g_rt[i].h == h && g_rt[i].fmt == f) { idx = i; break; }
    if (idx < 0 && g_nrt < RT_MAX && g_srvheap) {
        if (!g_rtvh) { D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, RT_MAX };
            ID3D12Device_CreateDescriptorHeap(g_dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&g_rtvh); g_rtvhinc = ID3D12Device_GetDescriptorHandleIncrementSize(g_dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV); }
        D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, 1, 1, f, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
        D3D12_CLEAR_VALUE cv = { f, { 0, 0, 0, 0 } };
        ID3D12Resource* tex = NULL;
        HRESULT hr = ID3D12Device_CreateCommittedResource(g_dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, &IID_ID3D12Resource, (void**)&tex);
        if (SUCCEEDED(hr) && g_rtvh) {
            int i = g_nrt; RtEnt* e = &g_rt[i]; e->addr = addr; e->w = w; e->h = h; e->fmt = f; e->tex = tex; e->st = D3D12_RESOURCE_STATE_RENDER_TARGET; e->drawn = 0;
            D3D12_CPU_DESCRIPTOR_HANDLE rh; ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(g_rtvh, &rh); rh.ptr += (SIZE_T)i * g_rtvhinc;
            ID3D12Device_CreateRenderTargetView(g_dev, tex, NULL, rh);
            D3D12_CPU_DESCRIPTOR_HANDLE sh; ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(g_srvheap, &sh); sh.ptr += (SIZE_T)(RT_SRV0 + i) * g_srvinc;
            ID3D12Device_CreateShaderResourceView(g_dev, tex, NULL, sh);
            InterlockedIncrement(&g_nrt); idx = i;
            rlog("rt cache #%d: game render target %llX %ux%u format %u", i, addr, w, h, f);
        } else rlog("rt cache: texture %ux%u format %u failed 0x%08lX", w, h, f, hr);
    }
    ReleaseSRWLockExclusive(&g_rtlk);
    return idx;
}
/* resolve a texture address the game samples: cached render target -> descriptor index, else -1 */
int rg_rt_srv(uint64_t addr) { for (int i = 0; i < g_nrt; i++) if (g_rt[i].addr == addr) return RT_SRV0 + i; return -1; }
static D3D12_CPU_DESCRIPTOR_HANDLE rt_rtv(int i) { D3D12_CPU_DESCRIPTOR_HANDLE h; ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(g_rtvh, &h); h.ptr += (SIZE_T)i * g_rtvhinc; return h; }
static void rt_state(int i, D3D12_RESOURCE_STATES want) {
    if (i < 0 || i >= g_nrt || g_rt[i].st == want) return;
    D3D12_RESOURCE_BARRIER b = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION }; b.Transition.pResource = g_rt[i].tex; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = g_rt[i].st; b.Transition.StateAfter = want; ID3D12GraphicsCommandList_ResourceBarrier(g_list, 1, &b); g_rt[i].st = want;
}
/* ordered per-frame operations: draws and clears, in the game's order */
typedef struct { int kind; ID3D12PipelineState* pso; uint32_t vsc[68], psc[68]; uint32_t verts, insts; int prim; float vp[6]; int rt; float clear[4]; } XDraw;
static XDraw* g_xq; static int g_nxq, g_capxq; static SRWLOCK g_xqlk = SRWLOCK_INIT;
static void* load_file(const char* path, size_t* len) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL); if (f == INVALID_HANDLE_VALUE) return NULL;
    DWORD sz = GetFileSize(f, NULL), rd = 0; void* p = malloc(sz ? sz : 1); if (p && !ReadFile(f, p, sz, &rd, NULL)) { free(p); p = NULL; }
    CloseHandle(f); *len = rd; return p;
}
/* shader pairs that have no translation yet go to <logs>\xlat_needed.txt (one "vs ps" line each), for tools/xlat_needed.py */
static void note_needed(uint64_t vh, uint64_t ph) {
    static uint64_t seen[4096][2]; static int n;
    for (int i = 0; i < n; i++) if (seen[i][0] == vh && seen[i][1] == ph) return;
    if (n < 4096) { seen[n][0] = vh; seen[n][1] = ph; n++; }
    char path[MAX_PATH]; if (!GetEnvironmentVariableA("GTA6_STUB_LOG", path, MAX_PATH)) return;
    char* sl = strrchr(path, '\\'); if (!sl) return; snprintf(sl + 1, MAX_PATH - (sl + 1 - path), "xlat_needed.txt");
    HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) { char l[64]; int k = snprintf(l, sizeof l, "%016llX %016llX\r\n", vh, ph); DWORD w; WriteFile(f, l, k, &w, NULL); CloseHandle(f); }
}
static int make_xrs(void) {
    if (g_xrs) return 1;
    D3D12_ROOT_PARAMETER1 p[3] = { 0 };
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; p[0].Descriptor.ShaderRegister = 0; p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; p[1].Descriptor.ShaderRegister = 1; p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    p[0].Descriptor.Flags = p[1].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; p[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC ss = { D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        0, 1, D3D12_COMPARISON_FUNC_NEVER, D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK, 0, D3D12_FLOAT32_MAX, 0, 0, D3D12_SHADER_VISIBILITY_ALL };
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rd = { D3D_ROOT_SIGNATURE_VERSION_1_1 };
    rd.Desc_1_1.NumParameters = 3; rd.Desc_1_1.pParameters = p; rd.Desc_1_1.NumStaticSamplers = 1; rd.Desc_1_1.pStaticSamplers = &ss;
    rd.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
    ID3DBlob *blob = NULL, *err = NULL;
    if (FAILED(D3D12SerializeVersionedRootSignature(&rd, &blob, &err))) { rlog("translated root signature serialize failed: %s", err ? (char*)ID3D10Blob_GetBufferPointer(err) : "?"); return 0; }
    HRESULT hr = ID3D12Device_CreateRootSignature(g_dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void**)&g_xrs);
    ID3D10Blob_Release(blob); if (FAILED(hr)) { rlog("translated root signature failed 0x%08lX", hr); return 0; }
    return 1;
}
/* topo: D3D_PRIMITIVE_TOPOLOGY value; rtf: render target format (back buffer when drawing without the cache) */
static ID3D12PipelineState* get_xpso(uint64_t vh, uint64_t ph, int topo, DXGI_FORMAT rtf) {
    int tt = (topo == 1) ? 1 : (topo == 2 || topo == 3) ? 2 : 3;      /* point, line, triangle */
    for (int i = 0; i < g_nxpsos; i++) if (g_xpsos[i].vh == vh && g_xpsos[i].ph == ph && g_xpsos[i].topo == tt && g_xpsos[i].rtf == rtf) return g_xpsos[i].pso;
    if (g_nxpsos >= XPSO_MAX) return NULL;
    XPso* e = &g_xpsos[g_nxpsos++]; e->vh = vh; e->ph = ph; e->topo = tt; e->rtf = rtf; e->pso = NULL;
    char dir[MAX_PATH]; if (!GetEnvironmentVariableA("GTA6_XLAT_DIR", dir, MAX_PATH) || !make_xrs()) return NULL;
    char pv[MAX_PATH], pp[MAX_PATH]; size_t lv = 0, lp = 0;
    snprintf(pv, sizeof pv, "%s\\%016llX.vs.cso", dir, vh); snprintf(pp, sizeof pp, "%s\\%016llX.ps.cso", dir, ph);
    void* vs = load_file(pv, &lv); void* ps = load_file(pp, &lp);
    if (!vs || !ps) { note_needed(vh, ph); free(vs); free(ps); return NULL; }
    {   char solid[MAX_PATH]; size_t ls = 0; void* sp;      /* GTA6_XLAT_SOLID=<ps_solid.cso>: geometry debug colours instead of the game's PS */
        if (GetEnvironmentVariableA("GTA6_XLAT_SOLID", solid, MAX_PATH) && (sp = load_file(solid, &ls)) != NULL) { free(ps); ps = sp; lp = ls; } }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = { 0 };
    d.pRootSignature = g_xrs; d.VS.pShaderBytecode = vs; d.VS.BytecodeLength = lv; d.PS.pShaderBytecode = ps; d.PS.BytecodeLength = lp;
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL; d.SampleMask = 0xFFFFFFFF;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; d.RasterizerState.DepthClipEnable = TRUE;
    d.PrimitiveTopologyType = tt == 1 ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT : tt == 2 ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1; d.RTVFormats[0] = rtf; d.SampleDesc.Count = 1;
    HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(g_dev, &d, &IID_ID3D12PipelineState, (void**)&e->pso);
    static int nl; if (nl++ < 40 || FAILED(hr)) rlog("pipeline from TRANSLATED shaders vs %016llX + ps %016llX (rt format %u): %s 0x%08lX", vh, ph, rtf, SUCCEEDED(hr) ? "OK" : "FAILED", hr);
    free(vs); free(ps);
    return e->pso;
}
static XDraw* xq_add(void) {
    if (g_nxq == g_capxq) { int nc = g_capxq ? g_capxq * 2 : 256; XDraw* n = (XDraw*)realloc(g_xq, sizeof(XDraw) * nc); if (!n) return NULL; g_xq = n; g_capxq = nc; }
    return &g_xq[g_nxq++];
}
/* vsc/psc: 68 constants each (32 user SGPRs, index buffer lo/hi + flags + spare, 8 x texture binding {addr lo, addr hi, srv, 0});
   rt: render target cache index (-1 = draw onto the back buffer, scaled from 3840x2160) */
void rg_queue_xlat_draw(uint64_t vh, uint64_t ph, const uint32_t* vsc, const uint32_t* psc, uint32_t verts, uint32_t insts, int prim, const float* vp, int rt) {
    if (g_state != 1) return;
    static int maxd = -1; if (maxd < 0) { char b[16]; maxd = GetEnvironmentVariableA("GTA6_XLAT_MAXDRAWS", b, sizeof b) ? atoi(b) : 4000; }
    if (g_nxq >= maxd) return;
    DXGI_FORMAT rtf = (rt >= 0 && rt < g_nrt) ? g_rt[rt].fmt : DXGI_FORMAT_B8G8R8A8_UNORM;
    ID3D12PipelineState* pso = get_xpso(vh, ph, prim, rtf); if (!pso) return;
    AcquireSRWLockExclusive(&g_xqlk);
    XDraw* q = xq_add();
    if (q) { q->kind = 0; q->pso = pso; memcpy(q->vsc, vsc, sizeof q->vsc); memcpy(q->psc, psc, sizeof q->psc);
             q->verts = verts; q->insts = insts; q->prim = prim; memcpy(q->vp, vp, sizeof q->vp); q->rt = rt; }
    ReleaseSRWLockExclusive(&g_xqlk);
}
void rg_queue_clear(int rt, const float* rgba) {
    if (g_state != 1 || rt < 0) return;
    AcquireSRWLockExclusive(&g_xqlk);
    XDraw* q = xq_add(); if (q) { q->kind = 1; q->rt = rt; memcpy(q->clear, rgba, 16); }
    ReleaseSRWLockExclusive(&g_xqlk);
}
static ID3D12PipelineState* g_blitpso;
/* present blit pipeline: tools/blit.hlsl (bin/blit_vs.cso + blit_ps.cso, folder in GTA6_BLIT_DIR); PS constants at b1 */
static void build_blit(void) {
    static int tried; if (tried++) return;
    char vsP[MAX_PATH], psP[MAX_PATH]; size_t lv = 0, lp = 0; void *vs = NULL, *ps = NULL;
    if (GetEnvironmentVariableA("GTA6_BLIT_DIR", vsP, MAX_PATH - 20)) { strcpy(psP, vsP); strcat(vsP, "\\blit_vs.cso"); strcat(psP, "\\blit_ps.cso"); vs = load_file(vsP, &lv); ps = load_file(psP, &lp); }
    if (!vs || !ps || !make_xrs()) { rlog("present blit: GTA6_BLIT_DIR (blit_vs.cso / blit_ps.cso) not available"); free(vs); free(ps); return; }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = { 0 };
    d.pRootSignature = g_xrs; d.VS.pShaderBytecode = vs; d.VS.BytecodeLength = lv; d.PS.pShaderBytecode = ps; d.PS.BytecodeLength = lp;
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL; d.SampleMask = 0xFFFFFFFF;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; d.NumRenderTargets = 1; d.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
    HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(g_dev, &d, &IID_ID3D12PipelineState, (void**)&g_blitpso);
    free(vs); free(ps); if (FAILED(hr)) rlog("present blit pipeline failed 0x%08lX", hr);
}
static uint64_t g_presaddr;                            /* address of the render target the game presents this frame */
void rg_set_present(uint64_t addr) { g_presaddr = addr; }
static void run_xlat(UINT bi, D3D12_CPU_DESCRIPTOR_HANDLE bbrtv, float sx, float sy, UINT bw, UINT bh) {
    if (!g_ptab || !g_srvheap || !make_xrs()) { g_nxq = 0; return; }
    if (!g_upload[bi]) {
        D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, UPLOAD_SIZE, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR };
        if (FAILED(ID3D12Device_CreateCommittedResource(g_dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&g_upload[bi]))) { g_nxq = 0; return; }
        ID3D12Resource_Map(g_upload[bi], 0, NULL, (void**)&g_upmap[bi]);
    }
    D3D12_GPU_VIRTUAL_ADDRESS ub = ID3D12Resource_GetGPUVirtualAddress(g_upload[bi]); size_t off = 0;
    ID3D12GraphicsCommandList_SetDescriptorHeaps(g_list, 1, &g_srvheap);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(g_list, g_xrs);
    ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(g_list, 2, ID3D12Resource_GetGPUVirtualAddress(g_ptab));
    AcquireSRWLockExclusive(&g_xqlk);
    int lastprim = -1, currt = -2, ndraw = 0, nclear = 0;
    for (int i = 0; i < g_nxq; i++) {
        XDraw* q = &g_xq[i];
        if (q->kind == 1) {                                       /* clear a cached render target */
            if (q->rt < 0 || q->rt >= g_nrt) continue;
            rt_state(q->rt, D3D12_RESOURCE_STATE_RENDER_TARGET);
            ID3D12GraphicsCommandList_ClearRenderTargetView(g_list, rt_rtv(q->rt), q->clear, 0, NULL); nclear++;
            continue;
        }
        if (off + 2 * STAGE_CB > UPLOAD_SIZE) break;
        /* textures this draw samples must be readable; its own target writable */
        for (int t = 0; t < 8; t++) { uint32_t s = q->psc[36 + t * 4 + 2]; if (s >= RT_SRV0 && s < RT_SRV0 + (uint32_t)g_nrt && (int)(s - RT_SRV0) != q->rt) rt_state((int)(s - RT_SRV0), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); }
        if (q->rt != currt || (q->rt >= 0 && g_rt[q->rt].st != D3D12_RESOURCE_STATE_RENDER_TARGET)) {
            if (q->rt >= 0 && q->rt < g_nrt) { rt_state(q->rt, D3D12_RESOURCE_STATE_RENDER_TARGET); D3D12_CPU_DESCRIPTOR_HANDLE h = rt_rtv(q->rt); ID3D12GraphicsCommandList_OMSetRenderTargets(g_list, 1, &h, FALSE, NULL); g_rt[q->rt].drawn = 1; }
            else ID3D12GraphicsCommandList_OMSetRenderTargets(g_list, 1, &bbrtv, FALSE, NULL);
            currt = q->rt;
        }
        float ax = (q->rt >= 0) ? 1.0f : sx, ay = (q->rt >= 0) ? 1.0f : sy;
        D3D12_VIEWPORT v = { q->vp[0] * ax, q->vp[1] * ay, q->vp[2] * ax, q->vp[3] * ay, q->vp[4], q->vp[5] };
        D3D12_RECT sc = { (LONG)v.TopLeftX, (LONG)v.TopLeftY, (LONG)(v.TopLeftX + v.Width), (LONG)(v.TopLeftY + v.Height) };
        ID3D12GraphicsCommandList_RSSetViewports(g_list, 1, &v); ID3D12GraphicsCommandList_RSSetScissorRects(g_list, 1, &sc);
        if (q->prim != lastprim) { ID3D12GraphicsCommandList_IASetPrimitiveTopology(g_list, (D3D_PRIMITIVE_TOPOLOGY)(q->prim ? q->prim : 4)); lastprim = q->prim; }
        ID3D12GraphicsCommandList_SetPipelineState(g_list, q->pso);
        memcpy(g_upmap[bi] + off, q->vsc, sizeof q->vsc); memcpy(g_upmap[bi] + off + STAGE_CB, q->psc, sizeof q->psc);
        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(g_list, 0, ub + off);
        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(g_list, 1, ub + off + STAGE_CB);
        off += 2 * STAGE_CB;
        ID3D12GraphicsCommandList_DrawInstanced(g_list, q->verts, q->insts ? q->insts : 1, 0, 0); ndraw++;
    }
    g_nxq = 0;
    ReleaseSRWLockExclusive(&g_xqlk);
    /* show the presented render target, scaled to the window */
    int prt = -1; for (int i = 0; i < g_nrt; i++) if (g_rt[i].addr == g_presaddr && g_rt[i].drawn) { prt = i; break; }
    if (prt >= 0) build_blit();
    if (prt >= 0 && g_blitpso && off + 2 * STAGE_CB <= UPLOAD_SIZE) {
        rt_state(prt, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ID3D12GraphicsCommandList_OMSetRenderTargets(g_list, 1, &bbrtv, FALSE, NULL);
        D3D12_VIEWPORT v = { 0, 0, (float)bw, (float)bh, 0, 1 }; D3D12_RECT sc = { 0, 0, (LONG)bw, (LONG)bh };
        ID3D12GraphicsCommandList_RSSetViewports(g_list, 1, &v); ID3D12GraphicsCommandList_RSSetScissorRects(g_list, 1, &sc);
        ID3D12GraphicsCommandList_SetPipelineState(g_list, g_blitpso);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(g_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        uint32_t* c = (uint32_t*)(g_upmap[bi] + off); memset(c, 0, 2 * STAGE_CB); c[0] = (uint32_t)(RT_SRV0 + prt);
        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(g_list, 0, ub + off);      /* the blit VS uses no constants */
        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(g_list, 1, ub + off);      /* blit PS: g_srv at b1 */
        ID3D12GraphicsCommandList_DrawInstanced(g_list, 3, 1, 0, 0);
    }
    static int logged; static ULONGLONG last; ULONGLONG now = GetTickCount64();
    if (logged < 3 || now - last > 5000) { logged++; last = now; rlog("frame: %d translated draws, %d clears, %d cached render targets, presented %s", ndraw, nclear, g_nrt, prt >= 0 ? "a cached render target" : "the back buffer"); }
}

/* Clear the next back buffer to rgba and present it (sync interval 0: pacing is done by the caller). */
int rg_present_clear(const float* rgba) {
    if (g_state != 1) return 0;
    gm_sync(); gm_selftest();
    UINT i = IDXGISwapChain3_GetCurrentBackBufferIndex(g_sc);
    if (ID3D12Fence_GetCompletedValue(g_fence) < g_fenceval[i]) { ID3D12Fence_SetEventOnCompletion(g_fence, g_fenceval[i], g_fev); WaitForSingleObject(g_fev, 1000); }
    ID3D12CommandAllocator_Reset(g_alloc[i]);
    ID3D12GraphicsCommandList_Reset(g_list, g_alloc[i], NULL);
    D3D12_RESOURCE_BARRIER b = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    b.Transition.pResource = g_bb[i]; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    ID3D12GraphicsCommandList_ResourceBarrier(g_list, 1, &b);
    D3D12_CPU_DESCRIPTOR_HANDLE h; ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(g_rtvheap, &h); h.ptr += (SIZE_T)i * g_rtvinc;
    ID3D12GraphicsCommandList_ClearRenderTargetView(g_list, h, rgba, 0, NULL);
    { D3D12_RESOURCE_DESC bd; ID3D12Resource_GetDesc(g_bb[i], &bd);
      run_queued(i, h, (float)bd.Width / 3840.0f, (float)bd.Height / 2160.0f);
      run_xlat(i, h, (float)bd.Width / 3840.0f, (float)bd.Height / 2160.0f, (UINT)bd.Width, bd.Height); }
    /* readback: the centre pixel a few times, and with GTA6_SHOT=1 full frames (600, 1500, 3000, 4500) -> <logs>\shot_<frame>.bmp */
    static ID3D12Resource* rb; static UINT64 rbsize; static int rbcount; static int shots = -1;
    if (shots < 0) shots = GetEnvironmentVariableA("GTA6_SHOT", NULL, 0) ? 1 : 0;
    int frame = rbcount++;
    int doshot = shots && (frame == 600 || frame == 1500 || frame == 3000 || frame == 4500);
    int dorb = doshot || (frame < 400 && (frame % 100) == 99);
    D3D12_RESOURCE_DESC bd; ID3D12Resource_GetDesc(g_bb[i], &bd);
    UINT pitch = ((UINT)bd.Width * 4 + 255) & ~255u;
    if (dorb) {
        UINT64 need = doshot ? (UINT64)pitch * bd.Height : 256;
        if (!rb || rbsize < need) {
            if (rb) ID3D12Resource_Release(rb); rb = NULL; rbsize = 0;
            UINT64 sz = (UINT64)pitch * bd.Height;      /* always room for a full frame */
            D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_READBACK }; D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, sz, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR };
            if (SUCCEEDED(ID3D12Device_CreateCommittedResource(g_dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&rb))) rbsize = sz;
        }
    }
    if (dorb && rb) {
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE; ID3D12GraphicsCommandList_ResourceBarrier(g_list, 1, &b);
        D3D12_TEXTURE_COPY_LOCATION src = { g_bb[i], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; src.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION dst = { rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM; dst.PlacedFootprint.Footprint.Depth = 1;
        if (doshot) { dst.PlacedFootprint.Footprint.Width = (UINT)bd.Width; dst.PlacedFootprint.Footprint.Height = bd.Height; dst.PlacedFootprint.Footprint.RowPitch = pitch;
            ID3D12GraphicsCommandList_CopyTextureRegion(g_list, &dst, 0, 0, 0, &src, NULL); }
        else { dst.PlacedFootprint.Footprint.Width = 1; dst.PlacedFootprint.Footprint.Height = 1; dst.PlacedFootprint.Footprint.RowPitch = 256;
            D3D12_BOX box = { (UINT)bd.Width / 2, bd.Height / 2, 0, (UINT)bd.Width / 2 + 1, bd.Height / 2 + 1, 1 };
            ID3D12GraphicsCommandList_CopyTextureRegion(g_list, &dst, 0, 0, 0, &src, &box); }
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    } else b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    ID3D12GraphicsCommandList_ResourceBarrier(g_list, 1, &b);
    ID3D12GraphicsCommandList_Close(g_list);
    ID3D12CommandList* lists[] = { (ID3D12CommandList*)g_list };
    ID3D12CommandQueue_ExecuteCommandLists(g_q, 1, lists);
    HRESULT hr = IDXGISwapChain3_Present(g_sc, 0, 0);
    g_fenceval[i] = g_fencenext++; ID3D12CommandQueue_Signal(g_q, g_fence, g_fenceval[i]);
    if (dorb && rb) {
        ID3D12Fence_SetEventOnCompletion(g_fence, g_fenceval[i], g_fev); WaitForSingleObject(g_fev, 2000);
        uint8_t* p = NULL; D3D12_RANGE r = { 0, doshot ? (SIZE_T)pitch * bd.Height : 4 };
        if (SUCCEEDED(ID3D12Resource_Map(rb, 0, &r, (void**)&p)) && p) {
            if (!doshot) rlog("centre pixel of presented frame (BGRA): %02X %02X %02X %02X", p[0], p[1], p[2], p[3]);
            else {
                char path[MAX_PATH]; char* sl;
                if (GetEnvironmentVariableA("GTA6_STUB_LOG", path, MAX_PATH) && (sl = strrchr(path, '\\')) != NULL) {
                    snprintf(sl + 1, MAX_PATH - (sl + 1 - path), "shot_%d.bmp", frame);
                    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
                    if (f != INVALID_HANDLE_VALUE) {
                        UINT w = (UINT)bd.Width, hgt = bd.Height; DWORD wr; uint32_t img = w * hgt * 4;
                        uint8_t hdr[54] = { 'B', 'M' }; *(uint32_t*)(hdr + 2) = 54 + img; *(uint32_t*)(hdr + 10) = 54; *(uint32_t*)(hdr + 14) = 40;
                        *(int32_t*)(hdr + 18) = (int32_t)w; *(int32_t*)(hdr + 22) = -(int32_t)hgt; *(uint16_t*)(hdr + 26) = 1; *(uint16_t*)(hdr + 28) = 32; *(uint32_t*)(hdr + 34) = img;
                        WriteFile(f, hdr, 54, &wr, NULL);
                        for (UINT y = 0; y < hgt; y++) WriteFile(f, p + (size_t)y * pitch, w * 4, &wr, NULL);
                        CloseHandle(f); rlog("screenshot of frame %d -> %s", frame, path);
                    }
                }
            }
            D3D12_RANGE z = { 0, 0 }; ID3D12Resource_Unmap(rb, 0, &z);
        }
    }
    static int bad; if (FAILED(hr) && bad++ < 5) rlog("Present failed 0x%08lX", hr);
    return SUCCEEDED(hr);
}

/* ---- root signature layouts: the game serializes with the PC D3D12; decode the blob it hands to CreateRootSignature ----
   Fills kind[i] (0 table, 1 constants, 2 CBV, 3 SRV, 4 UAV) and val[i] (constants: count; table: number of ranges) and a
   readable summary. Returns the parameter count, or -1. */
int rg_rootsig_info(const void* blob, size_t len, uint8_t* kind, uint16_t* val, int maxp, char* out, size_t cap) {
    ID3D12RootSignatureDeserializer* ds = NULL; size_t k = 0; out[0] = 0;
    if (!blob || !len || FAILED(D3D12CreateRootSignatureDeserializer(blob, len, &IID_ID3D12RootSignatureDeserializer, (void**)&ds))) return -1;
    const D3D12_ROOT_SIGNATURE_DESC* d = ID3D12RootSignatureDeserializer_GetRootSignatureDesc(ds);
    int n = (int)d->NumParameters;
    static const char* vis[] = { "all", "vs", "hs", "ds", "gs", "ps", "as", "ms" };
    k += snprintf(out + k, cap - k, "%d params, %u static samplers, flags 0x%X:", n, d->NumStaticSamplers, d->Flags);
    for (int i = 0; i < n && k + 80 < cap; i++) {
        const D3D12_ROOT_PARAMETER* p = &d->pParameters[i]; const char* v = p->ShaderVisibility < 8 ? vis[p->ShaderVisibility] : "?";
        switch (p->ParameterType) {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
            if (i < maxp) { kind[i] = 0; val[i] = (uint16_t)p->DescriptorTable.NumDescriptorRanges; }
            k += snprintf(out + k, cap - k, " [%d]T(%s", i, v);
            for (UINT r = 0; r < p->DescriptorTable.NumDescriptorRanges && k + 40 < cap; r++) {
                const D3D12_DESCRIPTOR_RANGE* g = &p->DescriptorTable.pDescriptorRanges[r];
                static const char* rt = "SUCS"; k += snprintf(out + k, cap - k, " %c%u@%u.%u+%d", rt[g->RangeType & 3], g->NumDescriptors, g->BaseShaderRegister, g->RegisterSpace, (int)g->OffsetInDescriptorsFromTableStart);
            }
            k += snprintf(out + k, cap - k, ")"); break; }
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            if (i < maxp) { kind[i] = 1; val[i] = (uint16_t)p->Constants.Num32BitValues; }
            k += snprintf(out + k, cap - k, " [%d]C%u@b%u.%u(%s)", i, p->Constants.Num32BitValues, p->Constants.ShaderRegister, p->Constants.RegisterSpace, v); break;
        default: {
            int t = p->ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV ? 2 : p->ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV ? 3 : 4;
            if (i < maxp) { kind[i] = (uint8_t)t; val[i] = 0; }
            k += snprintf(out + k, cap - k, " [%d]%s@%u.%u(%s)", i, t == 2 ? "CBV" : t == 3 ? "SRV" : "UAV", p->Descriptor.ShaderRegister, p->Descriptor.RegisterSpace, v); }
        }
    }
    ID3D12RootSignatureDeserializer_Release(ds);
    return n;
}
