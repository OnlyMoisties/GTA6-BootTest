/* Xbox texture layout library (xg_xs.dll) stand-in.
   XGCreateTextureComputer returns a probe object (64 logging slots). The layout answers are a plain
   linear layout computed from the description; the real library describes the console's tiled layouts. */
#define STUBDLL "xg_xs"
#include "stub.h"
#include <stdarg.h>
#include <string.h>

static void xlog(const char* fmt, ...) {
    char path[MAX_PATH], buf[2048];
    if (!GetEnvironmentVariableA("GTA6_STUB_LOG", path, MAX_PATH)) return;
    int n = snprintf(buf, 64, "%llu [xg_xs] ", GetTickCount64());
    va_list ap; va_start(ap, fmt); n += vsnprintf(buf + n, sizeof buf - n - 3, fmt, ap); va_end(ap);
    if (n > (int)sizeof buf - 3) n = sizeof buf - 3;
    buf[n++] = '\r'; buf[n++] = '\n';
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, buf, n, &w, NULL); CloseHandle(h); }
}
static int readable(uint64_t p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (p < 0x10000 || (p >> 47)) return 0;
    if (!VirtualQuery((void*)p, &mbi, sizeof mbi)) return 0;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    return (uint64_t)mbi.BaseAddress + mbi.RegionSize >= p + n;
}
static const char* hx(uint64_t v, int bytes) {
    static __declspec(thread) char s[6][260]; static __declspec(thread) int k; char* o = s[k++ % 6]; o[0] = 0;
    if (!readable(v, bytes)) return o;
    size_t n = 0; const unsigned char* p = (const unsigned char*)v;
    n += snprintf(o + n, 260 - n, "[");
    for (int i = 0; i < bytes && n + 4 < 260; i++) n += snprintf(o + n, 260 - n, "%02X%s", p[i], (i % 8 == 7) ? " " : "");
    snprintf(o + n, 260 - n, "]");
    return o;
}

typedef struct { void** vtbl; uint8_t desc[128]; } Comp;
static volatile LONG g_cnt[64], g_total;
static uint64_t probe(int slot, Comp* self, uint64_t a[6], void* ra) {
    uint64_t ret = 0;
    if (slot == 1 || slot == 2) ret = 1;
    LONG n = InterlockedIncrement(&g_cnt[slot]);
    if (n <= 6 && InterlockedIncrement(&g_total) <= 600)
        xlog("Computer.slot%d (0x%llX%s, 0x%llX%s, 0x%llX, 0x%llX, 0x%llX) -> 0x%llX caller=exe+0x%llX desc=%s",
            slot, a[0], hx(a[0], 32), a[1], hx(a[1], 32), a[2], a[3], a[4], ret, (uint64_t)ra - (uint64_t)GetModuleHandleA(NULL), hx((uint64_t)self->desc, 48));
    return ret;
}
#define DEF(n) static uint64_t s##n(Comp* o, uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4, uint64_t p5, uint64_t p6) { uint64_t v[6] = { p1, p2, p3, p4, p5, p6 }; return probe(n, o, v, _ReturnAddress()); }
DEF(0) DEF(1) DEF(2) DEF(3) DEF(4) DEF(5) DEF(6) DEF(7) DEF(8) DEF(9) DEF(10) DEF(11) DEF(12) DEF(13) DEF(14) DEF(15)
DEF(16) DEF(17) DEF(18) DEF(19) DEF(20) DEF(21) DEF(22) DEF(23) DEF(24) DEF(25) DEF(26) DEF(27) DEF(28) DEF(29) DEF(30) DEF(31)
static void* g_vt[32] = { s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11, s12, s13, s14, s15,
    s16, s17, s18, s19, s20, s21, s22, s23, s24, s25, s26, s27, s28, s29, s30, s31 };

EXP uint64_t XGCreateTextureComputer(void* desc, void** ppComputer, uint64_t c, uint64_t d, uint64_t e) {
    Comp* o = (Comp*)VirtualAlloc(NULL, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    o->vtbl = g_vt; if (readable((uint64_t)desc, 128)) memcpy(o->desc, desc, 128);
    if (ppComputer) *ppComputer = o;
    static volatile LONG n; if (InterlockedIncrement(&n) <= 20) xlog("XGCreateTextureComputer(desc=%p %s, pp=%p) -> probe %p", desc, hx((uint64_t)desc, 64), ppComputer, o);
    return 0;
}
STUB(XGSuggestSwizzleMode, 0)
