/* Real PC Direct3D 12 backend: device, queue and a flip-model swap chain on the game's window.
   Compiled into d3d12_xs.dll next to d3d12_xs.c (kept separate because the D3D12 headers clash with the stand-in exports). */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <d3d12.h>
#include <dxgi1_4.h>

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
   Root signature: param0 = 20 root constants (b0: 16 user SGPRs + memory-window base), param1 = root SRV t0 (memory window). */
typedef struct { uint64_t vh, ph; ID3D12PipelineState* pso; } XPso;
static XPso g_xpsos[256]; static int g_nxpsos;
static ID3D12RootSignature* g_xrs;
typedef struct { ID3D12PipelineState* pso; uint32_t user[16]; uint64_t base; uint8_t win[4096]; uint32_t winlen; uint32_t verts, insts; float vp[6]; } XDraw;
static XDraw* g_xq; static int g_nxq, g_capxq;
static void* load_file(const char* path, size_t* len) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL); if (f == INVALID_HANDLE_VALUE) return NULL;
    DWORD sz = GetFileSize(f, NULL), rd = 0; void* p = malloc(sz ? sz : 1); if (p && !ReadFile(f, p, sz, &rd, NULL)) { free(p); p = NULL; }
    CloseHandle(f); *len = rd; return p;
}
static ID3D12PipelineState* get_xpso(uint64_t vh, uint64_t ph) {
    for (int i = 0; i < g_nxpsos; i++) if (g_xpsos[i].vh == vh && g_xpsos[i].ph == ph) return g_xpsos[i].pso;
    if (g_nxpsos >= 256) return NULL;
    XPso* e = &g_xpsos[g_nxpsos++]; e->vh = vh; e->ph = ph; e->pso = NULL;
    char dir[MAX_PATH]; if (!GetEnvironmentVariableA("GTA6_XLAT_DIR", dir, MAX_PATH)) return NULL;
    if (!g_xrs) {
        D3D12_ROOT_PARAMETER p[2] = { 0 };
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; p[0].Constants.Num32BitValues = 20; p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rd = { 2, p, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ID3DBlob *blob = NULL, *err = NULL;
        if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) return NULL;
        HRESULT hr = ID3D12Device_CreateRootSignature(g_dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void**)&g_xrs);
        ID3D10Blob_Release(blob); if (FAILED(hr)) { rlog("translated root signature failed 0x%08lX", hr); return NULL; }
    }
    char pv[MAX_PATH], pp[MAX_PATH]; size_t lv = 0, lp = 0;
    snprintf(pv, sizeof pv, "%s\\%016llX.vs.cso", dir, vh); snprintf(pp, sizeof pp, "%s\\%016llX.ps.cso", dir, ph);
    void* vs = load_file(pv, &lv); void* ps = load_file(pp, &lp);
    if (!vs || !ps) { rlog("no translated shader for vs %016llX / ps %016llX (%s%s)", vh, ph, vs ? "" : "vs missing ", ps ? "" : "ps missing"); free(vs); free(ps); return NULL; }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = { 0 };
    d.pRootSignature = g_xrs; d.VS.pShaderBytecode = vs; d.VS.BytecodeLength = lv; d.PS.pShaderBytecode = ps; d.PS.BytecodeLength = lp;
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL; d.SampleMask = 0xFFFFFFFF;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; d.RasterizerState.DepthClipEnable = TRUE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; d.NumRenderTargets = 1; d.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
    HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(g_dev, &d, &IID_ID3D12PipelineState, (void**)&e->pso);
    rlog("pipeline from TRANSLATED shaders vs %016llX + ps %016llX: %s 0x%08lX", vh, ph, SUCCEEDED(hr) ? "OK" : "FAILED", hr);
    free(vs); free(ps);
    return e->pso;
}
void rg_queue_xlat_draw(uint64_t vh, uint64_t ph, const uint32_t* user16, uint64_t base, const void* win, uint32_t winlen, uint32_t verts, uint32_t insts, const float* vp) {
    if (g_state != 1) return;
    ID3D12PipelineState* pso = get_xpso(vh, ph); if (!pso) return;
    if (g_nxq == g_capxq) { int nc = g_capxq ? g_capxq * 2 : 64; XDraw* n = (XDraw*)realloc(g_xq, sizeof(XDraw) * nc); if (!n) return; g_xq = n; g_capxq = nc; }
    XDraw* q = &g_xq[g_nxq++]; q->pso = pso; memcpy(q->user, user16, 64); q->base = base;
    q->winlen = winlen > sizeof q->win ? sizeof q->win : winlen; memcpy(q->win, win, q->winlen);
    q->verts = verts; q->insts = insts; memcpy(q->vp, vp, sizeof q->vp);
}
static void run_xlat(UINT bi, D3D12_CPU_DESCRIPTOR_HANDLE rtv, float sx, float sy) {
    if (!g_nxq) return;
    if (!g_upload[bi]) {
        D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, UPLOAD_SIZE, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR };
        if (FAILED(ID3D12Device_CreateCommittedResource(g_dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&g_upload[bi]))) return;
        ID3D12Resource_Map(g_upload[bi], 0, NULL, (void**)&g_upmap[bi]);
    }
    D3D12_GPU_VIRTUAL_ADDRESS gbase = ID3D12Resource_GetGPUVirtualAddress(g_upload[bi]); size_t off = 0;
    ID3D12GraphicsCommandList_OMSetRenderTargets(g_list, 1, &rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(g_list, g_xrs);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(g_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (int i = 0; i < g_nxq; i++) {
        XDraw* q = &g_xq[i];
        if (off + q->winlen + 256 > UPLOAD_SIZE) break;
        memcpy(g_upmap[bi] + off, q->win, q->winlen);
        if (GetEnvironmentVariableA("GTA6_XLAT_TEST", NULL, 0) && q->winlen >= 64) {
            /* test: rewrite the colour stream (header dword 3: type 4, stride/offset packed like the game's) to red */
            uint32_t* w = (uint32_t*)(g_upmap[bi] + off); uint32_t hdr = w[3];
            if ((hdr & 15) == 4) { uint32_t stride = (hdr >> 3) & 60, coff = (hdr >> 7) & 0x1FFFFFC;
                for (uint32_t vtx = 0; vtx < q->verts && coff + vtx * stride + 16 <= q->winlen; vtx++) { float* c = (float*)((uint8_t*)w + coff + vtx * stride); c[0] = 1; c[1] = 0; c[2] = 0; c[3] = 1; } }
        }
        uint32_t rc[20]; memcpy(rc, q->user, 64); rc[16] = (uint32_t)q->base; rc[17] = (uint32_t)(q->base >> 32); rc[18] = rc[19] = 0;
        D3D12_VIEWPORT v = { q->vp[0] * sx, q->vp[1] * sy, q->vp[2] * sx, q->vp[3] * sy, q->vp[4], q->vp[5] };
        D3D12_RECT sc = { (LONG)v.TopLeftX, (LONG)v.TopLeftY, (LONG)(v.TopLeftX + v.Width), (LONG)(v.TopLeftY + v.Height) };
        ID3D12GraphicsCommandList_RSSetViewports(g_list, 1, &v); ID3D12GraphicsCommandList_RSSetScissorRects(g_list, 1, &sc);
        ID3D12GraphicsCommandList_SetPipelineState(g_list, q->pso);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(g_list, 0, 20, rc, 0);
        ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(g_list, 1, gbase + off);
        ID3D12GraphicsCommandList_DrawInstanced(g_list, q->verts, q->insts ? q->insts : 1, 0, 0);
        off = (off + q->winlen + 255) & ~(size_t)255;
    }
    static int logged; if (logged++ < 3) rlog("frame ran %d draws with TRANSLATED game shaders on the real GPU", g_nxq);
    g_nxq = 0;
}

/* Clear the next back buffer to rgba and present it (sync interval 0: pacing is done by the caller). */
int rg_present_clear(const float* rgba) {
    if (g_state != 1) return 0;
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
      run_xlat(i, h, (float)bd.Width / 3840.0f, (float)bd.Height / 2160.0f); }
    /* readback of the centre pixel a few times, to check what actually reached the screen */
    static ID3D12Resource* rb; static int rbcount;
    int dorb = rbcount < 400 && (rbcount % 100) == 99;
    rbcount++;
    if (dorb) {
        if (!rb) { D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_READBACK }; D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, 256, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR };
                   ID3D12Device_CreateCommittedResource(g_dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&rb); }
        D3D12_RESOURCE_DESC bd; ID3D12Resource_GetDesc(g_bb[i], &bd);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE; ID3D12GraphicsCommandList_ResourceBarrier(g_list, 1, &b);
        D3D12_TEXTURE_COPY_LOCATION src = { g_bb[i], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; src.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION dst = { rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM; dst.PlacedFootprint.Footprint.Width = 1; dst.PlacedFootprint.Footprint.Height = 1; dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = 256;
        D3D12_BOX box = { (UINT)bd.Width / 2, bd.Height / 2, 0, (UINT)bd.Width / 2 + 1, bd.Height / 2 + 1, 1 };
        ID3D12GraphicsCommandList_CopyTextureRegion(g_list, &dst, 0, 0, 0, &src, &box);
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
        ID3D12Fence_SetEventOnCompletion(g_fence, g_fenceval[i], g_fev); WaitForSingleObject(g_fev, 1000);
        uint8_t* p = NULL; D3D12_RANGE r = { 0, 4 };
        if (SUCCEEDED(ID3D12Resource_Map(rb, 0, &r, (void**)&p)) && p) { rlog("centre pixel of presented frame (BGRA): %02X %02X %02X %02X", p[0], p[1], p[2], p[3]); D3D12_RANGE z = { 0, 0 }; ID3D12Resource_Unmap(rb, 0, &z); }
    }
    static int bad; if (FAILED(hr) && bad++ < 5) rlog("Present failed 0x%08lX", hr);
    return SUCCEEDED(hr);
}
