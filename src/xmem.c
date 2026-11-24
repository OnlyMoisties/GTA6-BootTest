#define STUBDLL "xmem"
#include "stub.h"
#include <string.h>
#define FS_MEMHOOKS
static LPVOID WINAPI h_VirtualAlloc(LPVOID, SIZE_T, DWORD, DWORD);
static BOOL WINAPI h_VirtualFree(LPVOID, SIZE_T, DWORD);
#include "fsredir.h"
#include "gpumem.h"
/* ---- GPU-visible physical memory (see gpumem.h), opt-in with GTA6_GPUMEM=1 ---- */
typedef void* (WINAPI *MVOF3)(HANDLE, HANDLE, void*, ULONG64, SIZE_T, ULONG, ULONG, void*, ULONG);
typedef BOOL (WINAPI *UVOF2)(HANDLE, void*, ULONG);
static GpuMem g_gm; static int g_gpumem = -1; static SRWLOCK g_gmlk = SRWLOCK_INIT;
static volatile LONG64 g_nextpfn = 0x1000;
static int gpumem_on(void) { if (g_gpumem < 0) g_gpumem = GetEnvironmentVariableA("GTA6_GPUMEM", NULL, 0) ? 1 : 0; return g_gpumem; }
EXP void* xmem_gpu_state(void) { return gpumem_on() ? &g_gm : NULL; }
/* map physical page pfn at va (va must lie in a placeholder reservation, or already be one of our views); 1 = ok */
static int gm_map_page(uint64_t va, uint64_t pfn) {
    static MVOF3 mv; static UVOF2 uv;
    if (!mv) { HMODULE kb = GetModuleHandleA("kernelbase.dll"); mv = (MVOF3)GetProcAddress(kb, "MapViewOfFile3"); uv = (UVOF2)GetProcAddress(kb, "UnmapViewOfFile2"); }
    if (!mv || !uv || pfn < GM_PFN0) return 0;
    uint64_t idx = pfn - GM_PFN0; uint32_t s = (uint32_t)(idx / GM_SECPAGES);
    if (s >= GM_MAXSEC || (va & (GM_PAGE - 1)) || (va >> 48)) return 0;
    int ok = 0;
    AcquireSRWLockExclusive(&g_gmlk);
    while (g_gm.nsec <= (LONG)s) {
        uint64_t sz = (uint64_t)GM_SECPAGES * GM_PAGE;
        HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE | SEC_COMMIT, (DWORD)(sz >> 32), (DWORD)sz, NULL);
        if (!h) { slog_("gpumem: section create failed (index, error)", 0, g_gm.nsec, GetLastError(), 0, 0, 0, 0); goto out; }
        g_gm.sec[g_gm.nsec] = h; InterlockedIncrement(&g_gm.nsec);
        slog_("gpumem: new 2 GB physical section (index)", 0, g_gm.nsec - 1, 0, 0, 0, 0, 1);
    }
    uint32_t d = (uint32_t)(va >> 32);
    if (!g_gm.dir[d]) { uint32_t* c = (uint32_t*)VirtualAlloc(NULL, 65536 * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); if (!c) goto out; g_gm.dir[d] = c; }
    uint32_t* e = &g_gm.dir[d][(va >> 16) & 0xFFFF];
    static uint8_t* keep; int kept = 0;                    /* private committed memory there: keep its bytes (in-place commit kept them too) */
    MEMORY_BASIC_INFORMATION mi;
    if (!*e && VirtualQuery((void*)va, &mi, sizeof mi) && mi.State == MEM_COMMIT && mi.Type == MEM_PRIVATE) {
        if (!keep) keep = (uint8_t*)VirtualAlloc(NULL, GM_PAGE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (keep) { memcpy(keep, (void*)va, GM_PAGE); kept = 1; }
    }
    if (*e) uv(GetCurrentProcess(), (void*)va, MEM_PRESERVE_PLACEHOLDER);
    else VirtualFree((void*)va, GM_PAGE, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);     /* split this page out (fails harmlessly if already split) */
    if (mv(g_gm.sec[s], GetCurrentProcess(), (void*)va, (idx % GM_SECPAGES) * GM_PAGE, GM_PAGE, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0) != (void*)va) {
        static volatile LONG nf; if (InterlockedIncrement(&nf) <= 10) slog_("gpumem: map view failed (va, pfn, error)", 0, va, pfn, GetLastError(), 0, 0, 0);
        *e = 0; goto out;
    }
    if (kept) memcpy((void*)va, keep, GM_PAGE);
    *e = ((s + 1) << 24) | (uint32_t)(idx % GM_SECPAGES);
    InterlockedIncrement(&g_gm.chunkgen[d]); InterlockedIncrement64(&g_gm.gen); ok = 1;
out:
    ReleaseSRWLockExclusive(&g_gmlk);
    return ok;
}
static int gm_mapped(uint64_t va) { uint32_t* c = g_gm.dir[(va >> 32) & 0xFFFF]; return c && c[(va >> 16) & 0xFFFF]; }
/* Commit-on-first-touch for ranges the game only reserved: Xbox titles treat these as usable on demand.
   A vectored handler backs the touched 64 KB page and resumes, instead of committing tens of GB up front. */
static struct { uint64_t base, size; int phys; } g_res[256];
static volatile LONG g_nres, g_lazy;
static void res_add(void* p, size_t size, int phys) { LONG i = InterlockedIncrement(&g_nres) - 1; if (i < 256 && p) { g_res[i].base = (uint64_t)p; g_res[i].size = size; g_res[i].phys = phys; } }
static LONG CALLBACK lazy_veh(EXCEPTION_POINTERS* ep) {
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uint64_t a = r->ExceptionInformation[1]; LONG n = g_nres; if (n > 256) n = 256;
    {   /* known race (instant fake GPU): queue-submit loop reads an entry whose completion object isn't set yet.
           exe+0x24821FE: mov rax,[rcx] with rcx==0, followed by call [rax] (2 bytes) -> resume at exe+0x2482203 */
        uint64_t base = (uint64_t)GetModuleHandleA(NULL);
        /* on by default: any long stall in PresentX (first pipeline compile, opening pool sections) exposes it. GTA6_NO_RACE_GUARD=1 disables. */
        if (!GetEnvironmentVariableA("GTA6_NO_RACE_GUARD", NULL, 0) && (uint64_t)r->ExceptionAddress == base + 0x24821FE && ep->ContextRecord->Rcx == 0) {
            static volatile LONG c; LONG k = InterlockedIncrement(&c);
            if (k <= 5 || (k & (k - 1)) == 0) slog_("race-guard: skipped null completion callback in queue submit (count)", r->ExceptionAddress, k, 0, 0, 0, 0, 0);
            ep->ContextRecord->Rip = base + 0x2482203;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    for (LONG i = 0; i < n; i++) if (a >= g_res[i].base && a < g_res[i].base + g_res[i].size) {
        if (g_res[i].phys) {                     /* placeholder reservation: back the page with a fresh pool page */
            if (gm_mapped(a & ~0xFFFFull) || gm_map_page(a & ~0xFFFFull, (uint64_t)InterlockedIncrement64(&g_nextpfn))) {
                LONG c = InterlockedIncrement(&g_lazy);
                if (c <= 10 || (c & (c - 1)) == 0) slog_("lazy-map(pool page, total-pages-so-far)", r->ExceptionAddress, a & ~0xFFFFull, c, 0, 0, 0, 1);
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            break;
        }
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
        void* p = NULL;
        /* The title's own placeholder ranges hold its GPU heaps (per-draw root tables, descriptors). Its shaders keep pointers
           into them as 32 bits (high half hard-coded 0), so like the console they must sit below 4 GB: ask for 2-4 GB
           (0x10000000-0x7FFFFFFF stays free for the GPU-only descriptor heap aliases). GTA6_NO_LOW32=1 turns this off. */
        if (!addr && !GetEnvironmentVariableA("GTA6_NO_LOW32", NULL, 0)) {
            MEM_ADDRESS_REQUIREMENTS req = { 0 }; req.LowestStartingAddress = (PVOID)0x80000000ull; req.HighestEndingAddress = (PVOID)0xFFFFFFFFull;
            MEM_EXTENDED_PARAMETER ep = { 0 }; ep.Type = MemExtendedParameterAddressRequirements; ep.Pointer = &req;
            p = va2()(NULL, NULL, size, MEM_RESERVE | X_RESERVE_PLACEHOLDER, PAGE_NOACCESS, &ep, 1);
            if (!p) slog_("placeholder below 4 GB failed (size, error) - falling back to anywhere", 0, size, GetLastError(), 0, 0, 0, 0);
        }
        if (!p) p = va2()(NULL, addr, size, MEM_RESERVE | X_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
        SLOG("XMemVirtualAlloc[placeholder]", addr, size, type, a4, a5, p); return p;
    }
    if ((type & X_REPLACE_PLACEHOLDER) && va2()) {
        if (prot == PAGE_NOACCESS) prot = PAGE_READWRITE;
        void* p = va2()(NULL, addr, size, MEM_RESERVE | MEM_COMMIT | X_REPLACE_PLACEHOLDER, prot, NULL, 0);
        SLOG("XMemVirtualAlloc[replace-placeholder+commit]", addr, size, type, a4, a5, p ? (uint64_t)p : 0xDEAD0000ull | GetLastError()); return p;
    }
    DWORD t = (DWORD)type & (MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN | MEM_RESET); if (!t) t = MEM_COMMIT | MEM_RESERVE;
    static volatile LONG nphys; int physno = (type & MEM_PHYSICAL) && !(t & MEM_COMMIT) ? (int)InterlockedIncrement(&nphys) - 1 : -1;
    /* which MEM_PHYSICAL reservations (in creation order) become pool placeholders. Default leaves out #0 (24 GB, attrs 0x21):
       with it in the pool the title crashes after ~40 s (exe+0x258B000 / exe+0x247A098); cause not found yet. */
    char msk[20]; uint32_t physmask = GetEnvironmentVariableA("GTA6_GPUMEM_MASK", msk, sizeof msk) ? (uint32_t)strtoul(msk, NULL, 16) : 0xFFFFFFFEu;
    if (gpumem_on() && physno >= 0 && physno < 32 && ((physmask >> physno) & 1) && va2() && !GetEnvironmentVariableA("GTA6_GPUMEM_NORES", NULL, 0)) {
        /* GPU-visible range: a placeholder, so physical pool pages can be mapped into it page by page */
        void* p = va2()(NULL, addr, size, MEM_RESERVE | X_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
        if (p) { res_add(p, size, 1); SLOG("XMemVirtualAlloc[gpu placeholder]", addr, size, type, a4, a5, p); return p; }
    }
    /* Committed allocations with PAGE_GRAPHICS_* protection (0x800..0x10000: GPU readable/writable) hold vertex/index data,
       upload rings and other buffers the GPU reads: back them with pool pages up front so the real GPU sees the same bytes.
       GTA6_GPUMEM_NOVA=1 keeps them as ordinary private memory. */
    if (gpumem_on() && !addr && (t & MEM_COMMIT) && (t & MEM_RESERVE) && ((uint32_t)a5 & 0x1F800) && va2() && !GetEnvironmentVariableA("GTA6_GPUMEM_NOVA", NULL, 0)) {
        size_t sz = (size + 0xFFFF) & ~(size_t)0xFFFF;
        uint8_t* p = (uint8_t*)va2()(NULL, NULL, sz, MEM_RESERVE | X_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
        if (p) {
            size_t o = 0; for (; o < sz; o += 0x10000) if (!gm_map_page((uint64_t)p + o, (uint64_t)InterlockedIncrement64(&g_nextpfn))) break;
            if (o == sz) { res_add(p, sz, 1); SLOG("XMemVirtualAlloc[gpu-visible commit -> pool]", addr, size, type, a4, a5, p); return p; }
            slog_("gpu-visible commit: pool mapping failed part way (size, mapped) - range kept, rest lazily mapped", 0, sz, o, 0, 0, 0, 0);
            res_add(p, sz, 1); return p;
        }
    }
    if (gpumem_on() && addr && (t & MEM_COMMIT)) {          /* commit inside a GPU placeholder range: map pool pages there */
        uint64_t a0 = (uint64_t)addr & ~0xFFFFull, a1 = ((uint64_t)addr + size + 0xFFFF) & ~0xFFFFull; LONG n = g_nres; if (n > 256) n = 256;
        for (LONG i = 0; i < n; i++) if (g_res[i].phys && a0 >= g_res[i].base && a1 <= g_res[i].base + g_res[i].size) {
            for (uint64_t va = a0; va < a1; va += 0x10000) if (!gm_mapped(va)) gm_map_page(va, (uint64_t)InterlockedIncrement64(&g_nextpfn));
            SLOG("XMemVirtualAlloc[commit in gpu range]", addr, size, type, a4, a5, addr); return addr;
        }
    }
    void* p = VirtualAlloc(addr, size, t, prot);
    if (p && !(t & MEM_COMMIT)) res_add(p, size, 0);       /* reserve-only: back pages on first touch */
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
/* physical pages are 64 KB units; we hand out page numbers and commit memory when they get mapped (pool pages with GTA6_GPUMEM) */
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
    if (c && b && gpumem_on()) {
        uint64_t okc = 0;
        for (uint64_t i = 0; i < b; i++) {
            uint64_t pfn = ((uint64_t*)c)[i], va = a + (i << 16);
            if (pfn < GM_PFN0 || pfn > (uint64_t)g_nextpfn) pfn = (uint64_t)InterlockedIncrement64(&g_nextpfn);
            if (gm_map_page(va, pfn)) okc++;
            else if (!VirtualAlloc((void*)va, 0x10000, MEM_COMMIT, PAGE_READWRITE)) VirtualAlloc((void*)va, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        }
        SLOG("XMemMapPhysicalPages[gpu pool] (va, pages, gpu-visible)", a, b, okc, 0, 0, a);
        return a;
    }
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
    {   static volatile LONG nd; if (InterlockedIncrement(&nd) <= 12) { char h1[120], h2[120]; hexd(h1, 120, a, 6); hexd(h2, 120, pages, 6);
        slog_("Scatter args dump: a / pages contents follow", 0, a, count, pages, d, e, 0); slog_(h1, 0, 0, 0, 0, 0, 0, 0); slog_(h2, 0, 0, 0, 0, 0, 0, 0);
        if (isarray && pages && count > 1) { uint64_t* A = (uint64_t*)a; uint64_t* P = (uint64_t*)pages; uint64_t brk = 0, unal = 0, pdiff = 0, mn = ~0ull, mx = 0, pmn = ~0ull, pmx = 0;
            for (uint64_t i = 0; i < count; i++) { if (i && A[i] != A[i - 1] + 0x10000) brk++; if (A[i] & 0xFFFF) unal++; if (P[i] != P[0]) pdiff++;
                if (A[i] < mn) mn = A[i]; if (A[i] > mx) mx = A[i]; if (P[i] < pmn) pmn = P[i]; if (P[i] > pmx) pmx = P[i]; }
            slog_("Scatter analysis: va breaks, unaligned, pages!=first, va min, va max, page max", 0, brk, unal, pdiff, mn, mx, pmx);
            if (brk) for (uint64_t i = 1, k = 0; i < count && k < 6; i++) if (A[i] != A[i - 1] + 0x10000) { slog_("Scatter va break at index", 0, i, A[i - 1], A[i], P[i - 1], P[i], 0); k++; } } } }
    for (uint64_t i = 0; i < count && i < (1u << 20); i++) {
        uint64_t va = isarray ? ((uint64_t*)a)[i] : a + (i << 16);
        if (!pages || !va) continue;
        if (gpumem_on()) {
            uint64_t pfn = ((uint64_t*)pages)[i];
            /* every entry naming one page = a shared backing page; unless GTA6_GPUMEM_ALIAS, give each VA its own page (old behaviour) */
            /* A page repeated across neighbouring entries is a shared "null" page for unbacked sparse tiles. In our page numbering
               it lands on a live page of another region and aliasing it corrupts that page (crash at exe+0xB5A5F8), so each such
               entry gets its own zeroed page. Real pages (the rest of the array) keep true aliasing. */
            if ((i > 0 && ((uint64_t*)pages)[i - 1] == pfn) || (i + 1 < count && ((uint64_t*)pages)[i + 1] == pfn)) pfn = 0;
            if (pfn < GM_PFN0 || pfn > (uint64_t)g_nextpfn) pfn = (uint64_t)InterlockedIncrement64(&g_nextpfn);
            if (gm_map_page(va & ~0xFFFFull, pfn)) continue;
        }
        VirtualAlloc((void*)va, 0x10000, MEM_COMMIT, PAGE_READWRITE);
    }
    SLOG(isarray ? "XMemMapPhysicalPagesScatter[array]" : "XMemMapPhysicalPagesScatter[range]", a, count, pages, d, e, 1);
    return 1;
}
STUB(XMemTransferMemory, 0)
STUB(XMemIsToolsMemAvailable, 0)
STUB(XMemGetWorkingSetStatistics, 0)
/* Kernel VirtualAlloc/VirtualFree from the title (IAT hooks, see fsredir.h): inside GPU pool ranges a commit maps pool pages
   (VirtualAlloc cannot commit into a placeholder) and decommit/release keep the pages (like the in-place commit path did). */
static int gm_phys_range(uint64_t a0, uint64_t a1) {
    LONG n = g_nres; if (n > 256) n = 256;
    for (LONG i = 0; i < n; i++) if (g_res[i].phys && a0 >= g_res[i].base && a1 <= g_res[i].base + g_res[i].size) return 1;
    return 0;
}
static LPVOID WINAPI h_VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD prot) {
    if (gpumem_on() && addr && size && (type & MEM_COMMIT)) {
        uint64_t a0 = (uint64_t)addr & ~0xFFFFull, a1 = ((uint64_t)addr + size + 0xFFFF) & ~0xFFFFull;
        if (gm_phys_range(a0, a1)) {
            for (uint64_t va = a0; va < a1; va += 0x10000) if (!gm_mapped(va)) gm_map_page(va, (uint64_t)InterlockedIncrement64(&g_nextpfn));
            SLOG("VirtualAlloc[title, commit in gpu range]", addr, size, type, prot, 0, addr);
            return addr;
        }
    }
    LPVOID r = VirtualAlloc(addr, size, type, prot);
    if (!r && addr) SLOG("VirtualAlloc[title] FAILED", addr, size, type, prot, GetLastError(), 0);
    return r;
}
static BOOL WINAPI h_VirtualFree(LPVOID addr, SIZE_T size, DWORD type) {
    if (gpumem_on() && addr) {
        uint64_t a = (uint64_t)addr;
        if (gm_phys_range(a, a + (size ? size : 1))) { SLOG("VirtualFree[title, gpu range: pages kept]", addr, size, type, 0, 0, 1); return TRUE; }
    }
    return VirtualFree(addr, size, type);
}
/* GPU-visible scratch memory for the D3D12 stand-in (per-draw tables it builds itself, e.g. vertex buffer descriptors):
   a placeholder range below 4 GB backed page by page with pool pages, so the GPU reads it through the page table. */
EXP void* xmem_gpu_scratch(size_t size) {
    if (!gpumem_on() || !va2()) return NULL;
    size = (size + 0xFFFF) & ~(size_t)0xFFFF;
    MEM_ADDRESS_REQUIREMENTS req = { 0 }; req.LowestStartingAddress = (PVOID)0x80000000ull; req.HighestEndingAddress = (PVOID)0xFFFFFFFFull;
    MEM_EXTENDED_PARAMETER ep = { 0 }; ep.Type = MemExtendedParameterAddressRequirements; ep.Pointer = &req;
    uint8_t* p = (uint8_t*)va2()(NULL, NULL, size, MEM_RESERVE | X_RESERVE_PLACEHOLDER, PAGE_NOACCESS, &ep, 1);
    if (!p) p = (uint8_t*)va2()(NULL, NULL, size, MEM_RESERVE | X_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
    if (!p) return NULL;
    for (size_t o = 0; o < size; o += 0x10000) if (!gm_map_page((uint64_t)p + o, (uint64_t)InterlockedIncrement64(&g_nextpfn))) { SLOG("xmem_gpu_scratch: map failed", p, o, size, 0, 0, 0); return NULL; }
    SLOG("xmem_gpu_scratch (size, address)", size, 0, 0, 0, 0, p);
    return p;
}
