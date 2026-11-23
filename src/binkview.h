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
typedef int32_t (*BinkDoFrameFn)(void*);
typedef int32_t (*BinkWaitFn)(void*, int32_t);
static BinkDoFrameFn g_bkDo; static BinkWaitFn g_bkWait;
static int32_t h_BinkDoFrame(void* b) { int32_t r = g_bkDo(b); bink_show(b); return r; }
static int32_t h_BinkDoFrameAsyncWait(void* b, int32_t us) { int32_t r = g_bkWait(b, us); if (r) bink_show(b); return r; }
