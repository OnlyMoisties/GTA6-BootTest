/* Intro-video viewer: after the game decodes a Bink frame, read Bink's Y/cR/cB planes, convert to BGRA on the CPU
   and draw them into the game window. Works only if Bink decodes on the CPU into normal memory.
   Layout follows the public Bink SDK structs (BINKFRAMEBUFFERS / BINKPLANE), verified at runtime by logging. */
typedef struct { int32_t Allocate; int32_t pad; void* Buffer; uint32_t BufferPitch; uint32_t pad2; } BKPlane;          /* 24 bytes */
typedef struct { BKPlane Y, cR, cB, A, H; } BKPlaneSet;                                                                 /* 120 bytes */
typedef struct { int32_t TotalFrames; uint32_t YABufferWidth, YABufferHeight, cRcBBufferWidth, cRcBBufferHeight, FrameNum; BKPlaneSet Frames[4]; } BKFrameBuffers;
typedef void (*BinkGetFBInfoFn)(void*, void*);
static BinkGetFBInfoFn g_bkGetFB;
static volatile ULONGLONG g_videoTick;                 /* last time a video frame was drawn (d3d12 show_frame stays out of the way) */
__declspec(dllexport) ULONGLONG GTA6_VideoTick(void) { return g_videoTick; }
static uint8_t* g_vidrgb; static size_t g_vidcap;
static volatile LONG g_bklog;

static void bink_show(void* bink) {
    return;   /* superseded by the CPU viewer (the game's own buffers are GPU bitstream) */
    if (!g_bkGetFB) g_bkGetFB = (BinkGetFBInfoFn)GetProcAddress(GetModuleHandleA("Bink2Scarlett.dll"), "BinkGetFrameBuffersInfo");
    if (!g_bkGetFB || !bink) return;
    static __declspec(align(16)) uint8_t raw[2048]; memset(raw, 0, sizeof raw);
    g_bkGetFB(bink, raw);
    BKFrameBuffers* fb = (BKFrameBuffers*)raw;
    LONG n = InterlockedIncrement(&g_bklog);
    if (n <= 3) {
        char hexs[600]; size_t k = 0; for (int i = 0; i < 96 && k + 4 < sizeof hexs; i++) k += snprintf(hexs + k, sizeof hexs - k, "%02X%s", raw[i], (i % 8 == 7) ? " " : "");
        char path[MAX_PATH], buf[900];
        if (GetEnvironmentVariableA("GTA6_STUB_LOG", path, MAX_PATH)) {
            int m = snprintf(buf, sizeof buf, "%llu [bink] frame-buffers: total=%d Y=%ux%u C=%ux%u cur=%u Y.buf=%p pitch=%u cR.buf=%p cB.buf=%p raw=[%s]\r\n",
                GetTickCount64(), fb->TotalFrames, fb->YABufferWidth, fb->YABufferHeight, fb->cRcBBufferWidth, fb->cRcBBufferHeight, fb->FrameNum,
                fb->Frames[0].Y.Buffer, fb->Frames[0].Y.BufferPitch, fb->Frames[0].cR.Buffer, fb->Frames[0].cB.Buffer, hexs);
            HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, buf, m, &w, NULL); CloseHandle(h); }
        }
    }
    if (n <= 2) {   /* what do the three 16-byte entries at +0x18 point to? (probe objects carry a magic at base+0xFF0) */
        char path[MAX_PATH]; if (GetEnvironmentVariableA("GTA6_STUB_LOG", path, MAX_PATH)) {
            for (int i = 0; i < 3; i++) {
                uint64_t p = *(uint64_t*)(raw + 0x18 + i * 16), base = p & ~0xFFFFull; char buf[700]; int m = 0;
                MEMORY_BASIC_INFORMATION mi; int ok = p && VirtualQuery((void*)base, &mi, sizeof mi) && mi.State == MEM_COMMIT;
                int probe = ok && mi.RegionSize >= 0x1000 && *(uint64_t*)(base + 0xFF0) == 0x45424F5250424F50ull;
                m = snprintf(buf, sizeof buf, "%llu [bink] entry%d ptr=%llX probeObject=%d", GetTickCount64(), i, p, probe);
                if (probe) m += snprintf(buf + m, sizeof buf - m, " id=%llu", *(uint64_t*)(base + 0xFF8));
                if (ok && VirtualQuery((void*)p, &mi, sizeof mi) && mi.State == MEM_COMMIT) { m += snprintf(buf + m, sizeof buf - m, " data:"); for (int j = 0; j < 48; j++) m += snprintf(buf + m, sizeof buf - m, "%s%02X", j % 8 ? "" : " ", ((uint8_t*)p)[j]); }
                m += snprintf(buf + m, sizeof buf - m, "\r\n");
                HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
                if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, buf, m, &w, NULL); CloseHandle(h); }
            }
        }
    }
    uint32_t W = fb->YABufferWidth, H = fb->YABufferHeight, cur = fb->FrameNum;
    if (!W || !H || W > 8192 || H > 8192 || cur >= 4 || fb->TotalFrames < 1 || fb->TotalFrames > 4) return;
    BKPlaneSet* ps = &fb->Frames[cur];
    uint8_t *Y = (uint8_t*)ps->Y.Buffer, *Cr = (uint8_t*)ps->cR.Buffer, *Cb = (uint8_t*)ps->cB.Buffer;
    uint32_t yp = ps->Y.BufferPitch, rp = ps->cR.BufferPitch, bp = ps->cB.BufferPitch;
    MEMORY_BASIC_INFORMATION mbi;
    if (!Y || !Cr || !Cb || !yp || !VirtualQuery(Y, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return;
    size_t need = (size_t)W * H * 4;
    if (g_vidcap < need) { if (g_vidrgb) VirtualFree(g_vidrgb, 0, MEM_RELEASE); g_vidrgb = (uint8_t*)VirtualAlloc(NULL, need, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); g_vidcap = g_vidrgb ? need : 0; }
    if (!g_vidrgb) return;
    for (uint32_t y = 0; y < H; y++) {                  /* BT.709-ish limited-range YCbCr -> BGRA, chroma at half resolution */
        uint8_t* o = g_vidrgb + (size_t)y * W * 4; const uint8_t* yr = Y + (size_t)y * yp;
        const uint8_t* crr = Cr + (size_t)(y >> 1) * rp; const uint8_t* cbr = Cb + (size_t)(y >> 1) * bp;
        for (uint32_t x = 0; x < W; x++) {
            int c = (yr[x] - 16) * 298, d = cbr[x >> 1] - 128, e = crr[x >> 1] - 128;
            int r = (c + 459 * e + 128) >> 8, g = (c - 55 * d - 136 * e + 128) >> 8, b = (c + 541 * d + 128) >> 8;
            o[x * 4 + 0] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b); o[x * 4 + 1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
            o[x * 4 + 2] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r); o[x * 4 + 3] = 255;
        }
    }
    HWND hwnd = FindWindowA(NULL, "Americas"); if (!hwnd) return;
    RECT rc; GetClientRect(hwnd, &rc); HDC dc = GetDC(hwnd); if (!dc) return;
    BITMAPINFO bi = { 0 }; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = (LONG)W; bi.bmiHeader.biHeight = -(LONG)H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    /* letterbox to keep the video's aspect ratio */
    int cw = rc.right, ch = rc.bottom, dw = cw, dh = (int)((int64_t)cw * H / W);
    if (dh > ch) { dh = ch; dw = (int)((int64_t)ch * W / H); }
    SetStretchBltMode(dc, HALFTONE);
    StretchDIBits(dc, (cw - dw) / 2, (ch - dh) / 2, dw, dh, 0, 0, W, H, g_vidrgb, &bi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(hwnd, dc);
    g_videoTick = GetTickCount64();
}
/* ---- CPU viewer: a private second decode of the same in-memory .bk2, shown in the game window.
   The game's own handle is left exactly as it opened it. ---- */
typedef void* (*BkOpenFn)(const char*, void*, uint32_t);
typedef int32_t (*BkDoFn)(void*);
typedef void (*BkNextFn)(void*);
typedef int32_t (*BkCopyFn)(void*, void*, int32_t, uint32_t, uint32_t, uint32_t, uint32_t);
typedef void (*BkCloseFn)(void*);
typedef void (*BkSoundFn)(void*, int32_t);
static volatile LONG g_viewerBusy;
static void vlog(const char* fmt, ...) {
    char path[MAX_PATH], buf[600]; if (!GetEnvironmentVariableA("GTA6_STUB_LOG", path, MAX_PATH)) return;
    int n = snprintf(buf, 64, "%llu [bink-view] ", GetTickCount64());
    va_list ap; va_start(ap, fmt); n += vsnprintf(buf + n, sizeof buf - n - 3, fmt, ap); va_end(ap);
    if (n > (int)sizeof buf - 3) n = sizeof buf - 3; buf[n++] = '\r'; buf[n++] = '\n';
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, buf, n, &w, NULL); CloseHandle(h); }
}
static void blit_bgra(const uint8_t* px, uint32_t W, uint32_t H) {
    HWND hwnd = FindWindowA(NULL, "Americas"); if (!hwnd) return;
    RECT rc; GetClientRect(hwnd, &rc); HDC dc = GetDC(hwnd); if (!dc) return;
    BITMAPINFO bi = { 0 }; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = (LONG)W; bi.bmiHeader.biHeight = -(LONG)H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    int cw = rc.right, ch = rc.bottom, dw = cw, dh = (int)((int64_t)cw * H / W);
    if (dh > ch) { dh = ch; dw = (int)((int64_t)ch * W / H); }
    SetStretchBltMode(dc, HALFTONE); SetBrushOrgEx(dc, 0, 0, NULL);
    StretchDIBits(dc, (cw - dw) / 2, (ch - dh) / 2, dw, dh, 0, 0, W, H, px, &bi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(hwnd, dc);
    g_videoTick = GetTickCount64();
}
static void* volatile g_gameBink;        /* the game's own HBINK for the video being shown (frame clock) */
static volatile ULONGLONG g_gameOpenTick; /* when the game opened it: Bink starts the audio then */
static volatile uint32_t g_gameFlags;
static volatile LONG g_gameBinkClosed;
typedef void (*BkGotoFn)(void*, uint32_t, int32_t);
static DWORD WINAPI cpu_viewer(void* data) {
    HMODULE bk = GetModuleHandleA("Bink2Scarlett.dll");
    BkGotoFn gto = (BkGotoFn)GetProcAddress(bk, "BinkGoto");
    BkOpenFn op = (BkOpenFn)GetProcAddress(bk, "BinkOpenWithOptions"); BkDoFn dof = (BkDoFn)GetProcAddress(bk, "BinkDoFrame");
    BkNextFn nx = (BkNextFn)GetProcAddress(bk, "BinkNextFrame"); BkCopyFn cp = (BkCopyFn)GetProcAddress(bk, "BinkCopyToBuffer");
    BkCloseFn cl = (BkCloseFn)GetProcAddress(bk, "BinkClose"); BkSoundFn snd = (BkSoundFn)GetProcAddress(bk, "BinkSetSoundOnOff");
    if (!op || !dof || !nx || !cp || !cl) { vlog("missing Bink exports (open=%p do=%p next=%p copy=%p close=%p)", op, dof, nx, cp, cl); goto out; }
    /* from memory with CPU frame buffers; if the plain open fails, retry with the game's flags minus likely GPU bits */
    uint32_t gf = g_gameFlags, tries[5] = { 0x04000000, gf & ~0x00100000u, gf & ~0x00100400u, gf & ~0x00104400u, gf & ~0x00104000u };
    void* b = NULL; uint32_t used = 0;
    for (int t = 0; t < 5 && !b; t++) { if (t && tries[t] == tries[t - 1]) continue; b = op((const char*)data, NULL, tries[t]); used = tries[t]; }
    if (!b) { vlog("private BinkOpen failed with all flag sets (game flags 0x%08X)", gf); goto out; }
    vlog("private open flags 0x%08X (game used 0x%08X)", used, gf);
    uint32_t W = ((uint32_t*)b)[0], H = ((uint32_t*)b)[1], frames = ((uint32_t*)b)[2];
    uint32_t fr = ((uint32_t*)b)[5], frd = ((uint32_t*)b)[6]; if (!fr || fr > 240) fr = 30; if (!frd || frd > 1000) frd = 1;
    if (snd) snd(b, 0);                                                    /* the game already plays the audio */
    vlog("private decode open: %ux%u, %u frames, rate %u/%u", W, H, frames, fr, frd);
    uint8_t* px = (uint8_t*)VirtualAlloc(NULL, (size_t)W * H * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ULONGLONG t0 = GetTickCount64();
    /* Follow the game's own frame clock: the game's HBINK FrameNum (+0x0C, 1-based) is the frame its audio is on.
       Decode/seek our private copy to that frame and show it; stop when the game closes the video. */
    uint32_t shown = 0, ours = 1, lastTarget = 0; ULONGLONG lastMove = GetTickCount64();
    ULONGLONG closedAt = 0;
    while (px) {
        void* gb = g_gameBink;
        if (g_gameBinkClosed || !gb) {                                     /* closed: wait briefly for a reopen */
            if (!closedAt) closedAt = GetTickCount64();
            if (GetTickCount64() - closedAt > 1000) break;
            Sleep(5); continue;
        }
        closedAt = 0;
        /* frame clock: the game's FrameNum when it advances; otherwise elapsed time since the game opened the video
           (that is when Bink starts the audio), at the video's own frame rate */
        uint32_t gameFrame = ((volatile uint32_t*)gb)[3];
        uint32_t timeFrame = 1 + (uint32_t)((GetTickCount64() - g_gameOpenTick) * fr / (1000ull * frd));
        uint32_t target = (gameFrame > 1 && gameFrame <= frames) ? gameFrame : timeFrame;
        if (target > frames) break;                                         /* past the end */
        (void)lastMove;
        if (target == shown) { Sleep(2); continue; }
        if (target < ours || target > ours + 4) { if (gto) { gto(b, target, 0); ours = target; } }
        while (ours < target) { dof(b); nx(b); ours++; }                    /* catch up a few frames */
        ULONGLONG f0 = GetTickCount64();
        dof(b);
        uint32_t i = target - 1;
        int32_t cr = cp(b, px, (int32_t)(W * 4), H, 0, 0, 3);              /* 3 = 32-bit BGRx surface */
        if (shown < 3 || (i & 63) == 0) vlog("frame %u (game clock) decoded+copied in %llu ms (copy rc=%d)", i, GetTickCount64() - f0, cr);
        blit_bgra(px, W, H);
        shown = target;
        if (ours < frames) { nx(b); ours++; }
        {   /* GTA6_DUMP_FRAME=<n>: save decoded frame n as frame.bmp next to the log, for checking */
            char e[32]; if (GetEnvironmentVariableA("GTA6_DUMP_FRAME", e, sizeof e) && (uint32_t)atoi(e) == i) {
                char path[MAX_PATH]; if (GetEnvironmentVariableA("GTA6_STUB_LOG", path, MAX_PATH)) {
                    char* s = strrchr(path, '\\'); if (s) strcpy(s + 1, "frame.bmp");
                    BITMAPFILEHEADER fh = { 0x4D42 }; BITMAPINFOHEADER ih = { sizeof ih }; ih.biWidth = (LONG)W; ih.biHeight = -(LONG)H; ih.biPlanes = 1; ih.biBitCount = 32;
                    fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + W * H * 4;
                    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
                    if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, &fh, sizeof fh, &w, NULL); WriteFile(h, &ih, sizeof ih, &w, NULL); WriteFile(h, px, W * H * 4, &w, NULL); CloseHandle(h); vlog("saved frame %u to %s", i, path); }
                }
            }
        }
    }
    (void)fr; (void)frd;
    vlog("private decode finished after %llu ms", GetTickCount64() - t0);
    cl(b); if (px) VirtualFree(px, 0, MEM_RELEASE);
out:
    VirtualFree(data, 0, MEM_RELEASE);
    InterlockedExchange(&g_viewerBusy, 0);
    return 0;
}
static void start_cpu_viewer(const char* bk2) {
    MEMORY_BASIC_INFORMATION mi;
    if (!bk2 || !VirtualQuery(bk2, &mi, sizeof mi) || mi.State != MEM_COMMIT || memcmp(bk2, "KB2", 3)) return;
    uint32_t size = *(const uint32_t*)(bk2 + 4) + 8;                       /* Bink header: total size - 8 at +4 */
    if (size < 64 || size > (2u << 30)) return;
    if (InterlockedExchange(&g_viewerBusy, 1)) return;                     /* one video at a time */
    void* copy = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!copy) { g_viewerBusy = 0; return; }
    memcpy(copy, bk2, size);
    vlog("copied %u bytes of in-memory .bk2 for the CPU viewer", size);
    CloseHandle(CreateThread(NULL, 0, cpu_viewer, copy, 0, NULL));
}
/* BinkOpenWithOptions(name, options, flags): log what the game asks for (flags decide GPU vs CPU decoding) */
typedef void* (*BinkOpenWOFn)(const char*, void*, uint32_t);
static BinkOpenWOFn g_bkOpen;
static void* h_BinkOpenWithOptions(const char* name, void* opts, uint32_t flags) {
    uint32_t want = flags;
    char cl[64]; if (GetEnvironmentVariableA("GTA6_BINK_CLEAR", cl, sizeof cl)) want &= ~(uint32_t)_strtoui64(cl, NULL, 16);
    void* r = g_bkOpen(name, opts, want);
    if (r && (flags & 0x04000000) && GetEnvironmentVariableA("GTA6_VIDEO_VIEW", NULL, 0)) {   /* off by default (stand-in only) */
        g_gameBink = r; g_gameBinkClosed = 0;          /* the viewer always follows the newest game handle */
        g_gameOpenTick = GetTickCount64(); g_gameFlags = flags;
        if (!g_viewerBusy) start_cpu_viewer(name);
    }
    char path[MAX_PATH], buf[1200]; int m = 0;
    if (GetEnvironmentVariableA("GTA6_STUB_LOG", path, MAX_PATH)) {
        char nm[200] = "?"; MEMORY_BASIC_INFORMATION mi;
        if (name && VirtualQuery(name, &mi, sizeof mi) && mi.State == MEM_COMMIT) { int i = 0; for (; i < 199 && name[i] >= 0x20 && name[i] < 0x7F; i++) nm[i] = name[i]; nm[i] = 0; }
        m = snprintf(buf, sizeof buf, "%llu [bink] BinkOpenWithOptions(name=%p \"%s\", opts=%p, flags=0x%08X -> passed 0x%08X) -> %p opts:", GetTickCount64(), name, nm, opts, flags, want, r);
        if (opts && VirtualQuery(opts, &mi, sizeof mi) && mi.State == MEM_COMMIT) for (int j = 0; j < 96; j++) m += snprintf(buf + m, sizeof buf - m, "%s%02X", j % 8 ? "" : " ", ((uint8_t*)opts)[j]);
        if (r) { m += snprintf(buf + m, sizeof buf - m, " hbink:"); for (int j = 0; j < 32; j++) m += snprintf(buf + m, sizeof buf - m, "%s%02X", j % 8 ? "" : " ", ((uint8_t*)r)[j]); }
        m += snprintf(buf + m, sizeof buf - m, "\r\n");
        HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, buf, m, &w, NULL); CloseHandle(h); }
    }
    return r;
}
typedef void (*BinkCloseFn2)(void*);
static BinkCloseFn2 g_bkClose;
static void h_BinkClose(void* b) {
    if (b && b == g_gameBink) { g_gameBinkClosed = 1; g_gameBink = NULL; Sleep(20); }   /* viewer stops reading it first */
    g_bkClose(b);
}
typedef int32_t (*BinkDoFrameFn)(void*);
typedef int32_t (*BinkWaitFn)(void*, int32_t);
static BinkDoFrameFn g_bkDo; static BinkWaitFn g_bkWait;
static int32_t h_BinkDoFrame(void* b) { int32_t r = g_bkDo(b); bink_show(b); return r; }
static int32_t h_BinkDoFrameAsyncWait(void* b, int32_t us) { int32_t r = g_bkWait(b, us); if (r) bink_show(b); return r; }
