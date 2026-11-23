import sys, struct, mmap, os, json
p = sys.argv[1]; gamedir = os.path.dirname(p)
f = open(p, 'rb'); m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
pe = struct.unpack_from('<I', m, 0x3C)[0]
nsec, = struct.unpack_from('<H', m, pe+6); optsz, = struct.unpack_from('<H', m, pe+20)
opt = pe+24; so = opt+optsz; secs = []
for i in range(nsec):
    vs, va, rs, rp = struct.unpack_from('<IIII', m, so+8); secs.append((va, vs, rp, rs)); so += 40
def off(rva):
    for va, vs, rp, rs in secs:
        if va <= rva < va+max(vs, rs): return rp+(rva-va)
def cstr(o):
    e = m.find(b'\0', o); return m[o:e].decode('latin1')
rva, = struct.unpack_from('<I', m, opt+112+8); o = off(rva); out = {}
while True:
    ilt, _, _, nrva, iat = struct.unpack_from('<5I', m, o); o += 20
    if not nrva: break
    dll = cstr(off(nrva)); t = off(ilt or iat); fns = []
    while True:
        v, = struct.unpack_from('<Q', m, t); t += 8
        if not v: break
        fns.append(f"#{v&0xFFFF}" if v >> 63 else cstr(off(v & 0x7FFFFFFF)+2))
    out[dll] = fns
sysdirs = [gamedir, r'C:\Windows\System32']
for dll, fns in out.items():
    low = dll.lower()
    if low.startswith(('api-ms-', 'ext-ms-')): continue
    where = next((d for d in sysdirs if os.path.exists(os.path.join(d, dll))), None)
    tag = 'GAMEDIR' if where == gamedir else ('SYSTEM' if where else 'MISSING')
    print(f"{tag:8} {dll} ({len(fns)})")
    if tag == 'MISSING': print("         " + ", ".join(fns))
json.dump(out, open(sys.argv[2], 'w'), indent=1)
# exports of the exe + dynamic-load candidates
for kw in (b'XGameRuntime.dll', b'xgameruntime', b'gameos', b'kernelx', b'XSAPI', b'Microsoft.Xbox', b'GameChat', b'PlayFab', b'Party'):
    i = m.find(kw); print(kw.decode(), '->', cstr(max(i-0, 0))[:80] if i != -1 else None)
