#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <intrin.h>
#include <stdarg.h>
#include <stdlib.h>
#define EXP __declspec(dllexport)
#define MAXLOG 200
static void slog_(const char* fn, void* ra, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t ret) {
    char path[MAX_PATH], buf[512];
    if (!GetEnvironmentVariableA("GTA6_STUB_LOG", path, MAX_PATH)) return;
    uint64_t base = (uint64_t)GetModuleHandleA(NULL), r = (uint64_t)ra;
    int inexe = (r >= base && r < base + 0xB100000);
    int n = snprintf(buf, sizeof buf, "%llu [" STUBDLL "] %s(0x%llX, 0x%llX, 0x%llX, 0x%llX, 0x%llX) -> 0x%llX caller=%s0x%llX tid=%lu\r\n",
        GetTickCount64(), fn, a, b, c, d, e, ret, inexe ? "exe+" : "", inexe ? r - base : r, GetCurrentThreadId());
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, buf, n, &w, NULL); CloseHandle(h); }
}
#define SLOG(fn, a, b, c, d, e, ret) do { static volatile LONG cnt_; if (InterlockedIncrement(&cnt_) <= MAXLOG) slog_(fn, _ReturnAddress(), (uint64_t)(a), (uint64_t)(b), (uint64_t)(c), (uint64_t)(d), (uint64_t)(e), (uint64_t)(ret)); } while (0)
/* generic logging stub: returns a fixed value */
#define STUB(name, retval) EXP uint64_t name(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) { SLOG(#name, a, b, c, d, e, retval); return (uint64_t)(retval); }
#define E_FAIL_ 0x80004005ull
#define E_NOTIMPL_ 0x80004001ull
