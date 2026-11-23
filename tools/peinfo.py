import sys, struct, math, mmap, re
p = sys.argv[1]
f = open(p, 'rb'); m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
pe = struct.unpack_from('<I', m, 0x3C)[0]
nsec, = struct.unpack_from('<H', m, pe+6); optsz, = struct.unpack_from('<H', m, pe+20)
opt = pe+24
secs = []
so = opt+optsz
for i in range(nsec):
    name = m[so:so+8].rstrip(b'\0').decode('latin1'); vs, va, rs, rp = struct.unpack_from('<IIII', m, so+8)
    secs.append((name, va, vs, rp, rs)); so += 40
def off(rva):
    for n, va, vs, rp, rs in secs:
        if va <= rva < va+max(vs, rs): return rp+(rva-va)
    return None
def ent(b):
    if not b: return 0
    c = [0]*256
    for x in b: c[x] += 1
    return -sum(k/len(b)*math.log2(k/len(b)) for k in c if k)
for n, va, vs, rp, rs in secs:
    step = max(1, rs//(4<<20)); samp = bytes(m[rp:rp+rs:step]) if rs else b''
    print(f"{n:9} entropy~{ent(samp):.2f}")
dd = opt+112
names = ['export','import','resource','exception','security','reloc','debug','arch','gp','tls','loadcfg','boundimp','iat','delayimp','clr']
dirs = {}
for i, nm in enumerate(names):
    rva, sz = struct.unpack_from('<II', m, dd+i*8); dirs[nm] = (rva, sz)
    if rva: print(f"dir {nm}: rva=0x{rva:X} size=0x{sz:X}")
def cstr(o):
    e = m.find(b'\0', o); return m[o:e].decode('latin1')
def imports(rva, delay=False):
    o = off(rva)
    if o is None: print(' (unmapped)'); return
    while True:
        if delay:
            a = struct.unpack_from('<8I', m, o); nrva, ilt = a[1], a[4]; o += 32
            if not nrva: break
        else:
            ilt, _, _, nrva, iat = struct.unpack_from('<5I', m, o); o += 20
            if not nrva and not ilt: break
            ilt = ilt or iat
        dll = cstr(off(nrva)); fns = []; t = off(ilt)
        while t is not None:
            v, = struct.unpack_from('<Q', m, t); t += 8
            if not v: break
            fns.append(f"#{v&0xFFFF}" if v >> 63 else cstr(off(v & 0x7FFFFFFF)+2))
        print(f"  {dll} ({len(fns)}): {', '.join(fns[:12])}{' ...' if len(fns)>12 else ''}")
print("IMPORTS:"); imports(dirs['import'][0]) if dirs['import'][0] else print('  none')
if dirs['delayimp'][0]: print("DELAY IMPORTS:"); imports(dirs['delayimp'][0], True)
# debug dir / pdb path
rva, sz = dirs['debug']
if rva:
    o = off(rva)
    for i in range(sz//28):
        typ, dsz, _, dptr = struct.unpack_from('<IIII', m, o+i*28+12)
        if typ == 2 and m[dptr:dptr+4] == b'RSDS': print("PDB:", cstr(dptr+24))
# overlay
end = max(rp+rs for _, _, _, rp, rs in secs)
print(f"file size {len(m)}, end of sections {end}, overlay {len(m)-end} bytes")
# interesting strings in rdata/data
pat = re.compile(rb'(https?://[\x20-\x7e]{6,120}|[\x20-\x7e]{0,40}(?:discord|telegram|webhook|pastebin|powershell|cmd\.exe|schtasks|wallet|\AppData\|Login Data|stealer)[\x20-\x7e]{0,60})', re.I)
seen = set()
for n, va, vs, rp, rs in secs:
    if n in ('.rdata', '.data'):
        for mt in pat.finditer(m, rp, rp+rs):
            s = mt.group(0).decode('latin1')
            if s not in seen:
                seen.add(s)
                if len(seen) <= 60: print("STR:", s)
print("distinct suspicious/url strings:", len(seen))
for kw in (b'rage::', b'Rockstar', b'XGameRuntime', b'RPF9', b'socialclub', b'GTA6', b'gta6', b'Leonida'):
    print(kw.decode(), m.find(kw) != -1)
