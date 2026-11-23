import sys, struct, mmap, numpy as np, capstone
f = open(sys.argv[1], 'rb'); m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
pe = struct.unpack_from('<I', m, 0x3C)[0]; optsz, = struct.unpack_from('<H', m, pe+20); so = pe+24+optsz
vs, va, rs, rp = struct.unpack_from('<IIII', m, so+8)
t = np.frombuffer(m, dtype=np.uint8, count=rs, offset=rp)
d = (t[1:rs-3].astype(np.int64) | (t[2:rs-2].astype(np.int64) << 8) | (t[3:rs-1].astype(np.int64) << 16) | (t[4:rs].astype(np.int64) << 24))
d = np.where(d >= 2**31, d - 2**32, d)
pos = np.arange(rs-4, dtype=np.int64) + va
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
ctx = int(sys.argv[2]) if sys.argv[2].isdigit() else 0
for a in sys.argv[3:]:
    tgt = int(a, 16)
    hits = np.nonzero((t[0:rs-4] == 0xE8) & (pos + 5 + d == tgt))[0]
    print("== callers of 0x%X: %d" % (tgt, len(hits)))
    for h in hits[:6]:
        r = int(pos[h]); o = rp + (r - va)
        lines = []
        for i, ins in enumerate(md.disasm(m[o-24:o+60], r-24)):
            if ins.address >= r - 20: lines.append("   0x%X: %s %s" % (ins.address, ins.mnemonic, ins.op_str))
            if len(lines) > ctx: break
        print("\n".join(lines))
