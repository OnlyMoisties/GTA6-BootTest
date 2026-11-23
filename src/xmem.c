#define STUBDLL "xmem"
#include "stub.h"
#include <string.h>
#include "fsredir.h"
/* Commit-on-first-touch for ranges the game only reserved: Xbox titles treat these as usable on demand.
   A vectored handler backs the touched 64 KB page and resumes, instead of committing tens of GB up front. */
static struct { uint64_t base, size; } g_res[256];
static volatile LONG g_nres, g_lazy;
static void res_add(void* p, size_t size) { LONG i = InterlockedIncrement(&g_nres) - 1; if (i < 256 && p) { g_res[i].base = (uint64_t)p; g_res[i].size = size; } }
static LONG CALLBACK lazy_veh(EXCEPTION_POINTERS* ep) {
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uint64_t a = r->ExceptionInformation[1]; LONG n = g_nres; if (n > 256) n = 256;
    {   /* known race (instant fake GPU): queue-submit loop reads an entry whose completion object isn't set yet.
           exe+0x24821FE: mov rax,[rcx] with rcx==0, followed by call [rax] (2 bytes) -> resume at exe+0x2482203 */
        uint64_t base = (uint64_t)GetModuleHandleA(NULL);
        if (GetEnvironmentVariableA("GTA6_RACE_GUARD", NULL, 0) && (uint64_t)r->ExceptionAddress == base + 0x24821FE && ep->ContextRecord->Rcx == 0) {
            static volatile LONG c; LONG k = InterlockedIncrement(&c);
            if (k <= 5 || (k & (k - 1)) == 0) slog_("race-guard: skipped null completion callback in queue submit (count)", r->ExceptionAddress, k, 0, 0, 0, 0, 0);
            ep->ContextRecord->Rip = base + 0x2482203;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    for (LONG i = 0; i < n; i++) if (a >= g_res[i].base && a < g_res[i].base + g_res[i].size) {
        if (VirtualAlloc((void*)(a & ~0xFFFFull), 0x10000, MEM_COMMIT, PAGE_READWRITE)) {
            LONG c = InterlockedIncrement(&g_lazy);
            if (c <= 10 || (c & (c - 1)) == 0) slog_("lazy-commit(page, total-pages-so-far)", r->ExceptionAddress, a & ~0xFFFFull, c, 0, 0, 0, 1);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        break;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID r) {
    if (why == DLL_PROCESS_ATTACH) {
        fs_install(); AddVectoredExceptionHandler(1, lazy_veh);
        char w[32];   /* GTA6_WATCHEXE=<hex offset>: hardware write watchpoint on exe+offset (debugging aid) */
        if (GetEnvironmentVariableA("GTA6_WATCHEXE", w, sizeof w)) { char msg[80]; snprintf(msg, sizeof msg, "GTA6WATCH 0x%llX", (uint64_t)GetModuleHandleA(NULL) + _strtoui64(w, NULL, 16)); OutputDebugStringA(msg); }
    }
    return TRUE;
}
static DWORD prot_of(uint64_t a, uint64_t b) {
    uint64_t v[2] = { a, b };
    for (int i = 0; i < 2; i++) { uint64_t p = v[i] & 0xFF; if (v[i] < 0x1000 && (p == 1 || p == 2 || p == 4 || p == 0x10 || p == 0x20 || p == 0x40)) return (DWORD)p; }
    return PAGE_READWRITE;
}
/* Like the console's xmem: XMemAlloc/XMemFree go through the title's exported __XMemAllocationHooks__ {alloc, free}
   when present. The title's hook chain ends in XMemAllocDefault/XMemFreeDefault. */
typedef void* (*HookAlloc)(size_t, uint64_t);
typedef void (*HookFree)(void*, uint64_t);
static void** hooks(void) {
    static void** h; static int done;
    if (!done) { done = 1; h = (void**)GetProcAddress(GetModuleHandleA(NULL), "__XMemAllocationHooks__"); }
    return h;
}
static __declspec(thread) int t_inhook;
EXP void* XMemAlloc(size_t size, uint64_t attrs) {
    void** h = hooks(); void* p;
    if (h && h[0] && !t_inhook) { t_inhook = 1; p = ((HookAlloc)h[0])(size, attrs); t_inhook = 0; SLOG("XMemAlloc[title hook]", size, attrs, 0, 0, 0, p); return p; }
    p = VirtualAlloc(NULL, size ? size : 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); SLOG("XMemAlloc", size, attrs, 0, 0, 0, p); return p;
}
EXP void* XMemAllocDefault(size_t size, uint64_t attrs) { void* p = VirtualAlloc(NULL, size ? size : 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); SLOG("XMemAllocDefault", size, attrs, 0, 0, 0, p); return p; }
EXP void XMemFree(void* p, uint64_t attrs) {
    void** h = hooks();
    SLOG("XMemFree", p, attrs, 0, 0, 0, 0);
    if (h && h[1] && !t_inhook) { t_inhook = 1; ((HookFree)h[1])(p, attrs); t_inhook = 0; return; }
    if (p) VirtualFree(p, 0, MEM_RELEASE);
}
EXP void XMemFreeDefault(void* p, uint64_t attrs) { SLOG("XMemFreeDefault", p, attrs, 0, 0, 0, 0); if (p) VirtualFree(p, 0, MEM_RELEASE); }
/* Placeholder support: the game reserves a range with MEM_RESERVE_PLACEHOLDER, splits it with
   VirtualFree(MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER), then replaces each piece with MEM_REPLACE_PLACEHOLDER. */
#define X_RESERVE_PLACEHOLDER 0x00040000
#define X_REPLACE_PLACEHOLDER 0x00004000
typedef void* (WINAPI *VA2)(HANDLE, void*, SIZE_T, ULONG, ULONG, void*, ULONG);
static VA2 va2(void) { static VA2 f; if (!f) f = (VA2)GetProcAddress(GetModuleHandleA("kernelbase.dll"), "VirtualAlloc2"); return f; }
EXP void* XMemVirtualAlloc(void* addr, size_t size, uint64_t type, uint64_t a4, uint64_t a5) {
    DWORD lowprot = (DWORD)(a5 & 0xFF);
    DWORD prot = (lowprot == 1 || lowprot == 2 || lowprot == 4 || lowprot == 0x10 || lowprot == 0x20 || lowprot == 0x40) ? lowprot : prot_of(a4, a5);
    if ((type & X_RESERVE_PLACEHOLDER) && va2()) {
        void* p = va2()(NULL, addr, size, MEM_RESERVE | X_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
        SLOG("XMemVirtualAlloc[placeholder]", addr, size, type, a4, a5, p); return p;
    }
    if ((type & X_REPLACE_PLACEHOLDER) && va2()) {
        if (prot == PAGE_NOACCESS) prot = PAGE_READWRITE;
        void* p = va2()(NULL, addr, size, MEM_RESERVE | MEM_COMMIT | X_REPLACE_PLACEHOLDER, prot, NULL, 0);
        SLOG("XMemVirtualAlloc[replace-placeholder+commit]", addr, size, type, a4, a5, p ? (uint64_t)p : 0xDEAD0000ull | GetLastError()); return p;
    }
    DWORD t = (DWORD)type & (MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN | MEM_RESET); if (!t) t = MEM_COMMIT | MEM_RESERVE;
    void* p = VirtualAlloc(addr, size, t, prot);
    if (p && !(t & MEM_COMMIT)) res_add(p, size);          /* reserve-only: back pages on first touch */
    {   /* debugging aid: GTA6_WATCH=<size>:<offset> asks the harness to put a hardware write watchpoint at p+offset */
        char w[64]; if (p && GetEnvironmentVariableA("GTA6_WATCH", w, sizeof w)) {
            uint64_t wsz = _strtoui64(w, NULL, 16); char* c = strchr(w, ':');
            if (c && wsz == size) { char msg[80]; snprintf(msg, sizeof msg, "GTA6WATCH 0x%llX", (uint64_t)p + _strtoui64(c + 1, NULL, 16)); OutputDebugStringA(msg); }
        }
    }
    if (!p && addr) p = VirtualAlloc(NULL, size, t | MEM_RESERVE, prot_of(a4, a5));
    SLOG("XMemVirtualAlloc", addr, size, type, a4, a5, p); return p;
}
EXP size_t XMemVirtualQuery(void* addr, MEMORY_BASIC_INFORMATION* mbi, size_t len) { size_t r = VirtualQuery(addr, mbi, len); SLOG("XMemVirtualQuery", addr, mbi, len, 0, 0, r); return r; }
/* physical pages are 64 KB units; we hand out fake page numbers and commit memory when they get mapped */
static volatile LONG64 g_nextpfn = 0x1000;
EXP uint64_t XMemAllocatePhysicalPages(uint64_t flags, uint64_t* count, uint64_t* pages) {
    uint64_t n = count ? *count : 0;
    for (uint64_t i = 0; i < n; i++) pages[i] = (uint64_t)InterlockedIncrement64(&g_nextpfn);
    SLOG("XMemAllocatePhysicalPages", flags, n, pages, 0, 0, 1); return 1;
}
static void hexd(char* o, size_t cap, uint64_t p, int n) { MEMORY_BASIC_INFORMATION m; o[0] = 0; if (p < 0x10000 || (p >> 47) || !VirtualQuery((void*)p, &m, sizeof m) || m.State != MEM_COMMIT || (m.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return; size_t k = 0; for (int i = 0; i < n && k + 20 < cap; i++) k += snprintf(o + k, cap - k, "%llX ", ((uint64_t*)p)[i]); }
STUB(XMemFreePhysicalPages, 0)
EXP uint64_t XMemMapPhysicalPages(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    static volatile LONG cnt; if (InterlockedIncrement(&cnt) <= 60) { char path[MAX_PATH], buf[900], h[5][120];
        uint64_t v[5] = { a, b, c, d, e }; for (int i = 0; i < 5; i++) hexd(h[i], 120, v[i], 4);
        if (GetEnvironmentVariableA("GTA6_STUB_LOG", path, MAX_PATH)) { int n = snprintf(buf, sizeof buf, "%llu [xmem] XMemMapPhysicalPages RAW a=0x%llX{%s} b=0x%llX{%s} c=0x%llX{%s} d=0x%llX{%s} e=0x%llX{%s} f=0x%llX caller=exe+0x%llX\r\n", GetTickCount64(), a, h[0], b, h[1], c, h[2], d, h[3], e, h[4], f, (uint64_t)_ReturnAddress() - (uint64_t)GetModuleHandleA(NULL));
            HANDLE fh = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL); if (fh != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(fh, buf, n, &w, NULL); CloseHandle(fh); } } }
    /* (address, pageCount, pageArray): back the range with ordinary committed memory. pageArray == NULL is an unmap: keep the memory. */
    if (!a) return 0;
    if (c && b) {
        if (!VirtualAlloc((void*)a, (size_t)b << 16, MEM_COMMIT, PAGE_READWRITE))
            VirtualAlloc((void*)a, (size_t)b << 16, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
    return a;
}
/* XMemMapPhysicalPagesScatter(addresses-or-base, count, pageArray): back each 64 KB page with committed memory */
EXP uint64_t XMemMapPhysicalPagesScatter(uint64_t a, uint64_t count, uint64_t pages, uint64_t d, uint64_t e) {
    MEMORY_BASIC_INFORMATION mbi; int isarray = 0;
    if (a >= 0x10000 && !(a >> 47) && VirtualQuery((void*)a, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT && count) {
        uint64_t first = *(uint64_t*)a; isarray = (first >= 0x10000 && !(first >> 47) && !(first & 0xFFFF));
    }
    for (uint64_t i = 0; i < count && i < (1u << 20); i++) {
        uint64_t va = isarray ? ((uint64_t*)a)[i] : a + (i << 16);
        if (pages && va) VirtualAlloc((void*)va, 0x10000, MEM_COMMIT, PAGE_READWRITE);
    }
    SLOG(isarray ? "XMemMapPhysicalPagesScatter[array]" : "XMemMapPhysicalPagesScatter[range]", a, count, pages, d, e, 1);
    return 1;
}
STUB(XMemTransferMemory, 0)
STUB(XMemIsToolsMemAvailable, 0)
STUB(XMemGetWorkingSetStatistics, 0)
