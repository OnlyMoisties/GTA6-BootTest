import sys, struct, mmap, capstone
f = open(sys.argv[1], 'rb'); m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
pe = struct.unpack_from('<I', m, 0x3C)[0]; nsec, = struct.unpack_from('<H', m, pe+6); optsz, = struct.unpack_from('<H', m, pe+20)
opt = pe+24; so = opt+optsz; secs = []
for i in range(nsec):
    vs, va, rs, rp = struct.unpack_from('<IIII', m, so+8); secs.append((va, vs, rp, rs)); so += 40
def off(rva):
    for va, vs, rp, rs in secs:
        if va <= rva < va+rs: return rp+(rva-va)
def cstr(o): return m[o:m.find(b'\0', o)].decode('latin1')
iat = {}
rva, = struct.unpack_from('<I', m, opt+112+8); o = off(rva)
while True:
    ilt, _, _, nrva, first = struct.unpack_from('<5I', m, o); o += 20
    if not nrva: break
    dll = cstr(off(nrva)); t = off(ilt); k = 0
    while True:
        v, = struct.unpack_from('<Q', m, t + k*8)
        if not v: break
        iat[first + k*8] = dll + '!' + (("#%d" % (v & 0xFFFF)) if v >> 63 else cstr(off(v & 0x7FFFFFFF) + 2)); k += 1
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for a in sys.argv[2:]:
    r = int(a, 16); ins = next(md.disasm(m[off(r):off(r)+16], r))
    tgt = r + ins.size + int(ins.op_str.split('rip + ')[1].rstrip(']'), 16) if 'rip +' in ins.op_str else None
    print("0x%X: %s %s -> %s" % (r, ins.mnemonic, ins.op_str, iat.get(tgt, '?')))
