/* Proxies for interfaces handed out by xgameruntime.dll!QueryApiImpl (one proxy per distinct real object).
   Each proxy method is a C wrapper forwarding 10 integer-register/stack arguments to the real method and
   logging failing HRESULTs plus per-method call counts. (Float arguments in xmm registers are not preserved
   by this wrapper; set GTA6_NO_XGR_PROXY=1 to disable proxies if something misbehaves.) */
#define XP_SLOTS 128
#define XP_MAX 64
typedef struct { void** vtbl; void* real; int idx; } XProxy;
typedef struct { GUID prov, iface; void* real; XProxy proxy; volatile LONG cnt[XP_SLOTS]; LONG last[XP_SLOTS]; volatile LONG fails; } XPEntry;
static XPEntry g_xp[XP_MAX];
static volatile LONG g_nxp;
static SRWLOCK g_xplock = SRWLOCK_INIT;
typedef HRESULT (WINAPI *QueryApiImplFn)(const GUID*, const GUID*, void**);
static QueryApiImplFn g_realQuery;

static void xp_log(const char* fmt, ...) {
    char path[MAX_PATH], buf[1024];
    if (!GetEnvironmentVariableA("GTA6_STUB_LOG", path, MAX_PATH)) return;
    int n = snprintf(buf, 64, "%llu [xgr] ", GetTickCount64());
    va_list ap; va_start(ap, fmt); n += vsnprintf(buf + n, sizeof buf - n - 3, fmt, ap); va_end(ap);
    if (n > (int)sizeof buf - 3) n = sizeof buf - 3;
    buf[n++] = '\r'; buf[n++] = '\n';
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, buf, n, &w, NULL); CloseHandle(h); }
}
typedef uint64_t (*Fwd10)(void*, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
/* ---- local offline user ----
   XUser interface {01ACD177}: m8 = add-user result (async, &handle). When the real runtime has no user, hand back
   a fake handle. Any later runtime call that carries the fake handle is answered here (S_OK) and logged, never forwarded. */
static uint8_t g_fakeuser_blk[256];
#define FAKEUSER ((uint64_t)(uintptr_t)g_fakeuser_blk)
static int xp_has_fake(uint64_t* v, int n) { for (int i = 0; i < n; i++) if (v[i] == FAKEUSER) return 1; return 0; }
static volatile LONG g_fakecalls;
#define FAKE_LOCALID 0x1ull
#define FAKE_XUID 0x0009FFFF00000001ull
/* Answers for XUser methods (public XUser.h order) on the local user. Returns 1 if handled. */
static int xp_fakeuser(int slot, uint64_t* a, uint64_t* ret) {
    *ret = 0;
    switch (slot) {
    case 3: if (a[1]) *(uint64_t*)a[1] = FAKEUSER; return 1;                    /* DuplicateHandle(user, &dup) */
    case 4: return 1;                                                           /* CloseHandle(user) */
    case 5: if (a[2]) *(int32_t*)a[2] = (a[0] == a[1]) ? 0 : 1; return 1;       /* Compare(u1, u2, &result) */
    case 9: if (a[1]) *(uint64_t*)a[1] = FAKE_LOCALID; return 1;                /* GetLocalId(user, &id) */
    case 11: if (a[1]) *(uint64_t*)a[1] = FAKE_XUID; return 1;                  /* GetId(user, &xuid) */
    case 13: if (a[1]) *(uint8_t*)a[1] = 0; return 1;                           /* GetIsGuest(user, &isGuest) */
    case 14: if (a[1]) *(uint32_t*)a[1] = 0; return 1;                          /* GetState(user, &state) = SignedIn */
    case 15: {                                                                  /* GetGamertag(user, component, size, buf, &used) */
        const char* gt = "Player"; size_t n = strlen(gt) + 1;
        if (a[3] && a[2] >= n) memcpy((void*)a[3], gt, n);
        if (a[4]) *(uint64_t*)a[4] = n;
        return 1; }
    case 19: if (a[1]) *(uint32_t*)a[1] = 3; return 1;                          /* GetAgeGroup(user, &group) = Adult */
    case 20: if (a[3]) *(uint8_t*)a[3] = 1; if (a[4]) *(uint32_t*)a[4] = 0; return 1; /* CheckPrivilege(user, opts, priv, &has, &reason) */
    }
    return 0;
}
static uint64_t xp_call(int slot, XProxy* p, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8, uint64_t a9, uint64_t a10, void* ra) {
    XPEntry* e = &g_xp[p->idx];
    InterlockedIncrement(&e->cnt[slot]);
    uint64_t av[10] = { a1, a2, a3, a4, a5, a6, a7, a8, a9, a10 };
    int fakeon = !GetEnvironmentVariableA("GTA6_NO_FAKE_USER", NULL, 0);
    int isuser = (e->iface.Data1 == 0x01ACD177);
    if (fakeon && isuser && slot == 7) av[0] = a1 = 1;                         /* AddAsync: AddDefaultUserSilently -> no sign-in popup */
    if (fakeon && isuser && ((slot == 10 && a1 == FAKE_LOCALID) || (slot == 12 && a1 == FAKE_XUID))) {   /* FindUserBy{LocalId,Id} */
        if (a2) *(uint64_t*)a2 = FAKEUSER;
        return 0;
    }
    if (slot > 2 && fakeon && xp_has_fake(av, 6)) {
        uint64_t r = 0; int known = isuser && xp_fakeuser(slot, av, &r);
        if (!known && e->iface.Data1 == 0xCEF4FAC0 && slot == 3) {           /* XUserGetGamertag(user, component, size, buf, &used) */
            const char* gt = "Player"; uint64_t n = strlen(gt) + 1;
            if (a4 && a3 >= n) memcpy((void*)a4, gt, (size_t)n);
            if (a5) *(uint64_t*)a5 = n;
            known = 1;
        }
        if (!known && e->iface.Data1 == 0x1BFFF3AF && slot == 4) { r = 0x80004005; known = 1; }   /* save provider init: unavailable */
        /* Store: report "unavailable" for anything tied to the local user. Licence/ownership answers are deliberately NOT faked. */
        if (!known && e->iface.Data1 == 0xB09D803C) { if (a2 && slot == 3) *(uint64_t*)a2 = 0; r = 0x80004005; known = 1; }
        if (InterlockedIncrement(&g_fakecalls) <= 300)
            xp_log("FAKEUSER call iface#%d {%08lX-%04X} m%d(0x%llX, 0x%llX, 0x%llX, 0x%llX, 0x%llX) -> 0x%llX (%s) caller=exe+0x%llX",
                p->idx, e->iface.Data1, e->iface.Data2, slot, a1, a2, a3, a4, a5, r, known ? "answered" : "UNKNOWN, S_OK",
                (uint64_t)ra - (uint64_t)GetModuleHandleA(NULL));
        return r;
    }
    Fwd10 f = (Fwd10)(*(void***)p->real)[slot];
    uint64_t r = f(p->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
    if (e->iface.Data1 == 0x01ACD177 && slot == 8 && (int32_t)r < 0 && a2 && !GetEnvironmentVariableA("GTA6_NO_FAKE_USER", NULL, 0)) {
        *(uint64_t*)a2 = FAKEUSER;
        xp_log("FAKEUSER created: XUser add-result failed with 0x%08llX, returned local user handle %p instead", r, (void*)FAKEUSER);
        return 0;
    }
    if ((int32_t)r < 0 && (r >> 32) == 0 && slot != 1 && slot != 2 && InterlockedIncrement(&e->fails) <= 30)
        xp_log("FAIL iface#%d {%08lX-%04X} m%d(0x%llX, 0x%llX, 0x%llX, 0x%llX) -> 0x%08llX caller=exe+0x%llX",
            p->idx, e->iface.Data1, e->iface.Data2, slot, a1, a2, a3, a4, r, (uint64_t)ra - (uint64_t)GetModuleHandleA(NULL));
    return r;
}
#define XW(n) static uint64_t xw##n(XProxy* p, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8, uint64_t a9, uint64_t a10) { return xp_call(n, p, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, _ReturnAddress()); }
XW(0) XW(1) XW(2) XW(3) XW(4) XW(5) XW(6) XW(7) XW(8) XW(9) XW(10) XW(11) XW(12) XW(13) XW(14) XW(15) 
XW(16) XW(17) XW(18) XW(19) XW(20) XW(21) XW(22) XW(23) XW(24) XW(25) XW(26) XW(27) XW(28) XW(29) XW(30) XW(31) 
XW(32) XW(33) XW(34) XW(35) XW(36) XW(37) XW(38) XW(39) XW(40) XW(41) XW(42) XW(43) XW(44) XW(45) XW(46) XW(47) 
XW(48) XW(49) XW(50) XW(51) XW(52) XW(53) XW(54) XW(55) XW(56) XW(57) XW(58) XW(59) XW(60) XW(61) XW(62) XW(63) 
XW(64) XW(65) XW(66) XW(67) XW(68) XW(69) XW(70) XW(71) XW(72) XW(73) XW(74) XW(75) XW(76) XW(77) XW(78) XW(79) 
XW(80) XW(81) XW(82) XW(83) XW(84) XW(85) XW(86) XW(87) XW(88) XW(89) XW(90) XW(91) XW(92) XW(93) XW(94) XW(95) 
XW(96) XW(97) XW(98) XW(99) XW(100) XW(101) XW(102) XW(103) XW(104) XW(105) XW(106) XW(107) XW(108) XW(109) XW(110) XW(111) 
XW(112) XW(113) XW(114) XW(115) XW(116) XW(117) XW(118) XW(119) XW(120) XW(121) XW(122) XW(123) XW(124) XW(125) XW(126) XW(127) 
static void* g_xpvt[XP_SLOTS] = { xw0, xw1, xw2, xw3, xw4, xw5, xw6, xw7, xw8, xw9, xw10, xw11, xw12, xw13, xw14, xw15, xw16, xw17, xw18, xw19, xw20, xw21, xw22, xw23, xw24, xw25, xw26, xw27, xw28, xw29, xw30, xw31, xw32, xw33, xw34, xw35, xw36, xw37, xw38, xw39, xw40, xw41, xw42, xw43, xw44, xw45, xw46, xw47, xw48, xw49, xw50, xw51, xw52, xw53, xw54, xw55, xw56, xw57, xw58, xw59, xw60, xw61, xw62, xw63, xw64, xw65, xw66, xw67, xw68, xw69, xw70, xw71, xw72, xw73, xw74, xw75, xw76, xw77, xw78, xw79, xw80, xw81, xw82, xw83, xw84, xw85, xw86, xw87, xw88, xw89, xw90, xw91, xw92, xw93, xw94, xw95, xw96, xw97, xw98, xw99, xw100, xw101, xw102, xw103, xw104, xw105, xw106, xw107, xw108, xw109, xw110, xw111, xw112, xw113, xw114, xw115, xw116, xw117, xw118, xw119, xw120, xw121, xw122, xw123, xw124, xw125, xw126, xw127 };

static DWORD WINAPI xp_dumper(void* unused) {
    for (;;) {
        Sleep(3000);
        LONG n = g_nxp; if (n > XP_MAX) n = XP_MAX;
        for (LONG i = 0; i < n; i++) {
            XPEntry* e = &g_xp[i]; char line[900]; size_t k = 0; int any = 0;
            for (int s = 3; s < XP_SLOTS && k < 800; s++) {
                LONG c = e->cnt[s];
                if (c != e->last[s]) { k += snprintf(line + k, sizeof line - k, " m%d=%ld", s, c); e->last[s] = c; any = 1; }
            }
            if (any) xp_log("iface#%ld {%08lX-%04X} prov {%08lX} calls:%s", i, e->iface.Data1, e->iface.Data2, e->prov.Data1, line);
        }
    }
    return 0;
}
static HRESULT WINAPI h_QueryApiImpl(const GUID* prov, const GUID* iface, void** ppv) {
    HRESULT hr = g_realQuery(prov, iface, ppv);
    if (hr < 0 || !ppv || !*ppv || GetEnvironmentVariableA("GTA6_NO_XGR_PROXY", NULL, 0)) return hr;
    void* real = *ppv; LONG found = -1;
    AcquireSRWLockExclusive(&g_xplock);
    for (LONG i = 0; i < g_nxp; i++) if (g_xp[i].real == real && IsEqualGUID(&g_xp[i].iface, iface)) { found = i; break; }
    if (found < 0 && g_nxp < XP_MAX) {
        found = g_nxp; XPEntry* e = &g_xp[found];
        if (prov) e->prov = *prov; e->iface = *iface; e->real = real;
        e->proxy.vtbl = g_xpvt; e->proxy.real = real; e->proxy.idx = found;
        MemoryBarrier(); g_nxp++;
        xp_log("new iface#%ld prov={%08lX-%04X-%04X} iface={%08lX-%04X-%04X} real=%p", found, prov ? prov->Data1 : 0, prov ? prov->Data2 : 0, prov ? prov->Data3 : 0, iface->Data1, iface->Data2, iface->Data3, real);
    }
    ReleaseSRWLockExclusive(&g_xplock);
    if (found >= 0) *ppv = &g_xp[found].proxy;
    static volatile LONG started; if (!InterlockedExchange(&started, 1)) CloseHandle(CreateThread(NULL, 0, xp_dumper, NULL, 0, NULL));
    return hr;
}
