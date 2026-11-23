/* Check that a piece of a placeholder reservation, replaced by its own committed allocation,
   can be opened as a D3D12 heap (OpenExistingHeapFromAddress) and read by the GPU (zero copy).
   build: cl /nologo /O1 heaptest.c d3d12.lib dxgi.lib dxguid.lib */
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include <stdint.h>

typedef PVOID (WINAPI *VA2)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
#define CHK(x) do { HRESULT h_ = (x); if (FAILED(h_)) { printf("FAIL %s = 0x%08lX\n", #x, (unsigned long)h_); return 1; } } while (0)

int main(void) {
    VA2 va2 = (VA2)GetProcAddress(GetModuleHandleA("kernelbase.dll"), "VirtualAlloc2");
    const SIZE_T total = 64ull << 20, piece = 1ull << 20, at = 8ull << 20;
    uint8_t* base = va2(NULL, NULL, total, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
    if (!base) { printf("reserve failed %lu\n", GetLastError()); return 1; }
    /* split [at, at+piece) out of the placeholder, then replace it with a committed allocation */
    if (!VirtualFree(base + at, piece, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) { printf("split failed %lu\n", GetLastError()); return 1; }
    uint8_t* p = va2(NULL, base + at, piece, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
    if (p != base + at) { printf("replace failed %lu\n", GetLastError()); return 1; }
    for (SIZE_T i = 0; i < piece; i++) p[i] = (uint8_t)(i * 7 + 3);

    ID3D12Device* dev; CHK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device, (void**)&dev));
    D3D12_FEATURE_DATA_EXISTING_HEAPS eh = { 0 };
    ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_EXISTING_HEAPS, &eh, sizeof eh);
    printf("existing heaps supported: %d\n", eh.Supported);
    ID3D12Device3* d3; CHK(ID3D12Device_QueryInterface(dev, &IID_ID3D12Device3, (void**)&d3));
    ID3D12Heap* heap; CHK(ID3D12Device3_OpenExistingHeapFromAddress(d3, p, &IID_ID3D12Heap, (void**)&heap));
    D3D12_HEAP_DESC hd; ID3D12Heap_GetDesc(heap, &hd);
    printf("heap size 0x%llX\n", (unsigned long long)hd.SizeInBytes);

    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, piece, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER };
    ID3D12Resource* src; CHK(ID3D12Device_CreatePlacedResource(dev, heap, 0, &rd, D3D12_RESOURCE_STATE_COPY_SOURCE, NULL, &IID_ID3D12Resource, (void**)&src));
    /* GPU copy from the wrapped game memory into a readback buffer */
    D3D12_HEAP_PROPERTIES rb = { D3D12_HEAP_TYPE_READBACK };
    rd.Flags = D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* dst; CHK(ID3D12Device_CreateCommittedResource(dev, &rb, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&dst));
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    ID3D12CommandQueue* q; CHK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void**)&q));
    ID3D12CommandAllocator* al; CHK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void**)&al));
    ID3D12GraphicsCommandList* cl; CHK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, NULL, &IID_ID3D12GraphicsCommandList, (void**)&cl));
    ID3D12GraphicsCommandList_CopyBufferRegion(cl, dst, 0, src, 0, piece);
    CHK(ID3D12GraphicsCommandList_Close(cl));
    ID3D12CommandQueue_ExecuteCommandLists(q, 1, (ID3D12CommandList**)&cl);
    ID3D12Fence* f; CHK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&f));
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    ID3D12CommandQueue_Signal(q, f, 1); ID3D12Fence_SetEventOnCompletion(f, 1, ev); WaitForSingleObject(ev, 5000);
    uint8_t* m; D3D12_RANGE r = { 0, piece }; CHK(ID3D12Resource_Map(dst, 0, &r, (void**)&m));
    SIZE_T bad = 0; for (SIZE_T i = 0; i < piece; i++) if (m[i] != (uint8_t)(i * 7 + 3)) bad++;
    printf("GPU read of wrapped memory: %s (%zu mismatches)\n", bad ? "WRONG" : "OK", bad);
    printf("device removed reason: 0x%08lX\n", (unsigned long)ID3D12Device_GetDeviceRemovedReason(dev));
    return bad != 0;
}
