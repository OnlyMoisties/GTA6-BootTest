/* Xbox DirectStorage (XCRDAPI.dll) stand-in.
   COM objects with a shared "probe" vtable: every slot logs its index + args, known slots are implemented.
   Interface layouts are inferred from what the game calls (no GXDK headers here). */
#define STUBDLL "XCRDAPI"
#include "stub.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "fsredir.h"

enum { K_FACTORY, K_FILE, K_QUEUE, K_STATUS, K_N };
static const char* kname[] = { "Factory", "File", "Queue", "StatusArray" };
#define NSLOT 24
#define SLOTLOG 40

typedef struct Obj {
    void** vtbl; int kind; volatile LONG ref;
    HANDLE h;                 /* File: file handle, Queue: error event */
    uint32_t cap; volatile LONG* done; HRESULT* hr;   /* StatusArray */
    wchar_t path[300];
} Obj;

static void* g_vtbl[NSLOT];
static volatile LONG g_cnt[K_N + 1][NSLOT];

static void wlog(const char* fmt, ...) {
    char path[MAX_PATH], buf[4096];
    if (!GetEnvironmentVariableA("GTA6_STUB_LOG", path, MAX_PATH)) return;
    int n = snprintf(buf, 64, "%llu [XCRDAPI] ", GetTickCount64());
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
/* describe a value: if it is a readable pointer, hex-dump it and show it as a string when it looks like one */
static void desc(char* out, size_t cap, uint64_t v, int bytes) {
    out[0] = 0;
    if (!readable(v, bytes)) return;
    const unsigned char* p = (const unsigned char*)v; size_t n = 0;
    n += snprintf(out + n, cap - n, " [");
    for (int i = 0; i < bytes && n + 4 < cap; i++) n += snprintf(out + n, cap - n, "%02X%s", p[i], (i % 8 == 7) ? " " : "");
    n += snprintf(out + n, cap - n, "]");
    const wchar_t* w = (const wchar_t*)v;
    if (bytes >= 16 && w[0] >= 0x20 && w[0] < 0x7F && w[1] >= 0x20 && w[1] < 0x7F && w[2] >= 0x20 && w[2] < 0x7F) {
        char s[200]; int i = 0, lim = readable(v, 400) ? 199 : bytes / 2 - 1;
        for (; i < lim && w[i] >= 0x20 && w[i] < 0x7F; i++) s[i] = (char)w[i];
        s[i] = 0; snprintf(out + n, cap - n, " L\"%s\"", s);
    } else if (bytes >= 8 && p[0] >= 0x20 && p[0] < 0x7F && p[1] >= 0x20 && p[1] < 0x7F && p[2] >= 0x20 && p[2] < 0x7F && p[3] >= 0x20 && p[3] < 0x7F) {
        char s[100]; int i = 0;
        for (; i < bytes - 1 && i < 99 && p[i] >= 0x20 && p[i] < 0x7F; i++) s[i] = (char)p[i];
        s[i] = 0; snprintf(out + n, cap - n, " \"%s\"", s);
    }
}
static Obj* newobj(int kind) {
    Obj* o = (Obj*)calloc(1, sizeof(Obj)); o->vtbl = g_vtbl; o->kind = kind; o->ref = 1; return o;
}
static Obj* asobj(uint64_t p) { return (readable(p, sizeof(Obj)) && ((Obj*)p)->vtbl == g_vtbl) ? (Obj*)p : NULL; }

#define S_OK_ 0ull
#include "inflate.h"
/* Xbox DSTORAGE_REQUEST layout seen at runtime:
   +00 options (low byte 1 = zlib), +08 dest buffer, +18 dest size (u32), +20 IDStorageFile*, +28 file offset, +30 source size (u32) */
static volatile LONG g_nreq, g_nfail;
static HRESULT g_lasterr;
static void do_request(uint64_t rq) {
    if (!readable(rq, 0x38)) { g_lasterr = E_FAIL; return; }
    uint64_t opts = *(uint64_t*)(rq + 0x00), dst = *(uint64_t*)(rq + 0x08), fptr = *(uint64_t*)(rq + 0x20), off = *(uint64_t*)(rq + 0x28);
    uint32_t dsz = *(uint32_t*)(rq + 0x18), ssz = *(uint32_t*)(rq + 0x30);
    Obj* f = asobj(fptr); HRESULT hr = S_OK; long long got = -1; const char* why = "ok";
    LONG n = InterlockedIncrement(&g_nreq);
    if (!f || f->kind != K_FILE || !f->h) { hr = E_INVALIDARG; why = "bad file"; }
    else {
        unsigned char* tmp = (unsigned char*)malloc(ssz ? ssz : 1); DWORD rd = 0;
        OVERLAPPED ov = { 0 }; ov.Offset = (DWORD)off; ov.OffsetHigh = (DWORD)(off >> 32);
        if (!tmp || !ReadFile(f->h, tmp, ssz, &rd, &ov) || rd != ssz) { hr = HRESULT_FROM_WIN32(GetLastError()); why = "read failed"; }
        else if ((opts & 0xFF) == 0) { if (ssz <= dsz) { memcpy((void*)dst, tmp, ssz); got = ssz; } else { hr = E_INVALIDARG; why = "dest too small"; } }
        else if ((opts & 0xFF) == 1) { got = inflate_zlib((unsigned char*)dst, dsz, tmp, ssz); if (got < 0) { hr = E_FAIL; why = "inflate failed"; } }
        else { hr = E_NOTIMPL; why = "unknown compression"; }
        free(tmp);
    }
    if (hr) { g_lasterr = hr; InterlockedIncrement(&g_nfail); }
    if (n <= 30 || hr || (n & (n - 1)) == 0)
        wlog("request#%ld opts=0x%llX file=%ls off=0x%llX src=0x%X -> dst=0x%llX size=0x%X : %s (wrote %lld) hr=0x%08lX fails-so-far=%ld",
            n, opts, f ? f->path : L"?", off, ssz, dst, dsz, why, got, hr, g_nfail);
}
static uint64_t dispatch(Obj* o, int n, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, void* ra) {
    int k = asobj((uint64_t)o) ? o->kind : K_N;
    uint64_t ret = E_NOTIMPL_; const char* name = "?UNKNOWN-SLOT"; int quiet = 0;
    if (k == K_N) { wlog("call on non-object %p slot %d", o, n); return E_FAIL_; }
    /* IUnknown - same for every kind */
    if (n == 0) { name = "QueryInterface"; if (readable(b, 8)) *(void**)b = o; InterlockedIncrement(&o->ref); ret = S_OK_; }
    else if (n == 1) { name = "AddRef"; ret = InterlockedIncrement(&o->ref); quiet = 1; }
    else if (n == 2) { name = "Release"; ret = InterlockedDecrement(&o->ref); quiet = 1; }
    else switch (k * 100 + n) {
    case K_FACTORY * 100 + 3: { name = "CreateQueue(desc,riid,ppv)";
        Obj* q = newobj(K_QUEUE); q->h = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (readable(c, 8)) { *(void**)c = q; ret = S_OK_; } else ret = E_FAIL_; break; }
    case K_FACTORY * 100 + 4: { name = "OpenFile(path,riid,ppv)";
        if (!readable(a, 8) || !readable(c, 8)) { ret = E_FAIL_; break; }
        wchar_t rb[1024]; const wchar_t* rp = fs_redir((const wchar_t*)a, rb);
        HANDLE h = CreateFileW(rp, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) { *(void**)c = NULL; ret = 0x80070000ull | (GetLastError() & 0xFFFF); break; }
        Obj* f = newobj(K_FILE); f->h = h; wcsncpy(f->path, (const wchar_t*)a, 299); *(void**)c = f; ret = S_OK_; break; }
    case K_FACTORY * 100 + 5: { name = "CreateStatusArray(capacity,name,riid,ppv)";
        if (!readable(d, 8)) { ret = E_FAIL_; break; }
        Obj* s = newobj(K_STATUS); s->cap = (uint32_t)a ? (uint32_t)a : 1; s->done = (volatile LONG*)calloc(s->cap, sizeof(LONG)); s->hr = (HRESULT*)calloc(s->cap, sizeof(HRESULT));
        *(void**)d = s; ret = S_OK_; break; }
    case K_FACTORY * 100 + 10: { name = "CreateQueue(desc,riid,ppQueue,?)";
        Obj* q = newobj(K_QUEUE); q->h = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (readable(c, 8)) { *(void**)c = q; ret = S_OK_; } else ret = E_FAIL_; break; }
    case K_FACTORY * 100 + 6: name = "SetDebugFlags(flags)"; ret = S_OK_; break;
    case K_FACTORY * 100 + 7: name = "?slot7 (returning S_OK)"; ret = S_OK_; break;
    case K_FACTORY * 100 + 8: name = "SetStagingBufferSize(size)"; ret = S_OK_; break;
    case K_FILE * 100 + 3: name = "File.Close"; if (o->h) { CloseHandle(o->h); o->h = NULL; } ret = S_OK_; break;
    case K_FILE * 100 + 4: name = "File.GetFileInformation(info)"; ret = (readable(a, sizeof(BY_HANDLE_FILE_INFORMATION)) && GetFileInformationByHandle(o->h, (BY_HANDLE_FILE_INFORMATION*)a)) ? S_OK_ : E_FAIL_; break;
    case K_QUEUE * 100 + 3: name = "Queue.EnqueueRequest(req)"; ret = S_OK_; quiet = 1; do_request(a); break;
    case K_QUEUE * 100 + 4: { name = "Queue.EnqueueStatus(array,index)";
        Obj* s = asobj(a);                         /* requests run synchronously, so everything queued so far is done */
        if (s && s->kind == K_STATUS && b < s->cap) { s->hr[b] = g_lasterr; s->done[b] = 1; g_lasterr = 0; }
        ret = S_OK_; break; }
    case K_QUEUE * 100 + 5: name = "Queue.EnqueueSignal(fence,value)"; ret = S_OK_; break;
    case K_QUEUE * 100 + 6: name = "Queue.Submit"; ret = S_OK_; break;
    case K_QUEUE * 100 + 7: name = "Queue.CancelRequestsWithTag(mask,value)"; ret = S_OK_; break;
    case K_QUEUE * 100 + 8: name = "Queue.Close"; ret = S_OK_; break;
    case K_QUEUE * 100 + 9: name = "Queue.GetErrorEvent"; ret = (uint64_t)o->h; break;
    case K_QUEUE * 100 + 10: name = "Queue.RetrieveErrorRecord(rec)"; ret = S_OK_; break;
    case K_QUEUE * 100 + 11: name = "Queue.Query(info)"; ret = S_OK_; break;
    case K_STATUS * 100 + 3: name = "Status.IsComplete(index)"; ret = (a < o->cap) ? (uint64_t)(o->done[a] != 0) : 1; break;
    case K_STATUS * 100 + 4: name = "Status.GetHResult(index)"; ret = (a < o->cap) ? (uint64_t)(uint32_t)o->hr[a] : E_FAIL_; break;
    }
    LONG cnt = InterlockedIncrement(&g_cnt[k][n]);
    if (cnt <= (quiet ? 3 : SLOTLOG)) {
        uint64_t base = (uint64_t)GetModuleHandleA(NULL), r = (uint64_t)ra;
        char da[400], db[200], dc[200], dd[200];
        desc(da, sizeof da, a, 128); desc(db, sizeof db, b, 16); desc(dc, sizeof dc, c, 16); desc(dd, sizeof dd, d, 16);
        wlog("%s@%p slot%d %s (0x%llX%s, 0x%llX%s, 0x%llX%s, 0x%llX%s, 0x%llX) -> 0x%llX caller=exe+0x%llX tid=%lu",
            kname[k], o, n, name, a, da, b, db, c, dc, d, dd, e, ret, r - base, GetCurrentThreadId());
    }
    return ret;
}

#define SLOT(n) static uint64_t slot##n(Obj* o, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) { return dispatch(o, n, a, b, c, d, e, _ReturnAddress()); }
SLOT(0) SLOT(1) SLOT(2) SLOT(3) SLOT(4) SLOT(5) SLOT(6) SLOT(7) SLOT(8) SLOT(9) SLOT(10) SLOT(11)
SLOT(12) SLOT(13) SLOT(14) SLOT(15) SLOT(16) SLOT(17) SLOT(18) SLOT(19) SLOT(20) SLOT(21) SLOT(22) SLOT(23)
static void* g_vtbl[NSLOT] = { slot0, slot1, slot2, slot3, slot4, slot5, slot6, slot7, slot8, slot9, slot10, slot11,
    slot12, slot13, slot14, slot15, slot16, slot17, slot18, slot19, slot20, slot21, slot22, slot23 };

static Obj* g_factory;
EXP uint64_t DStorageGetFactory(const GUID* riid, void** ppv) {
    if (!g_factory) g_factory = newobj(K_FACTORY);
    if (ppv) *ppv = g_factory;
    InterlockedIncrement(&g_factory->ref);
    wlog("DStorageGetFactory -> %p", g_factory);
    return 0;
}
