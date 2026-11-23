/* Path redirection for the Xbox exe: its "G:" package root -> the real game folder.
   Installed by patching the exe's import table from xmem.dll's DllMain (xmem is always loaded first-hand by the exe). */
#include <wchar.h>
static wchar_t g_root[MAX_PATH];
static volatile LONG g_fslog;

static void fs_init(void) {
    if (g_root[0]) return;
    if (!GetEnvironmentVariableW(L"GTA6_ROOT", g_root, MAX_PATH)) {
        GetModuleFileNameW(NULL, g_root, MAX_PATH);
        wchar_t* s = wcsrchr(g_root, L'\\'); if (s) *s = 0;
    }
}
static void fs_log(const char* fn, const wchar_t* in, const wchar_t* out, uint64_t result) {
    if (strncmp(fn, "CreateWindow", 12) && InterlockedIncrement(&g_fslog) > 600) return;
    char path[MAX_PATH], buf[1400];
    if (!GetEnvironmentVariableA("GTA6_STUB_LOG", path, MAX_PATH)) return;
    int n = snprintf(buf, sizeof buf, "%llu [fs] %s(\"%.400ls\")%s%.400ls -> 0x%llX tid=%lu\r\n", GetTickCount64(), fn, in ? in : L"(null)", out != in ? " => " : "", out != in ? out : L"", result, GetCurrentThreadId());
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, buf, n, &w, NULL); CloseHandle(h); }
}
/* returns `in` unchanged, or `out` (>= 1024 wchar) holding the redirected path */
static const wchar_t* fs_redir(const wchar_t* in, wchar_t* out) {
    if (!in) return in;
    fs_init();
    const wchar_t* p = in;
    if (!wcsncmp(p, L"\\\\?\\", 4)) p += 4;
    if ((p[0] == L'G' || p[0] == L'g') && p[1] == L':') {
        const wchar_t* rest = p + 2; while (*rest == L'/' || *rest == L'\\') rest++;
        _snwprintf(out, 1023, L"%s\\%s", g_root, rest); out[1023] = 0;
        for (wchar_t* q = out; *q; q++) if (*q == L'/') *q = L'\\';
        return out;
    }
    return in;
}

#define RD wchar_t b_[1024]; const wchar_t* q_ = fs_redir(a, b_)
static HANDLE WINAPI h_CreateFileW(LPCWSTR a, DWORD acc, DWORD sh, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD fl, HANDLE t) { RD; HANDLE r = CreateFileW(q_, acc, sh, sa, disp, fl, t); fs_log("CreateFileW", a, q_, (uint64_t)r); return r; }
static BOOL WINAPI h_CreateDirectoryW(LPCWSTR a, LPSECURITY_ATTRIBUTES sa) { RD; BOOL r = CreateDirectoryW(q_, sa); fs_log("CreateDirectoryW", a, q_, r); return r; }
static BOOL WINAPI h_DeleteFileW(LPCWSTR a) { RD; fs_log("DeleteFileW[BLOCKED-if-redirected]", a, q_, 0); if (q_ != a) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; } return DeleteFileW(q_); }
static HANDLE WINAPI h_FindFirstFileW(LPCWSTR a, LPWIN32_FIND_DATAW fd) { RD; HANDLE r = FindFirstFileW(q_, fd); fs_log("FindFirstFileW", a, q_, (uint64_t)r); return r; }
static BOOL WINAPI h_GetDiskFreeSpaceExW(LPCWSTR a, PULARGE_INTEGER x, PULARGE_INTEGER y, PULARGE_INTEGER z) { RD; BOOL r = GetDiskFreeSpaceExW(q_, x, y, z); fs_log("GetDiskFreeSpaceExW", a, q_, r); return r; }
static BOOL WINAPI h_GetFileAttributesExW(LPCWSTR a, GET_FILEEX_INFO_LEVELS l, LPVOID d) { RD; BOOL r = GetFileAttributesExW(q_, l, d); fs_log("GetFileAttributesExW", a, q_, r); return r; }
static DWORD WINAPI h_GetFileAttributesW(LPCWSTR a) { RD; DWORD r = GetFileAttributesW(q_); fs_log("GetFileAttributesW", a, q_, r); return r; }
static DWORD WINAPI h_GetFileAttributesA(LPCSTR s) { wchar_t a[1024]; MultiByteToWideChar(CP_ACP, 0, s ? s : "", -1, a, 1024); RD; DWORD r = GetFileAttributesW(q_); fs_log("GetFileAttributesA", a, q_ == a ? a : q_, r); return r; }
static BOOL WINAPI h_RemoveDirectoryW(LPCWSTR a) { RD; fs_log("RemoveDirectoryW[BLOCKED-if-redirected]", a, q_, 0); if (q_ != a) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; } return RemoveDirectoryW(q_); }
static BOOL WINAPI h_SetFileAttributesW(LPCWSTR a, DWORD at) { RD; BOOL r = SetFileAttributesW(q_, at); fs_log("SetFileAttributesW", a, q_, r); return r; }
static BOOL WINAPI h_MoveFileExW(LPCWSTR a, LPCWSTR to, DWORD fl) { RD; fs_log("MoveFileExW[BLOCKED-if-redirected]", a, q_, 0); if (q_ != a) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; } return MoveFileExW(a, to, fl); }

static volatile LONG g_gpacnt;
#include "xgrproxy.h"
static FARPROC WINAPI h_GetProcAddress(HMODULE m, LPCSTR name) {
    FARPROC r = GetProcAddress(m, name);
    if (r && ((uint64_t)name >> 16) && !strcmp(name, "QueryApiImpl")) { g_realQuery = (QueryApiImplFn)r; r = (FARPROC)h_QueryApiImpl; }
    if (InterlockedIncrement(&g_gpacnt) <= 400) {
        wchar_t mod[MAX_PATH] = L"?", nm[200]; GetModuleFileNameW(m, mod, MAX_PATH);
        if ((uint64_t)name >> 16) MultiByteToWideChar(CP_ACP, 0, name, -1, nm, 200); else _snwprintf(nm, 200, L"#%u", (unsigned)(uint64_t)name);
        wchar_t* base = wcsrchr(mod, L'\\'); wchar_t line[500]; _snwprintf(line, 500, L"%s!%s", base ? base + 1 : mod, nm); line[499] = 0;
        fs_log("GetProcAddress", line, line, (uint64_t)r);
    }
    return r;
}
static HMODULE WINAPI h_LoadLibraryExW(LPCWSTR a, HANDLE f, DWORD fl) { HMODULE r = LoadLibraryExW(a, f, fl); fs_log("LoadLibraryExW", a, a, (uint64_t)r); return r; }
static HMODULE WINAPI h_LoadLibraryW(LPCWSTR a) { HMODULE r = LoadLibraryW(a); fs_log("LoadLibraryW", a, a, (uint64_t)r); return r; }
static HMODULE WINAPI h_LoadLibraryExA(LPCSTR s, HANDLE f, DWORD fl) { wchar_t a[512]; MultiByteToWideChar(CP_ACP, 0, s ? s : "", -1, a, 512); HMODULE r = LoadLibraryExA(s, f, fl); fs_log("LoadLibraryExA", a, a, (uint64_t)r); return r; }
/* Window: borderless, covering the monitor the window opens on (set GTA6_WINDOWED=1 to keep the game's own style) */
static HWND WINAPI h_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    int full = !parent && !GetEnvironmentVariableA("GTA6_WINDOWED", NULL, 0);
    if (full) {
        HMONITOR mon = MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi = { sizeof mi };
        if (GetMonitorInfoA(mon, &mi)) {
            style = WS_POPUP | (style & WS_VISIBLE); ex &= ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME);
            x = mi.rcMonitor.left; y = mi.rcMonitor.top; w = mi.rcMonitor.right - mi.rcMonitor.left; h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        }
    }
    HWND r = CreateWindowExA(ex, cls, title, style, x, y, w, h, parent, menu, inst, param);
    wchar_t t[200]; MultiByteToWideChar(CP_ACP, 0, title && ((uint64_t)title >> 16) ? title : "(none)", -1, t, 200);
    fs_log(full ? "CreateWindowExA[fullscreen-borderless]" : "CreateWindowExA", t, t, (uint64_t)r);
    if (r && !parent) SetEnvironmentVariableA("GTA6_HWND", (char[32]){0}) , (void)0;
    if (r && !parent) { char hb[32]; snprintf(hb, sizeof hb, "%llX", (uint64_t)r); SetEnvironmentVariableA("GTA6_HWND", hb); }
    return r;
}
#include "binkview.h"
static const struct { const char* name; void* fn; } g_hooks[] = {
    { "CreateWindowExA", h_CreateWindowExA },
    { "BinkDoFrame", h_BinkDoFrame }, { "BinkDoFrameAsyncWait", h_BinkDoFrameAsyncWait },
    { "GetProcAddress", h_GetProcAddress }, { "LoadLibraryExW", h_LoadLibraryExW }, { "LoadLibraryW", h_LoadLibraryW }, { "LoadLibraryExA", h_LoadLibraryExA },
    { "CreateFileW", h_CreateFileW }, { "CreateDirectoryW", h_CreateDirectoryW }, { "DeleteFileW", h_DeleteFileW },
    { "FindFirstFileW", h_FindFirstFileW }, { "GetDiskFreeSpaceExW", h_GetDiskFreeSpaceExW }, { "GetFileAttributesExW", h_GetFileAttributesExW },
    { "GetFileAttributesW", h_GetFileAttributesW }, { "GetFileAttributesA", h_GetFileAttributesA }, { "RemoveDirectoryW", h_RemoveDirectoryW },
    { "SetFileAttributesW", h_SetFileAttributesW }, { "MoveFileExW", h_MoveFileExW },
};
static void fs_install(void) {
    BYTE* base = (BYTE*)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return;
    int patched = 0;
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; d++) {
        if (!d->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA* names = (IMAGE_THUNK_DATA*)(base + d->OriginalFirstThunk);
        IMAGE_THUNK_DATA* iat = (IMAGE_THUNK_DATA*)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            const char* nm = ((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name;
            for (int i = 0; i < (int)(sizeof g_hooks / sizeof g_hooks[0]); i++) if (!strcmp(nm, g_hooks[i].name)) {
                if (!strcmp(nm, "BinkDoFrame")) g_bkDo = (BinkDoFrameFn)iat->u1.Function;
                if (!strcmp(nm, "BinkDoFrameAsyncWait")) g_bkWait = (BinkWaitFn)iat->u1.Function;
                DWORD old; if (VirtualProtect(&iat->u1.Function, 8, PAGE_READWRITE, &old)) { iat->u1.Function = (ULONGLONG)g_hooks[i].fn; VirtualProtect(&iat->u1.Function, 8, old, &old); patched++; }
            }
        }
    }
    fs_init();
    fs_log("fs_install: hooks patched / root", g_root, g_root, patched);
}
