/* Xbox D3D12 (d3d12_xs.dll) stand-in.
   D3D12XboxCreateDevice returns a PROBE device. Every object is a zeroed 64 KB block whose vtable has 1024 logging slots.
   Any (riid, ppv) pair in a call gets a fresh probe object written to *ppv, so the game keeps going and we can map
   which interfaces/slots it uses. Nothing is rendered. */
#define STUBDLL "d3d12_xs"
#include "stub.h"
#include <stdarg.h>
#include <string.h>

STUB(D3D12ConfigureVirtualMemoryX, 0)
static void dlog(const char* fmt, ...);
static const char* hx(uint64_t v, int bytes);
/* These two have the same signature in the real PC d3d12.dll (blob in, blob out), so forward them. */
static void* pc_d3d12(const char* name) {
    static HMODULE h; if (!h) h = LoadLibraryA("d3d12.dll");
    return h ? (void*)GetProcAddress(h, name) : NULL;
}
EXP uint64_t D3D12SerializeVersionedRootSignature(void* desc, void** ppBlob, void** ppErr) {
    typedef long (WINAPI *F)(void*, void**, void**);
    F f = (F)pc_d3d12("D3D12SerializeVersionedRootSignature");
    uint64_t r = f ? (uint64_t)(uint32_t)f(desc, ppBlob, ppErr) : E_NOTIMPL_;
    if ((int32_t)r < 0 && f) {
        /* Retry with only the flag bits the PC runtime knows (Xbox adds its own), and with version 1.1 if needed. */
        uint32_t* d = (uint32_t*)desc; uint32_t ver = d[0], oldflags = d[10];
        d[10] = oldflags & 0xFFF;
        r = (uint64_t)(uint32_t)f(desc, ppBlob, ppErr);
        if ((int32_t)r < 0 && ver > 2) { d[0] = 2; r = (uint64_t)(uint32_t)f(desc, ppBlob, ppErr); d[0] = ver; }
        d[10] = oldflags;
        static volatile LONG c; if (InterlockedIncrement(&c) <= 6) {
            dlog("SerializeVersionedRootSignature retry: ver=0x%X flags=0x%X -> 0x%llX desc=%s params=%s", ver, oldflags, r, hx((uint64_t)desc, 48), hx(((uint64_t*)desc)[2], 64));
        }
    }
    SLOG("D3D12SerializeVersionedRootSignature[forwarded to PC d3d12]", desc, ppBlob, ppErr, 0, 0, r); return r;
}
EXP uint64_t D3D12CreateRootSignatureDeserializer(void* data, size_t size, const GUID* riid, void** ppv) {
    typedef long (WINAPI *F)(void*, size_t, const GUID*, void**);
    F f = (F)pc_d3d12("D3D12CreateRootSignatureDeserializer");
    uint64_t r = f ? (uint64_t)(uint32_t)f(data, size, riid, ppv) : E_NOTIMPL_;
    SLOG("D3D12CreateRootSignatureDeserializer[forwarded to PC d3d12]", data, size, riid, ppv, 0, r); return r;
}

static void dlog(const char* fmt, ...) {
    char path[MAX_PATH], buf[2048];
    if (!GetEnvironmentVariableA("GTA6_STUB_LOG", path, MAX_PATH)) return;
    int n = snprintf(buf, 64, "%llu [d3d12_xs] ", GetTickCount64());
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
static int writable(uint64_t p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!readable(p, n) || !VirtualQuery((void*)p, &mbi, sizeof mbi)) return 0;
    return (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY)) != 0;
}
static const char* hx(uint64_t v, int bytes) {
    static __declspec(thread) char s[8][200]; static __declspec(thread) int k; char* o = s[k++ % 8]; o[0] = 0;
    if (!readable(v, bytes)) return o;
    size_t n = 0; const unsigned char* p = (const unsigned char*)v;
    n += snprintf(o + n, 200 - n, "[");
    for (int i = 0; i < bytes && n + 4 < 200; i++) n += snprintf(o + n, 200 - n, "%02X%s", p[i], (i % 8 == 7) ? " " : "");
    snprintf(o + n, 200 - n, "]");
    return o;
}

/* ---- probe objects ---- */
#define NPROBE 1024
#define MAXOBJ (1 << 20)
#define OBJSIZE 0x10000            /* address-space granule per object; only the first 4 KB is committed */
#define OBJCOMMIT 0x1000
#define OBJMAGIC 0x45424F5250424F50ull   /* "POBPRBOE" tag at +0xFF0, object id at +0xFF8 */
static void* g_dvt[NPROBE];
enum { KIND_GENERIC, KIND_DXGIDEV, KIND_ADAPTER, KIND_OUTPUT, KIND_FACTORY };
typedef struct { void* p; int parent, slot, kind; GUID iid; void* aux; void* meta; volatile LONG calls; } ObjInfo;
/* ---- command recording (emulator step 1) ----
   Command lists keep a record buffer: {u32 slot, u32 nextra, u64 a[6]} followed by nextra extra dwords (copied data).
   Pipeline objects keep meta = PsoMeta (shader code, size, hash, copy of the creation desc). */
typedef struct { uint64_t code, size, hash; uint8_t desc[0x180]; } PsoMeta;
typedef struct { uint8_t* buf; size_t used, cap; } CmdRec;
typedef struct { uint32_t slot, nextra; uint64_t a[6]; } RecHdr;
static ObjInfo* g_obj;             /* MAXOBJ entries, allocated on first use */
static void* g_dxgidev;
static volatile LONG g_nobj, g_total;
static uint64_t g_exe, g_exeend;

static int objid(uint64_t p) {
    if (!p || (p & (OBJSIZE - 1)) || (p >> 47) || !g_obj) return -1;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void*)p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || mbi.RegionSize < OBJCOMMIT) return -1;
    if (*(uint64_t*)(p + 0xFF0) != OBJMAGIC) return -1;
    uint64_t id = *(uint64_t*)(p + 0xFF8);
    return (id < (uint64_t)g_nobj && id < MAXOBJ && g_obj[id].p == (void*)p) ? (int)id : -1;
}
static void* newobj(int parent, int slot, const GUID* iid) {
    if (!g_obj) {
        ObjInfo* t = (ObjInfo*)VirtualAlloc(NULL, sizeof(ObjInfo) * (size_t)MAXOBJ, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (InterlockedCompareExchangePointer((void* volatile*)&g_obj, t, NULL) != NULL) VirtualFree(t, 0, MEM_RELEASE);
    }
    LONG id = InterlockedIncrement(&g_nobj) - 1;
    if (id >= MAXOBJ) return NULL;
    uint8_t* p = (uint8_t*)VirtualAlloc(NULL, OBJSIZE, MEM_RESERVE, PAGE_NOACCESS);
    if (!p || !VirtualAlloc(p, OBJCOMMIT, MEM_COMMIT, PAGE_READWRITE)) return NULL;
    *(void***)p = g_dvt;
    *(uint64_t*)(p + 0xFF0) = OBJMAGIC; *(uint64_t*)(p + 0xFF8) = (uint64_t)id;
    g_obj[id].parent = parent; g_obj[id].slot = slot; if (iid) g_obj[id].iid = *iid;
    MemoryBarrier(); g_obj[id].p = p;
    return p;
}
/* true for addresses inside any loaded module image (exe or a bundled DLL) - where interface IDs live */
/* Rough byte size of a public D3D12_RESOURCE_DESC (56 bytes): buffers = Width; textures = W*H*depth*4, *4/3 when mipped. 64 KB aligned. */
/* bits per pixel for a DXGI format; negative = block-compressed (bytes per 4x4 block) */
static int fmt_bpp(uint32_t f) {
    if (f >= 1 && f <= 4) return 128;   if (f >= 5 && f <= 8) return 96;
    if (f >= 9 && f <= 22) return 64;   if (f >= 23 && f <= 47) return 32;
    if (f >= 48 && f <= 59) return 16;  if (f >= 60 && f <= 65) return 8;
    if (f == 66) return 1;              if (f >= 67 && f <= 69) return 32;
    if ((f >= 70 && f <= 72) || (f >= 79 && f <= 81)) return -8;     /* BC1, BC4 */
    if ((f >= 73 && f <= 78) || (f >= 82 && f <= 84) || (f >= 94 && f <= 99)) return -16; /* BC2/3/5/6H/7 */
    if (f >= 85 && f <= 86) return 16;  if (f >= 87 && f <= 93) return 32;
    if (f == 115) return 16;
    return 32;
}
/* byte size of a public D3D12_RESOURCE_DESC (56 bytes) with its full mip chain, 64 KB aligned */
static uint64_t res_size(uint64_t descp) {
    if (!readable(descp, 56)) return 0x10000;
    const uint8_t* d = (const uint8_t*)descp;
    uint32_t dim = *(const uint32_t*)d; uint64_t w = *(const uint64_t*)(d + 16); uint32_t h = *(const uint32_t*)(d + 24);
    uint32_t depth = *(const uint16_t*)(d + 28), mips = *(const uint16_t*)(d + 30), fmt = *(const uint32_t*)(d + 32);
    uint32_t samples = *(const uint32_t*)(d + 36);
    uint64_t sz = 0;
    if (dim == 1) sz = w;
    else {
        if (!h) h = 1; if (!depth) depth = 1; if (!samples) samples = 1;
        if (!mips) { mips = 1; for (uint64_t m = (w > h ? w : h); m > 1; m >>= 1) mips++; }
        int bpp = fmt_bpp(fmt);
        uint32_t slices = (dim == 4) ? 1 : depth;
        for (uint32_t i = 0; i < mips && i < 16; i++) {
            uint64_t mw = w >> i, mh = h >> i, md = (dim == 4) ? (depth >> i) : 1;
            if (!mw) mw = 1; if (!mh) mh = 1; if (!md) md = 1;
            uint64_t lvl = (bpp < 0) ? ((mw + 3) / 4) * ((mh + 3) / 4) * (uint64_t)(-bpp) : (mw * mh * (uint64_t)bpp + 7) / 8;
            sz += ((lvl * md + 255) & ~255ull);        /* rows/levels padded to 256 bytes like the PC runtime */
        }
        sz *= slices * samples;
    }
    if (sz > (8ull << 30)) sz = 0x10000;                       /* garbage desc guard */
    return (sz + 0xFFFF) & ~0xFFFFull;
}
/* resource memory goes through xmem's XMemAlloc (and so through the title's allocation hooks), allocator id 0x80 = D3D */
#define D3D_ALLOC_ATTRS 0x808000ull
static void* xmem_alloc(size_t size, uint64_t attrs) {
    typedef void* (*F)(size_t, uint64_t); static F f;
    if (!f) f = (F)GetProcAddress(GetModuleHandleA("xmem.dll"), "XMemAlloc");
    return f ? f(size, attrs) : NULL;
}
static int in_exe(uint64_t p) {
    if (p >= g_exe && p < g_exeend) return 1;
    MEMORY_BASIC_INFORMATION mbi;
    if (p < 0x10000 || (p >> 47) || !VirtualQuery((void*)p, &mbi, sizeof mbi)) return 0;
    return mbi.State == MEM_COMMIT && mbi.Type == MEM_IMAGE;
}

/* Show the presented back buffer in the game window (~30 Hz) with a status line. The buffer is read as 32-bit
   BGRA rows; on the console it may be tiled/another format, and nothing renders into it yet, so expect black. */
static void show_frame(void* pixels, uint32_t w, uint32_t h, LONG frame) {
    static ULONGLONG last; static HWND hwnd; static ULONGLONG t0; static LONG f0;
    ULONGLONG now = GetTickCount64();
    if (now - last < 33) return;
    {   /* while the intro video is being drawn by xmem's Bink viewer, don't paint over it */
        typedef ULONGLONG (*VT)(void); static VT vt; if (!vt) vt = (VT)GetProcAddress(GetModuleHandleA("xmem.dll"), "GTA6_VideoTick");
        if (vt && now - vt() < 300) return;
    }
    last = now;
    if (!hwnd) {
        char hb[32]; if (GetEnvironmentVariableA("GTA6_HWND", hb, sizeof hb)) hwnd = (HWND)(uintptr_t)_strtoui64(hb, NULL, 16);
        if (!hwnd) hwnd = FindWindowA(NULL, "Americas");
        if (!hwnd) return;
        dlog("show_frame: drawing into window %p", hwnd);
        t0 = now; f0 = frame;
    }
    RECT rc; if (!GetClientRect(hwnd, &rc)) return;
    HDC dc = GetDC(hwnd); if (!dc) return;
    if (pixels && readable((uint64_t)pixels, (size_t)w * 4)) {
        BITMAPINFO bi = { 0 }; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = (LONG)w; bi.bmiHeader.biHeight = -(LONG)h;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, HALFTONE);
        StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, w, h, pixels, &bi, DIB_RGB_COLORS, SRCCOPY);
    } else { HBRUSH b = (HBRUSH)GetStockObject(BLACK_BRUSH); FillRect(dc, &rc, b); }
    char s[300]; double secs = (now - t0) / 1000.0;
    int n = snprintf(s, sizeof s, "GTA6-BootTest   frame %ld   %.0f fps (fake GPU, instant)   back buffer %ux%u   GPU commands are NOT executed yet - image stays black   [Alt+F4 to close]",
        frame, secs > 0.5 ? (frame - f0) / secs : 0.0, w, h);
    SetBkMode(dc, OPAQUE); SetBkColor(dc, RGB(0, 0, 0)); SetTextColor(dc, RGB(255, 210, 0));
    TextOutA(dc, 16, 16, s, n);
    ReleaseDC(hwnd, dc);
}
/* Walk one recorded command list, tracking compute state; log each new (shader, dispatch) combination once.
   This is where the interpreter will execute dispatches. */
static void replay_list(int lid) {
    if (lid <= 0 || !g_obj[lid].meta) return;
    CmdRec* r = (CmdRec*)g_obj[lid].meta;
    int pso = -1, rootsig = -1; uint64_t tables[16] = { 0 }, views[16] = { 0 }, heaps[2] = { 0 }; uint32_t consts[16][64]; uint32_t cmask[16] = { 0 };
    static volatile LONG logged; static uint64_t seen[4096]; static LONG nseen;
    for (size_t off = 0; off + sizeof(RecHdr) <= r->used;) {
        RecHdr* h = (RecHdr*)(r->buf + off); const uint32_t* ex = (const uint32_t*)(h + 1);
        switch (h->slot) {
        case 25: pso = objid(h->a[0]); break;
        case 29: rootsig = objid(h->a[0]); break;
        case 28: for (uint32_t i = 0; i < h->nextra / 2 && i < 2; i++) heaps[i] = ((const uint64_t*)ex)[i]; break;
        case 31: if (h->a[0] < 16) tables[h->a[0]] = h->a[1]; break;
        case 37: case 39: case 41: if (h->a[0] < 16) views[h->a[0]] = h->a[1]; break;
        case 33: if (h->a[0] < 16 && h->a[2] < 64) { consts[h->a[0]][h->a[2]] = (uint32_t)h->a[1]; cmask[h->a[0]] |= 1; } break;
        case 35: if (h->a[0] < 16) for (uint32_t i = 0; i < h->nextra && h->a[3] + i < 64; i++) { consts[h->a[0]][h->a[3] + i] = ex[i]; cmask[h->a[0]] |= 1; } break;
        case 14: {
            static volatile LONG seqdump;
            if (InterlockedIncrement(&seqdump) <= 2) {               /* full call sequence of this list up to the dispatch */
                dlog("SEQ list obj#%d up to dispatch #%ld:", lid, seqdump);
                for (size_t o2 = 0; o2 + sizeof(RecHdr) <= off;) {
                    RecHdr* q = (RecHdr*)(r->buf + o2); const uint32_t* qx = (const uint32_t*)(q + 1);
                    char ex2[200] = ""; size_t k2 = 0; for (uint32_t j = 0; j < q->nextra && j < 16 && k2 < 180; j++) k2 += snprintf(ex2 + k2, sizeof ex2 - k2, "%X ", qx[j]);
                    dlog("   m%u (%llX, %llX, %llX, %llX, %llX) %s%s", q->slot, q->a[0], q->a[1], q->a[2], q->a[3], q->a[4], q->nextra ? "data: " : "", ex2);
                    o2 += sizeof(RecHdr) + (size_t)q->nextra * 4;
                }
            }
            PsoMeta* m = (pso > 0) ? (PsoMeta*)g_obj[pso].meta : NULL;
            uint64_t key = (m ? m->hash : 0) ^ ((uint64_t)(uint32_t)h->a[0] << 40) ^ ((uint64_t)(uint32_t)h->a[1] << 20) ^ (uint32_t)h->a[2];
            int isnew = 1; for (LONG i = 0; i < nseen && i < 4096; i++) if (seen[i] == key) { isnew = 0; break; }
            if (isnew && nseen < 4096) seen[nseen++] = key;
            if (isnew && InterlockedIncrement(&logged) <= 400) {
                char cs[400] = ""; size_t k = 0;
                for (int ri = 0; ri < 16 && k < 300; ri++) if (cmask[ri]) { k += snprintf(cs + k, sizeof cs - k, " r%d:[", ri); for (int j = 0; j < 8; j++) k += snprintf(cs + k, sizeof cs - k, "%X ", consts[ri][j]); k += snprintf(cs + k, sizeof cs - k, "]"); }
                dlog("DISPATCH %ux%ux%u shader=%016llX(%llu bytes) pso=obj#%d rootsig=obj#%d heaps=%llX,%llX tables=%llX,%llX,%llX views=%llX,%llX,%llX%s",
                    (uint32_t)h->a[0], (uint32_t)h->a[1], (uint32_t)h->a[2], m ? m->hash : 0, m ? m->size : 0, pso, rootsig, heaps[0], heaps[1],
                    tables[0], tables[1], tables[2], views[0], views[1], views[2], cs);
            }
            break; }
        }
        off += sizeof(RecHdr) + (size_t)h->nextra * 4;
    }
}
static volatile LONG g_kcnt[64][NPROBE];
static volatile uint32_t g_kiid[64];
/* every 5 s: which (interface, method) pairs were called and how often in that window */
static DWORD WINAPI rate_dumper(void* unused) {
    static LONG last[64][NPROBE];
    for (;;) {
        Sleep(5000);
        char line[1500]; size_t k = 0;
        for (int i = 0; i < 64; i++) for (int s = 0; s < NPROBE; s++) {
            LONG c = g_kcnt[i][s], d = c - last[i][s];
            if (d >= 5 && k < 1400) k += snprintf(line + k, sizeof line - k, " %08X.m%d=%ld", g_kiid[i], s, d);
            last[i][s] = c;
        }
        if (k) dlog("RATE(5s):%s", line);
    }
    return 0;
}
static uint64_t dprobe(int slot, uint64_t rcx, uint64_t a[11], void* ra) {
    /* normal call: rcx = this. struct-return call: rcx = return buffer, a[0] = this. */
    int id = objid(rcx), sret = 0; uint64_t* arg = a;
    if (id < 0 && objid(a[0]) >= 0) { id = objid(a[0]); sret = 1; arg = a + 1; }
    int nargs = sret ? 10 : 11;
    if (slot == 1 || slot == 2) return 1;                       /* AddRef / Release */
    char made[200] = "";
    /* ---- DXGI chain: device.QI(IDXGIDevice) -> GetAdapter -> EnumOutputs(0) -> display mode list ---- */
    {
        int kind = id >= 0 ? g_obj[id].kind : KIND_GENERIC; uint64_t r = 0; int handled = 0; void* o = NULL; const char* what = "";
        if (slot == 0 && in_exe(arg[0]) && readable(arg[0], 16) && ((const GUID*)arg[0])->Data1 == 0x54EC77FA && writable(arg[1], 8)) {
            if (!g_dxgidev) { g_dxgidev = newobj(id, slot, (const GUID*)arg[0]); g_obj[objid((uint64_t)g_dxgidev)].kind = KIND_DXGIDEV; }
            o = g_dxgidev; *(void**)arg[1] = o; handled = 1; what = "QI(IDXGIDevice)";
        } else if (kind == KIND_DXGIDEV && slot == 7 && writable(arg[0], 8)) {
            o = newobj(id, slot, NULL); g_obj[objid((uint64_t)o)].kind = KIND_ADAPTER; *(void**)arg[0] = o; handled = 1; what = "DXGIDevice.GetAdapter";
        } else if (kind == KIND_ADAPTER && slot == 7 && writable(arg[1], 8)) {
            if ((uint32_t)arg[0] == 0) { o = newobj(id, slot, NULL); g_obj[objid((uint64_t)o)].kind = KIND_OUTPUT; *(void**)arg[1] = o; } else { *(void**)arg[1] = NULL; r = 0x887A0002ull; }
            handled = 1; what = "Adapter.EnumOutputs";
        } else if (kind == KIND_OUTPUT && slot == 21) {
            if (writable(arg[2], 4)) *(uint32_t*)arg[2] = 0;      /* zero display modes: game falls back to 60 Hz */
            handled = 1; what = "Output.GetDisplayModeList -> 0 modes";
        } else if (kind != KIND_GENERIC && slot == 6 && in_exe(arg[0]) && writable(arg[1], 8)) {
            o = newobj(id, slot, (const GUID*)arg[0]); g_obj[objid((uint64_t)o)].kind = KIND_FACTORY; *(void**)arg[1] = o; handled = 1; what = "DXGI GetParent";
        }
        if (handled) {
            dlog("obj#%d kind%d slot%d %s (0x%llX, 0x%llX, 0x%llX, 0x%llX) -> 0x%llX obj=%p(#%d) caller=exe+0x%llX", id, kind, slot, what, arg[0], arg[1], arg[2], arg[3], r, o, objid((uint64_t)o), (uint64_t)ra - g_exe);
            return r;
        }
    }
    /* any (riid in exe image, writable ppv) pair -> hand back a new probe object */
    /* only when *ppv is still NULL: stale (riid, ppv) values left in registers by an earlier call must not trigger this.
       Known non-creating methods on child objects (Close/Reset/etc. on command lists) are excluded outright. */
    /* Device creation methods: index of the riid argument (ppv follows it). Slots 0-43 follow the public ID3D12Device
       layout; 48 and 73 are Xbox-specific creators observed at runtime. -1 = not a creator. */
    int known = -1;
    if (slot == 0 || (id > 0 && slot == 7)) known = 0;
    else if (id == 0) switch (slot) {
        case 8: case 9: case 10: case 11: case 14: case 28: case 39: case 48: known = 1; break;
        case 36: case 41: known = 2; break;
        case 16: case 30: case 73: known = 3; break;
        case 12: known = 4; break;
        case 27: case 29: known = 5; break;
        case 127: known = 8; break;                  /* Xbox CreateCommittedResource variant: (heapProps, flags, desc, state, clear, 0, 0, 0, riid, ppv) */
        case 133: known = 4; break;                  /* Xbox placed-resource creator: (addr, desc, ?, ?, riid, ppv) */
        case 62: known = 3; break;                   /* Xbox pipeline-state creator: (desc, 0, 0, riid, ppv) */
        case 55: known = 3; break;                   /* Xbox creator: (?, ?, desc, riid, ppv), result cached by the title */
        case 57: case 67: case 86: known = 1; break; /* Xbox pipeline-state creators: (desc, riid=ID3D12PipelineState, ppv) */
        case 78: case 79: known = 4; break;          /* Xbox command-allocator creators: (?, ?, ?, ?, riid, ppv) */
        default: known = -1;                         /* unknown slots never create (scanning wrote into the title's heap once) */
    }
    for (int i = (known >= 0 ? known : 0); i + 1 < nargs && known != -1; i++) {
        if (known >= 0 && i != known) break;
        if (in_exe(arg[i]) && readable(arg[i], 16) && writable(arg[i + 1], 8)) {
            const GUID* g = (const GUID*)arg[i];
            if (g->Data1 == 0 || (uint32_t)g->Data1 == 0xFFFFFFFF) continue;
            /* QueryInterface returns self; GetDevice (slot 7 on any child) returns the device */
            void* o = (slot == 0 && id >= 0) ? g_obj[id].p : (id > 0 && slot == 7) ? g_obj[0].p : newobj(id, slot, g);
            *(void**)arg[i + 1] = o;
            int no = objid((uint64_t)o);
            /* heaps (slot 28): own memory; placed resources: heap memory + offset (slot 29) or a title-given address (Xbox slot 133) */
            if (id == 0 && slot == 28 && no > 0 && readable(arg[0], 8)) {
                uint64_t hs = (*(uint64_t*)arg[0] + 0xFFFF) & ~0xFFFFull; if (!hs) hs = 0x10000;
                g_obj[no].aux = VirtualAlloc(NULL, (size_t)hs, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            }
            if (id == 0 && slot == 29 && no > 0) { int h = objid(arg[0]); if (h > 0 && g_obj[h].aux) g_obj[no].aux = (uint8_t*)g_obj[h].aux + arg[1]; }
            if (id == 0 && slot == 133 && no > 0) g_obj[no].aux = (void*)arg[0];
            /* committed resources (public slot 27, Xbox slot 127): allocate their memory like the console driver does */
            if (id == 0 && (slot == 27 || slot == 127) && objid((uint64_t)o) > 0) {
                uint64_t sz = res_size(arg[2]);
                g_obj[objid((uint64_t)o)].aux = xmem_alloc((size_t)sz, D3D_ALLOC_ATTRS);
                dlog("  resource obj#%d size=0x%llX -> memory %p (via XMemAlloc attrs 0x%llX)", objid((uint64_t)o), sz, g_obj[objid((uint64_t)o)].aux, D3D_ALLOC_ATTRS);
            }
            /* descriptor heap: give it backing memory so its start handles are real, distinct addresses */
            if (id == 0 && slot == 14 && readable(arg[0], 16) && objid((uint64_t)o) > 0) {
                uint32_t num = ((uint32_t*)arg[0])[1]; if (!num) num = 1;
                /* Xbox extension: the description is followed by a title-supplied address (+0x10) and size (+0x18).
                   When the address is set, the heap must start exactly there. */
                uint64_t useraddr = readable(arg[0], 32) ? ((uint64_t*)arg[0])[2] : 0;
                g_obj[objid((uint64_t)o)].aux = (useraddr && readable(useraddr, 8)) ? (void*)useraddr
                    : VirtualAlloc(NULL, (size_t)num * 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            }
            snprintf(made, sizeof made, " => obj#%d %p iid={%08lX-%04X-%04X}", objid((uint64_t)o), o, g->Data1, g->Data2, g->Data3);
            break;
        }
    }
    uint64_t ret = 0;
    if (id == 0 && slot == 15 && !sret) ret = 32;               /* GetDescriptorHandleIncrementSize */
    if (id == 0 && slot == 7 && !sret) ret = 1;                 /* GetNodeCount */
    int isheap = (id > 0 && g_obj[id].iid.Data1 == 0x8EFB471D);   /* ID3D12DescriptorHeap */
    int isres = (id > 0 && g_obj[id].iid.Data1 == 0x696442BE);    /* ID3D12Resource */
    int isfence = (id > 0 && g_obj[id].iid.Data1 == 0x0A753DCF);  /* ID3D12Fence: value lives in aux */
    int isqueue = (id > 0 && g_obj[id].iid.Data1 == 0x0EC870A6);  /* ID3D12CommandQueue */
    /* fence value lives inside the fence object at +0x100 so its address can be handed out (Xbox fence slot 12) */
    #define FENCEVAL(i) (*(volatile uint64_t*)((uint8_t*)g_obj[i].p + 0x100))
    if (id == 0 && slot == 36 && !sret) {                        /* CreateFence(initial, flags, riid, ppv): seed value */
        int f = objid(writable(arg[3], 8) ? *(uint64_t*)arg[3] : 0);
        if (f > 0) FENCEVAL(f) = arg[0];
    }
    if (isfence && !sret) {
        if (slot == 8) return FENCEVAL(id);                      /* GetCompletedValue */
        if (slot == 9) {                                         /* SetEventOnCompletion(value, event): everything completes at once here */
            if (arg[1]) SetEvent((HANDLE)arg[1]);
            return 0;
        }
        if (slot == 10) { FENCEVAL(id) = arg[0]; return 0; }     /* Signal(value) from the CPU */
        if (slot == 12) return (uint64_t)&FENCEVAL(id);          /* Xbox: address of the completed value, polled by the title */
    }
    /* remember shader + desc on the created pipeline object (dev slot 67: ppv is arg[2]) */
    if (id == 0 && slot == 67 && readable(arg[0], 0x38) && writable(arg[2], 8)) {
        int po = objid(*(uint64_t*)arg[2]);
        if (po > 0 && !g_obj[po].meta) {
            PsoMeta* m = (PsoMeta*)calloc(1, sizeof(PsoMeta));
            if (m) {
                m->code = ((uint64_t*)arg[0])[5]; m->size = ((uint64_t*)arg[0])[6];
                uint64_t h = 1469598103934665603ull;
                if (m->size >= 16 && m->size <= (4u << 20) && readable(m->code, (size_t)m->size)) for (uint64_t i = 0; i < m->size; i++) { h ^= ((uint8_t*)m->code)[i]; h *= 1099511628211ull; }
                m->hash = h;
                if (readable(arg[0], sizeof m->desc)) memcpy(m->desc, (void*)arg[0], sizeof m->desc);
                g_obj[po].meta = m;
            }
        }
    }
    if (id == 0 && slot == 67 && readable(arg[0], 0x38)) {     /* dump each unique shader blob: code at desc+0x28, size at desc+0x30 */
        uint64_t code = ((uint64_t*)arg[0])[5], size = ((uint64_t*)arg[0])[6];
        if (size >= 16 && size <= (4u << 20) && readable(code, (size_t)size)) {
            uint64_t h = 1469598103934665603ull; for (uint64_t i = 0; i < size; i++) { h ^= ((uint8_t*)code)[i]; h *= 1099511628211ull; }
            static volatile LONG nd; static uint64_t seen[65536]; static SRWLOCK lk = SRWLOCK_INIT; int isnew = 1;
            AcquireSRWLockExclusive(&lk);
            for (LONG i = 0; i < nd && i < 65536; i++) if (seen[i] == h) { isnew = 0; break; }
            if (isnew && nd < 65536) seen[nd++] = h;
            ReleaseSRWLockExclusive(&lk);
            char path[MAX_PATH];
            if (isnew && GetEnvironmentVariableA("GTA6_STUB_LOG", path, MAX_PATH)) {
                char* s = strrchr(path, '\\'); if (s) { strcpy(s + 1, "shaders"); CreateDirectoryA(path, NULL); snprintf(s + 1, MAX_PATH - (s + 1 - path), "shaders\\%016llX.bin", h); }
                HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
                if (f != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(f, (void*)code, (DWORD)size, &w, NULL); CloseHandle(f); }
                /* the pipeline description (driver metadata: user-data mapping) next to it, plus what its pointers point at */
                char* dot = strrchr(path, '.'); if (dot) strcpy(dot, ".desc");
                f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
                if (f != INVALID_HANDLE_VALUE) {
                    DWORD w; uint8_t blob[0x180 + 16 * 0x100]; memset(blob, 0, sizeof blob);
                    if (readable(arg[0], 0x180)) memcpy(blob, (void*)arg[0], 0x180);
                    for (int i = 0; i < 16; i++) { uint64_t q = ((uint64_t*)blob)[i]; if (q != code && readable(q, 0x100)) memcpy(blob + 0x180 + i * 0x100, (void*)q, 0x100); }
                    WriteFile(f, blob, sizeof blob, &w, NULL); CloseHandle(f);
                }
            }
        }
    }
    if (id == 0 && (slot == 55 || slot == 57 || slot == 62 || slot == 67 || slot == 86)) {   /* pipeline creators: learn the desc layout */
        static volatile LONG pc2[128]; uint64_t dp = (slot == 55) ? arg[2] : arg[0];
        if (InterlockedIncrement(&pc2[slot]) <= 2 && readable(dp, 128)) {
            dlog("PSODESC slot%d desc=%llX %s", slot, dp, hx(dp, 64));
            dlog("PSODESC slot%d desc+64 %s", slot, hx(dp + 64, 64));
            for (int i = 0; i < 16; i++) { uint64_t q = ((uint64_t*)dp)[i]; if (readable(q, 32)) dlog("   +0x%02X -> %llX %s", i * 8, q, hx(q, 32)); }
        }
    }
    if (isqueue && !sret && slot == 32) {                        /* PresentX(planeCount, planes, params) */
        static volatile LONG pc; LONG k = InterlockedIncrement(&pc);
        /* plane0 +0x10 -> array of back-buffer resources, entry 0 is the one being presented; source rect at params +0x28 */
        if (readable(arg[1], 24)) {
            uint64_t arr = ((uint64_t*)arg[1])[2]; int ro = readable(arr, 8) ? objid(*(uint64_t*)arr) : -1;
            uint32_t w = 3840, h = 2160;
            if (readable(arg[2], 0x30)) { uint32_t* rc = (uint32_t*)(arg[2] + 0x28); if (rc[0] && rc[1] && rc[0] <= 8192 && rc[1] <= 8192) { w = rc[0]; h = rc[1]; } }
            if (ro > 0) show_frame(g_obj[ro].aux, w, h, k);
        }
        if (k <= 3 && readable(arg[1], 64)) {
            uint64_t* q = (uint64_t*)arg[1];
            dlog("PresentX#%ld planes=%u plane0=%s params=%s", k, (unsigned)arg[0], hx(arg[1], 64), hx(arg[2], 64));
            for (int i = 0; i < 8; i++) {
                int o = objid(q[i]);
                if (o > 0) dlog("   plane0 qword%d = obj#%d iid=%08lX aux=%p", i, o, g_obj[o].iid.Data1, g_obj[o].aux);
                else if (readable(q[i], 16)) { uint64_t* r2 = (uint64_t*)q[i]; int o2 = objid(r2[0]);
                    dlog("   plane0 qword%d -> %s%s", i, hx(q[i], 16), o2 > 0 ? " (first entry is a probe object)" : ""); }
            }
        }
    }
    /* ---- record compute-relevant command-list calls ---- */
    if (id > 0 && g_obj[id].iid.Data1 == 0x5B160D0F && !sret) {
        int rec = 0; uint32_t extra = 0; const void* src = NULL;
        switch (slot) {
        case 10: {                                               /* Reset(allocator, pso): start a fresh recording */
            CmdRec* r = (CmdRec*)g_obj[id].meta; if (r) r->used = 0; rec = 1; break; }
        case 14: case 25: case 28: case 29: case 31: case 33: case 37: case 39: case 41: case 9: rec = 1; break;
        default: if (slot >= 60) { rec = 1;                      /* Xbox-specific methods: keep them, plus 16 dwords behind arg[1] */
                     if (readable(arg[1], 64)) { extra = 16; src = (const void*)arg[1]; } } break;
        case 35:                                                 /* SetComputeRoot32BitConstants(root, n, data, offset): copy data */
            if ((uint32_t)arg[1] <= 64 && readable(arg[2], (size_t)(uint32_t)arg[1] * 4)) { extra = (uint32_t)arg[1]; src = (const void*)arg[2]; }
            rec = 1; break;
        case 28 + 1000: break;
        }
        if (slot == 28 && (uint32_t)arg[0] <= 2 && readable(arg[1], 16)) { extra = (uint32_t)arg[0] * 2; src = (const void*)arg[1]; }   /* heap pointers */
        if (rec) {
            CmdRec* r = (CmdRec*)g_obj[id].meta;
            if (!r) { r = (CmdRec*)calloc(1, sizeof(CmdRec)); g_obj[id].meta = r; }
            size_t need = sizeof(RecHdr) + (size_t)extra * 4;
            if (r && r->used + need > r->cap) { size_t nc = r->cap ? r->cap * 2 : 4096; while (nc < r->used + need) nc *= 2; uint8_t* nb = (uint8_t*)realloc(r->buf, nc); if (nb) { r->buf = nb; r->cap = nc; } }
            if (r && r->buf && r->used + need <= r->cap) {
                RecHdr* h = (RecHdr*)(r->buf + r->used); h->slot = (uint32_t)slot; h->nextra = extra;
                for (int i = 0; i < 6; i++) h->a[i] = arg[i];
                if (extra && src) memcpy(h + 1, src, (size_t)extra * 4);
                r->used += need;
            }
        }
    }
    /* ---- ExecuteCommandLists(n, lists): walk recordings, log dispatches with their compute state ---- */
    if (isqueue && !sret && slot == 35 && readable(arg[1], (size_t)(uint32_t)arg[0] * 8)) {
        for (uint32_t li = 0; li < (uint32_t)arg[0] && li < 64; li++) replay_list(objid(((uint64_t*)arg[1])[li]));
    }
    if (isqueue && !sret && slot == 16) {                        /* Xbox queue Signal(fence, value): commands "finish" instantly */
        int f = objid(arg[0]);
        if (f > 0 && g_obj[f].iid.Data1 == 0x0A753DCF) FENCEVAL(f) = arg[1];
        return 0;
    }
    if (isheap && (slot == 9 || slot == 10)) {                   /* Get CPU/GPU DescriptorHandleForHeapStart */
        uint64_t h = (uint64_t)g_obj[id].aux;
        if (sret) { *(uint64_t*)rcx = h; ret = rcx; } else ret = h;
    } else if (isres && slot == 11 && !sret) {                   /* GetGPUVirtualAddress: unified memory, GPU VA = CPU address */
        ret = (uint64_t)g_obj[id].aux;
    } else if (isres && slot == 8 && !sret) {                    /* Map(subresource, range, ppData) */
        if (!g_obj[id].aux) {                                    /* resource created some way we don't track: give it 16 MB */
            g_obj[id].aux = VirtualAlloc(NULL, 16u << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            dlog("  Map on resource obj#%d without memory: gave fallback 16 MB at %p", id, g_obj[id].aux);
        }
        if (writable(arg[2], 8)) *(void**)arg[2] = g_obj[id].aux;
        ret = 0;
    } else if (sret) {                                           /* fill something sane for known struct returns */
        if (id == 0 && slot == 26) { uint32_t* h = (uint32_t*)rcx; uint32_t t = (uint32_t)arg[1]; h[0] = 4; h[1] = t == 1 ? 1 : (t == 2 ? 2 : 3); h[2] = 1; h[3] = 1; h[4] = 1; }
        else if (id == 0 && (slot == 25 || slot == 108)) {
            /* GetResourceAllocationInfo(mask, numDescs, pDescs) -> { SizeInBytes, Alignment }
               slot 108 = the "...Info1" variant: same plus an optional per-desc {Offset, Alignment, SizeInBytes} array */
            uint64_t total = 0; uint32_t num = (uint32_t)arg[1];
            for (uint32_t i = 0; i < num && i < 64; i++) {
                uint64_t sz = res_size(arg[2] + (uint64_t)i * 56);
                if (slot == 108 && writable(arg[3] + (uint64_t)i * 24, 24)) { uint64_t* o1 = (uint64_t*)(arg[3] + (uint64_t)i * 24); o1[0] = total; o1[1] = 0x10000; o1[2] = sz; }
                total += sz;
            }
            if (!total) total = 0x10000;
            ((uint64_t*)rcx)[0] = total; ((uint64_t*)rcx)[1] = 0x10000;
        }
        else *(uint64_t*)rcx = 0;
        ret = rcx;
    }
    LONG n = 1;
    LONG tot = (id >= 0) ? InterlockedIncrement(&g_obj[id].calls) : 1;
    /* log the first 4 calls of each (interface, method) pair across all objects, plus early creations */
    uint32_t kd = id >= 0 ? (uint32_t)g_obj[id].iid.Data1 : 0;
    int kslot = (int)((kd ^ (kd >> 16) ^ (kd >> 8)) & 63);
    g_kiid[kslot] = kd;
    LONG kn = InterlockedIncrement(&g_kcnt[kslot][slot]);
    { static volatile LONG started; if (!started && !InterlockedExchange(&started, 1)) CloseHandle(CreateThread(NULL, 0, rate_dumper, NULL, 0, NULL)); }
    (void)n; (void)tot;
    if ((kn <= 4 || (made[0] && kn <= 12)) && InterlockedIncrement(&g_total) <= 20000) {
        dlog("obj#%d(from obj#%d.slot%d iid=%08lX).slot%d%s (0x%llX%s, 0x%llX%s, 0x%llX%s, 0x%llX%s, 0x%llX, 0x%llX)%s caller=exe+0x%llX tid=%lu",
            id, id >= 0 ? g_obj[id].parent : -1, id >= 0 ? g_obj[id].slot : -1, id >= 0 ? g_obj[id].iid.Data1 : 0, slot, sret ? "[sret]" : "",
            arg[0], hx(arg[0], 32), arg[1], hx(arg[1], 32), arg[2], hx(arg[2], 16), arg[3], hx(arg[3], 16), arg[4], sret ? 0 : arg[5],
            made, (uint64_t)ra - g_exe, GetCurrentThreadId());
    }
    return ret;
}

#define DEF(a, b, c) static uint64_t s_##a##b##c(uint64_t o, uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4, uint64_t p5, uint64_t p6, uint64_t p7, uint64_t p8, uint64_t p9, uint64_t p10, uint64_t p11) { uint64_t v[11] = { p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11 }; return dprobe(0x##a##b##c, o, v, _ReturnAddress()); }
#define REF(a, b, c) (void*)s_##a##b##c,
#define HEX(M, a, b) M(a, b, 0) M(a, b, 1) M(a, b, 2) M(a, b, 3) M(a, b, 4) M(a, b, 5) M(a, b, 6) M(a, b, 7) M(a, b, 8) M(a, b, 9) M(a, b, A) M(a, b, B) M(a, b, C) M(a, b, D) M(a, b, E) M(a, b, F)
#define HEX2(M, a) HEX(M, a, 0) HEX(M, a, 1) HEX(M, a, 2) HEX(M, a, 3) HEX(M, a, 4) HEX(M, a, 5) HEX(M, a, 6) HEX(M, a, 7) HEX(M, a, 8) HEX(M, a, 9) HEX(M, a, A) HEX(M, a, B) HEX(M, a, C) HEX(M, a, D) HEX(M, a, E) HEX(M, a, F)
#define ALL(M) HEX2(M, 0) HEX2(M, 1) HEX2(M, 2) HEX2(M, 3)
ALL(DEF)
static void* g_dvt[NPROBE] = { ALL(REF) };

EXP uint64_t D3D12XboxCreateDevice(void* adapter, void* params, const GUID* riid, void** ppDevice, uint64_t e) {
    if (!g_exe) {
        g_exe = (uint64_t)GetModuleHandleA(NULL);
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(g_exe + ((IMAGE_DOS_HEADER*)g_exe)->e_lfanew);
        g_exeend = g_exe + nt->OptionalHeader.SizeOfImage;
    }
    void* dev = g_nobj ? g_obj[0].p : newobj(-1, -1, riid);
    dlog("D3D12XboxCreateDevice(adapter=%p, params=%p %s, riid=%p %s, ppDevice=%p) -> PROBE device obj#0 %p caller=exe+0x%llX",
        adapter, params, hx((uint64_t)params, 64), riid, hx((uint64_t)riid, 16), ppDevice, dev, (uint64_t)_ReturnAddress() - g_exe);
    if (ppDevice) *ppDevice = dev;
    return 0;
}
