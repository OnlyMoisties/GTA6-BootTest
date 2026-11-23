/* Xbox audio hardware stand-ins: AcpHal.dll (APU memory + shape contexts), xapu.dll, chad.dll (XDsp).
   APU memory is real (one heap, 32-bit APU addresses = offset into it); everything else succeeds as a no-op,
   so the game runs silent. Built three times with /DWHICH_<dll>. */
#if defined(WHICH_AcpHal)
#define STUBDLL "AcpHal"
#elif defined(WHICH_xapu)
#define STUBDLL "xapu"
#else
#define STUBDLL "chad"
#endif
#include "stub.h"

/* generic no-op COM object: every method logs once and returns S_OK (AddRef/Release return 1) */
static volatile LONG g_ocnt[32];
static uint64_t onop(int slot, void* self, uint64_t a, uint64_t b, uint64_t c, void* ra) {
    if (InterlockedIncrement(&g_ocnt[slot]) <= 3) slog_("object-method", ra, (uint64_t)slot, a, b, c, (uint64_t)self, 0);
    return (slot == 1 || slot == 2) ? 1 : 0;
}
#define OD(n) static uint64_t o##n(void* s, uint64_t a, uint64_t b, uint64_t c) { return onop(n, s, a, b, c, _ReturnAddress()); }
OD(0) OD(1) OD(2) OD(3) OD(4) OD(5) OD(6) OD(7) OD(8) OD(9) OD(10) OD(11) OD(12) OD(13) OD(14) OD(15)
OD(16) OD(17) OD(18) OD(19) OD(20) OD(21) OD(22) OD(23) OD(24) OD(25) OD(26) OD(27) OD(28) OD(29) OD(30) OD(31)
static void* g_ovt[32] = { o0, o1, o2, o3, o4, o5, o6, o7, o8, o9, o10, o11, o12, o13, o14, o15,
    o16, o17, o18, o19, o20, o21, o22, o23, o24, o25, o26, o27, o28, o29, o30, o31 };
static void* newnop(void) { void** o = (void**)VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); if (o) o[0] = g_ovt; return o; }

#if defined(WHICH_AcpHal)
static uint8_t* g_heap; static uint64_t g_heapsize; static volatile LONG64 g_heapused;
#define APU_BASE 0x10000000u
EXP uint64_t ApuCreateHeap(uint64_t cached, uint64_t noncached, uint64_t c, uint64_t d, uint64_t e) {
    uint64_t sz = (uint32_t)cached + (uint32_t)noncached;
    if (!g_heap) { g_heap = (uint8_t*)VirtualAlloc(NULL, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); g_heapsize = sz; }
    SLOG("ApuCreateHeap(cached, noncached)", cached, noncached, c, d, e, g_heap ? 0 : E_FAIL_);
    return g_heap ? 0 : E_FAIL_;
}
/* ApuAlloc(void** va, APU_ADDRESS* pa, size, alignment, flags): bump allocator inside the heap */
EXP uint64_t ApuAlloc(void** va, uint32_t* pa, uint64_t size, uint64_t align, uint64_t flags) {
    uint32_t al = (uint32_t)align; if (al < 16) al = 16; if (al & (al - 1)) al = 16;
    uint64_t r = E_OUTOFMEMORY;
    if (g_heap) for (;;) {
        LONG64 used = g_heapused; uint64_t start = ((uint64_t)used + al - 1) & ~(uint64_t)(al - 1);
        if (start + (uint32_t)size > g_heapsize) break;
        if (InterlockedCompareExchange64(&g_heapused, (LONG64)(start + (uint32_t)size), used) == used) {
            if (va) *va = g_heap + start; if (pa) *pa = APU_BASE + (uint32_t)start; r = 0; break;
        }
    }
    SLOG("ApuAlloc(va*, pa*, size, align, flags)", va, pa, size, align, flags, r); return r;
}
EXP uint64_t ApuFree(void* va) { SLOG("ApuFree", va, 0, 0, 0, 0, 0); return 0; }
EXP uint64_t ApuIsVirtualAddressValid(uint8_t* va, uint64_t contiguous) { return g_heap && va >= g_heap && va < g_heap + g_heapsize; }
EXP uint64_t ApuMapVirtualAddress(uint8_t* va) { return (g_heap && va >= g_heap) ? APU_BASE + (uint32_t)(va - g_heap) : 0; }
EXP void* ApuMapApuAddress(uint32_t pa) { return (g_heap && pa >= APU_BASE) ? g_heap + (pa - APU_BASE) : NULL; }
EXP uint64_t AcpHalCreate(void** ppHal, uint64_t b, uint64_t c, uint64_t d, uint64_t e) {
    if (ppHal) *ppHal = newnop(); SLOG("AcpHalCreate -> no-op object", ppHal, b, c, d, e, 0); return 0;
}
/* AcpHalAllocateShapeContexts(ctx): six u32 counts at +0x00..+0x14, six array pointers at +0x18..+0x40.
   Count 4 -> pointer at +0x38 confirmed (title memsets count*64 there). Arrays come from the APU heap,
   256 bytes per entry to be safe about the real per-type size. */
EXP uint64_t AcpHalAllocateShapeContexts(uint8_t* ctx, uint64_t b, uint64_t c, uint64_t d, uint64_t e) {
    uint64_t r = 0;
    if (!ctx) return E_FAIL_;
    for (int i = 0; i < 6; i++) {
        uint32_t n = *(uint32_t*)(ctx + i * 4);
        if (!n || n > 100000) continue;
        void* va = NULL; uint32_t pa = 0;
        if (ApuAlloc(&va, &pa, (uint64_t)n * 256, 256, 0) != 0) { va = VirtualAlloc(NULL, (size_t)n * 256, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); }
        *(void**)(ctx + 0x18 + i * 8) = va;
        SLOG("AcpHalAllocateShapeContexts: type, count, array", (uint64_t)i, n, va, 0, 0, 0);
    }
    return r;
}
STUB(AcpHalReleaseShapeContexts, 0)

#elif defined(WHICH_xapu)
EXP uint64_t XApuConnect(void** pp, uint64_t b, uint64_t c, uint64_t d, uint64_t e) { if (pp) *pp = newnop(); SLOG("XApuConnect", pp, b, c, d, e, 0); return 0; }
STUB(XApuDisconnect, 0)
STUB(XApuEnqueueCommand, 0)
STUB(XApuDequeueResult, 0x80004005)
STUB(XApuGetResultCount, 0)

#else
EXP uint64_t XDspActivate(void** pp, uint64_t b, uint64_t c, uint64_t d, uint64_t e) { SLOG("XDspActivate", pp, b, c, d, e, 0); return 0; }
EXP uint64_t XDspConnectWithMaximumStreamLimit(void** pp, uint64_t b, uint64_t c, uint64_t d, uint64_t e) { if (pp) *pp = newnop(); SLOG("XDspConnectWithMaximumStreamLimit", pp, b, c, d, e, 0); return 0; }
STUB(XDspDeactivate, 0)
STUB(XDspDisconnect, 0)
STUB(XDspSubmitCommand, 0)
#endif
