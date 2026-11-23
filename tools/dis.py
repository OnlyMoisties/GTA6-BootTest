import sys, struct, mmap
import capstone
f = open(sys.argv[1], 'rb'); m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
pe = struct.unpack_from('<I', m, 0x3C)[0]; nsec, = struct.unpack_from('<H', m, pe+6); optsz, = struct.unpack_from('<H', m, pe+20)
opt = pe+24; so = opt+optsz; secs = []
for i in range(nsec):
    vs, va, rs, rp = struct.unpack_from('<IIII', m, so+8); secs.append((va, vs, rp, rs)); so += 40
def off(rva):
    for va, vs, rp, rs in secs:
        if va <= rva < va+rs: return rp+(rva-va)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
mode = sys.argv[2]
if mode == 'guid':
    for a in sys.argv[3:]:
        r = int(a, 16); b = m[off(r):off(r)+16]; d1, d2, d3 = struct.unpack_from('<IHH', b)
        print("rva 0x%X GUID {%08X-%04X-%04X-%s-%s}" % (r, d1, d2, d3, b[8:10].hex().upper(), b[10:].hex().upper()))
elif mode == 'dis':
    start = int(sys.argv[3], 16); n = int(sys.argv[4], 16)
    for i in md.disasm(m[off(start):off(start)+n], start):
        print("0x%X: %s %s" % (i.address, i.mnemonic, i.op_str))
elif mode == 'str':
    for a in sys.argv[3:]:
        r = int(a, 16); o = off(r); e = m.find(b'\0', o); print("rva 0x%X: %r" % (r, m[o:e][:200]))
