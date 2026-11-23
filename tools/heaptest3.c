/* Leak check for ID3D12Device3::OpenExistingHeapFromAddress.
   MODE=alloc : heap over a plain VirtualAlloc block (SIZE_MB)
   MODE=view  : heap over a full MapViewOfFile view of a pagefile section (the pool would use this)
   The GPU copies a probe value out of the wrapped memory, then everything is released; the caller compares
   system commit before and after. build: cl /nologo /O1 heaptest3.c d3d12.lib dxguid.lib */
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#define CHK(x) do { HRESULT h_ = (x); if (FAILED(h_)) { printf("FAIL %s = 0x%08lX\n", #x, (unsigned long)h_); return 1; } } while (0)

int main(void) {
    char e[32] = "alloc", s[32] = "1024";
    GetEnvironmentVariableA("MODE", e, sizeof e); GetEnvironmentVariableA("SIZE_MB", s, sizeof s);
    uint64_t size = _strtoui64(s, NULL, 10) << 20;
    HANDLE sec = NULL; uint8_t* p;
    if (!strcmp(e, "view")) {
        sec = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE | SEC_COMMIT, (DWORD)(size >> 32), (DWORD)size, NULL);
        p = sec ? (uint8_t*)MapViewOfFile(sec, FILE_MAP_ALL_ACCESS, 0, 0, size) : NULL;
    } else p = (uint8_t*)VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p) { printf("allocation failed %lu\n", GetLastError()); return 1; }
    *(uint64_t*)(p + size - 4096) = 0x1122334455667788ull;
    ID3D12Device* dev; CHK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device, (void**)&dev));
    ID3D12Device3* d3; CHK(ID3D12Device_QueryInterface(dev, &IID_ID3D12Device3, (void**)&d3));
    ID3D12Heap* heap; CHK(ID3D12Device3_OpenExistingHeapFromAddress(d3, p, &IID_ID3D12Heap, (void**)&heap));
    D3D12_HEAP_DESC hd; ID3D12Heap_GetDesc(heap, &hd);
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER };
    ID3D12Resource* src; CHK(ID3D12Device_CreatePlacedResource(dev, heap, 0, &rd, D3D12_RESOURCE_STATE_COPY_SOURCE, NULL, &IID_ID3D12Resource, (void**)&src));
    D3D12_HEAP_PROPERTIES rb = { D3D12_HEAP_TYPE_READBACK }; rd.Width = 256; rd.Flags = D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* dst; CHK(ID3D12Device_CreateCommittedResource(dev, &rb, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&dst));
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    ID3D12CommandQueue* q; CHK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void**)&q));
    ID3D12CommandAllocator* al; CHK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void**)&al));
    ID3D12GraphicsCommandList* cl; CHK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, NULL, &IID_ID3D12GraphicsCommandList, (void**)&cl));
    ID3D12GraphicsCommandList_CopyBufferRegion(cl, dst, 0, src, size - 4096, 8);
    CHK(ID3D12GraphicsCommandList_Close(cl));
    ID3D12CommandQueue_ExecuteCommandLists(q, 1, (ID3D12CommandList**)&cl);
    ID3D12Fence* f; CHK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&f));
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    ID3D12CommandQueue_Signal(q, f, 1); ID3D12Fence_SetEventOnCompletion(f, 1, ev); WaitForSingleObject(ev, 5000);
    uint64_t* m; D3D12_RANGE r = { 0, 8 }; CHK(ID3D12Resource_Map(dst, 0, &r, (void**)&m));
    printf("mode %s, heap %.2f GB, GPU read %llX: %s\n", e, hd.SizeInBytes / 1073741824.0, m[0], m[0] == 0x1122334455667788ull ? "OK" : "WRONG");
    if (GetEnvironmentVariableA("HOLD", NULL, 0)) { printf("holding, kill me\n"); fflush(stdout); Sleep(600000); }
    ID3D12Resource_Unmap(dst, 0, NULL); ID3D12Resource_Release(dst); ID3D12Resource_Release(src); ID3D12Heap_Release(heap);
    ID3D12GraphicsCommandList_Release(cl); ID3D12CommandAllocator_Release(al); ID3D12CommandQueue_Release(q); ID3D12Fence_Release(f);
    ID3D12Device3_Release(d3); ID3D12Device_Release(dev);
    if (sec) { UnmapViewOfFile(p); CloseHandle(sec); } else VirtualFree(p, 0, MEM_RELEASE);
    return 0;
}
