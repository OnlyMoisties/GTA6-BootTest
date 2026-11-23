/* Debugger harness: launches the exe with the stub dir on PATH, logs loader/exception/exit events, kills on timeout. */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>

static FILE* g_log;
static ULONG64 g_base, g_size = 0xB100000;
static HANDLE g_proc;
static struct { DWORD id; HANDLE h; } g_thr[512];
static int g_nthr;

static void L(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char b[2048]; vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    printf("%s\n", b);
    if (g_log) { fprintf(g_log, "%llu %s\n", GetTickCount64(), b); fflush(g_log); }
}
static const char* A(ULONG64 a) {
    static char s[4][40]; static int i; char* o = s[i++ & 3];
    if (a >= g_base && a < g_base + g_size) snprintf(o, 40, "exe+0x%llX", a - g_base); else snprintf(o, 40, "0x%llX", a);
    return o;
}
static struct { ULONG64 rva; BYTE orig; int hits; } g_bp[32];
static int g_nbp, g_skip, g_maxhits = 3, g_stepbp = -1, g_nskip, g_cores;
static DWORD g_steptid;
/* describe remote memory: ascii string, wide string, or hex */
static const char* R(ULONG64 p) {
    static char s[6][300]; static int k; char* o = s[k++ % 6]; unsigned char b[128]; SIZE_T got = 0; o[0] = 0;
    if (p < 0x10000 || !ReadProcessMemory(g_proc, (void*)p, b, sizeof b, &got) || got < 16) return o;
    int n = 0; while (n < (int)got - 1 && b[n] >= 0x20 && b[n] < 0x7F) n++;
    if (n >= 3 && b[n] == 0) { snprintf(o, 300, " \"%.*s\"", n, b); return o; }
    n = 0; while (n * 2 + 1 < (int)got - 2 && b[n * 2] >= 0x20 && b[n * 2] < 0x7F && b[n * 2 + 1] == 0) n++;
    if (n >= 3) { char t[80]; for (int i = 0; i < n && i < 79; i++) t[i] = b[i * 2]; t[n < 79 ? n : 79] = 0; snprintf(o, 300, " L\"%s\"", t); return o; }
    int m = 0; m += snprintf(o + m, 300 - m, " ["); for (int i = 0; i < 40; i++) m += snprintf(o + m, 300 - m, "%02X%s", b[i], i % 8 == 7 ? " " : ""); snprintf(o + m, 300 - m, "]");
    return o;
}
static ULONG64 g_watch; static int g_nwatch;
static void arm_watch(HANDLE h) {
    if (!g_watch || !h) return;
    CONTEXT c; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(h, &c)) return;
    c.Dr0 = g_watch; c.Dr7 = (c.Dr7 & ~0xF0003ull) | 1 | (1ull << 16) | (3ull << 18);   /* L0, write-only, 8 bytes */
    SetThreadContext(h, &c);
}
static void setbyte(ULONG64 a, BYTE v) { SIZE_T w; WriteProcessMemory(g_proc, (void*)a, &v, 1, &w); FlushInstructionCache(g_proc, (void*)a, 1); }
static HANDLE thr(DWORD id) { for (int i = 0; i < g_nthr; i++) if (g_thr[i].id == id) return g_thr[i].h; return NULL; }
static void dump(HANDLE h, const char* tag) {
    CONTEXT c; c.ContextFlags = CONTEXT_FULL;
    if (!h || !GetThreadContext(h, &c)) return;
    L("  %s rip=%s rsp=0x%llX rax=0x%llX rcx=0x%llX rdx=0x%llX r8=0x%llX r9=0x%llX", tag, A(c.Rip), c.Rsp, c.Rax, c.Rcx, c.Rdx, c.R8, c.R9);
    ULONG64 st[96]; SIZE_T got = 0; char line[1024] = ""; int n = 0;
    if (ReadProcessMemory(g_proc, (void*)c.Rsp, st, sizeof st, &got))
        for (unsigned i = 0; i < got / 8 && n < 14; i++)
            if (st[i] >= g_base + 0x1000 && st[i] < g_base + 0x5F41000) { strcat(line, A(st[i])); strcat(line, " "); n++; }
    if (n) L("  %s stack(exe addrs): %s", tag, line);
}
static void av(EXCEPTION_RECORD* r) {
    if (r->ExceptionCode == 0xC0000005 && r->NumberParameters >= 2)
        L("  access violation %s 0x%llX", r->ExceptionInformation[0] ? "writing" : "reading", (ULONG64)r->ExceptionInformation[1]);
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 6) { printf("usage: harness <exe> <stubdir> <logdir> <timeoutSec> <load|run> [game args]\n"); return 2; }
    int timeout = _wtoi(argv[4]);
    int loadonly = !wcscmp(argv[5], L"load");
    static wchar_t p[32768], old[30000];
    wchar_t cwd[MAX_PATH], t[MAX_PATH * 2];
    swprintf(t, MAX_PATH * 2, L"%s\\harness.log", argv[3]); g_log = _wfopen(t, L"w");
    swprintf(t, MAX_PATH * 2, L"%s\\stubs.log", argv[3]); DeleteFileW(t); SetEnvironmentVariableW(L"GTA6_STUB_LOG", t);
    GetEnvironmentVariableW(L"PATH", old, 30000);
    swprintf(p, 32768, L"%s;%s", argv[2], old); SetEnvironmentVariableW(L"PATH", p);
    wcscpy(cwd, argv[1]); wchar_t* s = wcsrchr(cwd, L'\\'); if (s) *s = 0;
    swprintf(p, 32768, L"\"%s\"", argv[1]);
    for (int i = 6; i < argc; i++) {
        if (!wcscmp(argv[i], L"-skip")) g_skip = 1;
        else if (!wcsncmp(argv[i], L"-maxhits=", 9)) g_maxhits = _wtoi(argv[i] + 9);
        else if (!wcsncmp(argv[i], L"-cores=", 7)) g_cores = _wtoi(argv[i] + 7);
        else if (!wcsncmp(argv[i], L"-bp=", 4)) { wchar_t* q = argv[i] + 4; while (*q && g_nbp < 32) { g_bp[g_nbp++].rva = wcstoull(q, &q, 16); if (*q == L',') q++; } }
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
    if (!CreateProcessW(argv[1], p, NULL, NULL, FALSE, DEBUG_ONLY_THIS_PROCESS, NULL, cwd, &si, &pi)) { L("CreateProcess failed %lu", GetLastError()); return 1; }
    g_proc = pi.hProcess;
    if (g_cores > 0 && g_cores < 64) { DWORD_PTR mask = ((DWORD_PTR)1 << g_cores) - 1; SetProcessAffinityMask(pi.hProcess, mask); L("affinity: first %d logical CPUs", g_cores); }
    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)timeout * 1000;
    int firstbp = 1, nexc = 0, nods = 0, ndll = 0, done = 0;

    while (!done) {
        DEBUG_EVENT ev;
        BOOL got = WaitForDebugEvent(&ev, 500);
        if (got && GetTickCount64() > deadline) { ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE); got = FALSE; }   /* event flood (e.g. ODS spam) must not stop the timeout */
        if (!got) {
            if (GetTickCount64() > deadline) {
                L("TIMEOUT after %ds: process still alive (%d threads). Thread states:", timeout, g_nthr);
                for (int i = 0; i < g_nthr && i < 24; i++) { SuspendThread(g_thr[i].h); char tg[32]; snprintf(tg, 32, "tid %lu", g_thr[i].id); dump(g_thr[i].h, tg); }
                TerminateProcess(pi.hProcess, 99);
                break;
            }
            continue;
        }
        DWORD cont = DBG_CONTINUE;
        switch (ev.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT:
            g_base = (ULONG64)ev.u.CreateProcessInfo.lpBaseOfImage;
            g_thr[g_nthr].id = ev.dwThreadId; g_thr[g_nthr++].h = ev.u.CreateProcessInfo.hThread;
            L("PROCESS created pid=%lu base=0x%llX", ev.dwProcessId, g_base);
            if (ev.u.CreateProcessInfo.hFile) CloseHandle(ev.u.CreateProcessInfo.hFile);
            break;
        case CREATE_THREAD_DEBUG_EVENT:
            if (g_nthr < 512) { g_thr[g_nthr].id = ev.dwThreadId; g_thr[g_nthr++].h = ev.u.CreateThread.hThread; }
            arm_watch(ev.u.CreateThread.hThread);
            break;
        case LOAD_DLL_DEBUG_EVENT: {
            char n[MAX_PATH] = "?";
            if (ev.u.LoadDll.hFile) { GetFinalPathNameByHandleA(ev.u.LoadDll.hFile, n, MAX_PATH, 0); CloseHandle(ev.u.LoadDll.hFile); }
            if (ndll++ < 400) L("DLL %s @0x%llX", n, (ULONG64)ev.u.LoadDll.lpBaseOfDll);
            break; }
        case OUTPUT_DEBUG_STRING_EVENT: {
            char b[1024] = ""; SIZE_T got; WORD len = ev.u.DebugString.nDebugStringLength; if (len > 1023) len = 1023;
            ReadProcessMemory(pi.hProcess, ev.u.DebugString.lpDebugStringData, b, len, &got); b[1023] = 0;
            for (char* q = b; *q; q++) if (*q == '\r' || *q == '\n') *q = ' ';
            if (nods++ < 3000) L("ODS: %s", b);
            if (!strncmp(b, "GTA6WATCH 0x", 12)) {
                g_watch = strtoull(b + 12, NULL, 16); L("WATCH armed at 0x%llX on %d threads", g_watch, g_nthr);
                for (int i = 0; i < g_nthr; i++) { if (g_thr[i].id != ev.dwThreadId) SuspendThread(g_thr[i].h); arm_watch(g_thr[i].h); if (g_thr[i].id != ev.dwThreadId) ResumeThread(g_thr[i].h); }
            }
            break; }
        case EXCEPTION_DEBUG_EVENT: {
            EXCEPTION_RECORD* r = &ev.u.Exception.ExceptionRecord; DWORD code = r->ExceptionCode;
            if (code == 0x80000003 && firstbp) {
                firstbp = 0; L("LOADER OK: initial breakpoint reached - every static import resolved");
                if (loadonly) TerminateProcess(pi.hProcess, 0);
                for (int i = 0; i < g_nbp; i++) { SIZE_T g; ReadProcessMemory(g_proc, (void*)(g_base + g_bp[i].rva), &g_bp[i].orig, 1, &g); setbyte(g_base + g_bp[i].rva, 0xCC); }
                break;
            }
            if (code == 0x80000003) {
                ULONG64 a = (ULONG64)r->ExceptionAddress; int hit = -1;
                for (int i = 0; i < g_nbp; i++) if (g_base + g_bp[i].rva == a) hit = i;
                if (hit >= 0) {
                    HANDLE h = thr(ev.dwThreadId); CONTEXT c; c.ContextFlags = CONTEXT_FULL; GetThreadContext(h, &c);
                    ULONG64 ret = 0; SIZE_T g; ReadProcessMemory(g_proc, (void*)c.Rsp, &ret, 8, &g);
                    g_bp[hit].hits++;
                    L("BP exe+0x%llX hit#%d tid=%lu ret=%s rax=0x%llX%s", g_bp[hit].rva, g_bp[hit].hits, ev.dwThreadId, A(ret), c.Rax, R(c.Rax));
                    L("   rcx=0x%llX%s", c.Rcx, R(c.Rcx)); L("   rdx=0x%llX%s", c.Rdx, R(c.Rdx)); L("   r8=0x%llX%s", c.R8, R(c.R8)); L("   r9=0x%llX%s", c.R9, R(c.R9));
                    L("   rbx=0x%llX rsi=0x%llX%s rdi=0x%llX%s", c.Rbx, c.Rsi, R(c.Rsi), c.Rdi, R(c.Rdi));
                    { ULONG64 st[40]; SIZE_T g2 = 0; char ln[900] = ""; size_t k = 0;
                      if (ReadProcessMemory(g_proc, (void*)c.Rsp, st, sizeof st, &g2)) { for (int i = 0; i < 40; i++) k += snprintf(ln + k, sizeof ln - k, "%s+%02X=%llX", i ? " " : "", i * 8, st[i]); L("   stack: %s", ln); } }
                    setbyte(a, g_bp[hit].orig); c.Rip = a; c.EFlags |= 0x100; SetThreadContext(h, &c);
                    g_stepbp = hit; g_steptid = ev.dwThreadId; break;
                }
                if (g_skip && a >= g_base && a < g_base + g_size) { if (g_nskip++ < 300) { L("ASSERT skipped at %s tid=%lu", A(a), ev.dwThreadId); if (g_nskip <= 40) dump(thr(ev.dwThreadId), "ctx"); } break; }
            }
            if (code == 0x80000004 && g_watch) {
                HANDLE h = thr(ev.dwThreadId); CONTEXT c; c.ContextFlags = CONTEXT_FULL | CONTEXT_DEBUG_REGISTERS;
                if (h && GetThreadContext(h, &c) && (c.Dr6 & 1)) {
                    ULONG64 v = 0; SIZE_T g; ReadProcessMemory(g_proc, (void*)g_watch, &v, 8, &g);
                    ULONG64 rv = c.Rip - g_base;
                    /* GTA6_WATCH pool-header filter (only for the heap-region watch): skip values that look like pool chunk headers */
                    int poolcode = !getenv("GTA6_WATCHEXE") && (((v & 0xFFFFFFFFull) < 0x4000 && ((v >> 32) & 0xFFFF) < 0x400 && v != 0) || rv == 0x55AA52A);
                    if (!poolcode && g_nwatch++ < 40) { L("WATCH write #%d at 0x%llX now=0x%llX by rip=%s tid=%lu", g_nwatch, g_watch, v, A(c.Rip), ev.dwThreadId); dump(h, "watch"); }
                    c.Dr6 = 0; SetThreadContext(h, &c);
                    cont = DBG_CONTINUE; break;
                }
            }
            if (code == 0x80000004 && g_stepbp >= 0 && ev.dwThreadId == g_steptid) {
                if (g_bp[g_stepbp].hits < g_maxhits) setbyte(g_base + g_bp[g_stepbp].rva, 0xCC);
                g_stepbp = -1; break;
            }
            cont = DBG_EXCEPTION_NOT_HANDLED;
            if (code == 0x406D1388 || code == 0x40010006 || code == 0x4001000A) break;
            if (ev.u.Exception.dwFirstChance) {
                /* access violations may be resolved in-process by the commit-on-touch handler: don't flood the log with them */
                static int nav; int quiet = (code == 0xC0000005 && ++nav > 6);
                if (!quiet && nexc++ < 60) {
                    L("EXC first-chance 0x%08lX at %s tid=%lu%s", code, A((ULONG64)r->ExceptionAddress), ev.dwThreadId, code == 0xE06D7363 ? " (C++ throw)" : "");
                    av(r); dump(thr(ev.dwThreadId), "ctx");
                }
            } else {
                L("FATAL unhandled exception 0x%08lX at %s tid=%lu", code, A((ULONG64)r->ExceptionAddress), ev.dwThreadId);
                av(r); dump(thr(ev.dwThreadId), "ctx");
                { CONTEXT c; c.ContextFlags = CONTEXT_FULL; HANDLE h = thr(ev.dwThreadId);
                  if (h && GetThreadContext(h, &c)) {
                      L("  rbx=0x%llX%s", c.Rbx, R(c.Rbx)); L("  rsi=0x%llX%s", c.Rsi, R(c.Rsi)); L("  rdi=0x%llX%s", c.Rdi, R(c.Rdi)); L("  rbp=0x%llX%s", c.Rbp, R(c.Rbp));
                      L("  r10=0x%llX%s", c.R10, R(c.R10)); L("  r11=0x%llX%s", c.R11, R(c.R11)); L("  r12=0x%llX%s", c.R12, R(c.R12));
                      L("  r13=0x%llX%s", c.R13, R(c.R13)); L("  r14=0x%llX%s", c.R14, R(c.R14)); L("  r15=0x%llX%s", c.R15, R(c.R15)); } }
                TerminateProcess(pi.hProcess, code);
            }
            break; }
        case EXIT_PROCESS_DEBUG_EVENT:
            L("PROCESS exited code=0x%08lX (threads seen: %d)", ev.u.ExitProcess.dwExitCode, g_nthr);
            done = 1;
            break;
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont);
    }
    if (g_log) fclose(g_log);
    return 0;
}
