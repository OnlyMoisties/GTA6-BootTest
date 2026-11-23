# find rip-relative references to target RVAs (disp32 ending the instruction), print containing instruction
import sys, struct, mmap
import numpy as np, capstone
f = open(sys.argv[1], 'rb'); m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
pe = struct.unpack_from('<I', m, 0x3C)[0]; nsec, = struct.unpack_from('<H', m, pe+6); optsz, = struct.unpack_from('<H', m, pe+20)
so = pe+24+optsz; secs = []
for i in range(nsec):
    vs, va, rs, rp = struct.unpack_from('<IIII', m, so+8); secs.append((va, vs, rp, rs)); so += 40
va, vs, rp, rs = secs[0]
text = np.frombuffer(m, dtype=np.uint8, count=rs, offset=rp)
n = rs - 4
d = (text[0:n].astype(np.int64) | (text[1:n+1].astype(np.int64) << 8) | (text[2:n+2].astype(np.int64) << 16) | (text[3:n+3].astype(np.int64) << 24))
d = np.where(d >= 2**31, d - 2**32, d)
pos = np.arange(n, dtype=np.int64) + va
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for t in sys.argv[2:]:
    tgt = int(t, 16)
    for extra in (0, 1, 4):   # instruction may have imm8/imm32 after disp
        hits = np.nonzero(pos + 4 + extra + d == tgt)[0]
        for h in hits[:60]:
            r = int(pos[h])
            for back in range(2, 10):
                st = r - back; o = rp + (st - va)
                ins = next(md.disasm(m[o:o+16], st), None)
                if ins and ins.address + ins.size == r + 4 + extra and 'rip' in ins.op_str:
                    print("0x%X -> 0x%X: %s %s" % (ins.address, tgt, ins.mnemonic, ins.op_str)); break
