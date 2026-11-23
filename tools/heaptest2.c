/* Physical-page pool test: a 16 GB pagefile-backed section is the "physical memory"; 64 KB pages get mapped as
   views into a placeholder reservation (like XMemMapPhysicalPages), and the GPU reads the pool through one heap
   opened with OpenExistingHeapFromFileMapping. Also checks aliasing (two VAs -> same page) and mapping speed.
   build: cl /nologo /O1 heaptest2.c d3d12.lib dxgi.lib dxguid.lib onecore.lib */
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include <stdint.h>
#define CHK(x) do { HRESULT h_ = (x); if (FAILED(h_)) { printf("FAIL %s = 0x%08lX\n", #x, (unsigned long)h_); return 1; } } while (0)
#define PG 0x10000ull

int main(void) {
    char ev_[32]; uint64_t pool = 16ull << 30; const uint64_t npg = 20000;
    if (GetEnvironmentVariableA("POOL_MB", ev_, 32)) pool = _strtoui64(ev_, NULL, 10) << 20;
    DWORD secflags = GetEnvironmentVariableA("POOL_RESERVE", NULL, 0) ? SEC_RESERVE : SEC_COMMIT;
    HANDLE sec = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE | secflags, (DWORD)(pool >> 32), (DWORD)pool, NULL);
    if (!sec) { printf("section failed %lu\n", GetLastError()); return 1; }
    uint8_t* base = VirtualAlloc2(NULL, NULL, npg * PG * 2, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
    if (!base) { printf("reserve failed %lu\n", GetLastError()); return 1; }
    LARGE_INTEGER t0, t1, fr; QueryPerformanceFrequency(&fr); QueryPerformanceCounter(&t0);
    for (uint64_t i = 0; i < npg; i++) {          /* page i of the VA range -> physical page (npg - 1 - i): scrambled on purpose */
        uint8_t* va = base + i * PG;
        if (i + 1 < npg * 2 && !VirtualFree(va, PG, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) { printf("split %llu failed %lu\n", i, GetLastError()); return 1; }
        if (MapViewOfFile3(sec, GetCurrentProcess(), va, (npg - 1 - i) * PG, PG, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0) != va) { printf("map %llu failed %lu\n", i, GetLastError()); return 1; }
    }
    QueryPerformanceCounter(&t1);
    printf("mapped %llu pages in %.1f ms\n", npg, (t1.QuadPart - t0.QuadPart) * 1000.0 / fr.QuadPart);
    /* alias: map physical page 5 a second time, after the first range */
    uint8_t* alias = base + npg * PG;
    VirtualFree(alias, PG, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    if (MapViewOfFile3(sec, GetCurrentProcess(), alias, 5 * PG, PG, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0) != alias) { printf("alias map failed %lu\n", GetLastError()); return 1; }
    for (uint64_t i = 0; i < npg; i++) *(uint64_t*)(base + i * PG + 128) = 0xABCD000000000000ull | i;
    printf("alias sees VA page %llu value: %llX (expect %llX)\n", npg - 1 - 5, *(uint64_t*)(alias + 128), 0xABCD000000000000ull | (npg - 1 - 5));
    /* unmap one page back to a placeholder, and remap it */
    if (!UnmapViewOfFile2(GetCurrentProcess(), base, MEM_PRESERVE_PLACEHOLDER)) { printf("unmap failed %lu\n", GetLastError()); return 1; }
    if (MapViewOfFile3(sec, GetCurrentProcess(), base, (npg - 1) * PG, PG, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0) != base) { printf("remap failed %lu\n", GetLastError()); return 1; }

    ID3D12Device* dev; CHK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device, (void**)&dev));
    ID3D12Device3* d3; CHK(ID3D12Device_QueryInterface(dev, &IID_ID3D12Device3, (void**)&d3));
    ID3D12Heap* heap; CHK(ID3D12Device3_OpenExistingHeapFromFileMapping(d3, sec, &IID_ID3D12Heap, (void**)&heap));
    D3D12_HEAP_DESC hd; ID3D12Heap_GetDesc(heap, &hd);
    printf("pool heap size %.1f GB\n", hd.SizeInBytes / 1073741824.0);
    /* GPU: copy 8 bytes from physical page p (VA page npg-1-p) for a few pages */
    const uint64_t chunk = pool < (2ull << 30) ? pool : (2ull << 30);
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, chunk, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER };
    ID3D12Resource* src; CHK(ID3D12Device_CreatePlacedResource(dev, heap, 0, &rd, D3D12_RESOURCE_STATE_COPY_SOURCE, NULL, &IID_ID3D12Resource, (void**)&src));
    D3D12_HEAP_PROPERTIES rb = { D3D12_HEAP_TYPE_READBACK }; rd.Width = 4096; rd.Flags = D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* dst; CHK(ID3D12Device_CreateCommittedResource(dev, &rb, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&dst));
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    ID3D12CommandQueue* q; CHK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void**)&q));
    ID3D12CommandAllocator* al; CHK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void**)&al));
    ID3D12GraphicsCommandList* cl; CHK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, NULL, &IID_ID3D12GraphicsCommandList, (void**)&cl));
    uint64_t probe[4] = { 0, 5, 777, npg - 1 };
    for (int k = 0; k < 4; k++) ID3D12GraphicsCommandList_CopyBufferRegion(cl, dst, k * 8, src, probe[k] * PG + 128, 8);
    CHK(ID3D12GraphicsCommandList_Close(cl));
    ID3D12CommandQueue_ExecuteCommandLists(q, 1, (ID3D12CommandList**)&cl);
    ID3D12Fence* f; CHK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&f));
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    ID3D12CommandQueue_Signal(q, f, 1); ID3D12Fence_SetEventOnCompletion(f, 1, ev); WaitForSingleObject(ev, 5000);
    uint64_t* m; D3D12_RANGE r = { 0, 32 }; CHK(ID3D12Resource_Map(dst, 0, &r, (void**)&m));
    int bad = 0;
    for (int k = 0; k < 4; k++) { uint64_t want = 0xABCD000000000000ull | (npg - 1 - probe[k]); printf("phys page %5llu: GPU %llX  want %llX\n", probe[k], m[k], want); bad += m[k] != want; }
    printf("result: %s, removed reason 0x%08lX\n", bad ? "WRONG" : "OK", (unsigned long)ID3D12Device_GetDeviceRemovedReason(dev));
    return bad;
}
