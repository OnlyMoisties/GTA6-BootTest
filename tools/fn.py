# usage: fn.py <exe> <rva hex> [grep regex] -> prints the function containing rva (via .pdata), optionally only matching lines
import sys, struct, mmap, re
import capstone
f = open(sys.argv[1], 'rb'); m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
pe = struct.unpack_from('<I', m, 0x3C)[0]; nsec, = struct.unpack_from('<H', m, pe+6); optsz, = struct.unpack_from('<H', m, pe+20)
opt = pe+24; so = opt+optsz; secs = []
for i in range(nsec):
    vs, va, rs, rp = struct.unpack_from('<IIII', m, so+8); secs.append((va, vs, rp, rs)); so += 40
def off(rva):
    for va, vs, rp, rs in secs:
        if va <= rva < va+rs: return rp+(rva-va)
pr, ps = struct.unpack_from('<II', m, opt+112+3*8); po = off(pr); n = ps//12
target = int(sys.argv[2], 16); lo, hi = 0, n-1; beg = end = None
while lo <= hi:
    mid = (lo+hi)//2; b, e, u = struct.unpack_from('<III', m, po+mid*12)
    if target < b: hi = mid-1
    elif target >= e: lo = mid+1
    else: beg, end = b, e; break
if beg is None: print("no pdata entry"); sys.exit()
print("function 0x%X-0x%X (%d bytes)" % (beg, end, end-beg))
pat = re.compile(sys.argv[3]) if len(sys.argv) > 3 else None
limit = int(sys.argv[4], 16) if len(sys.argv) > 4 else end
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for i in md.disasm(m[off(beg):off(beg)+(end-beg)], beg):
    if i.address > limit: break
    s = "0x%X: %s %s" % (i.address, i.mnemonic, i.op_str)
    if 'rip' in i.op_str:
        mm = re.search(r'rip ([+-]) (0x[0-9a-f]+)', i.op_str)
        if mm: s += "   ; -> 0x%X" % (i.address + i.size + (int(mm.group(2), 16) if mm.group(1) == '+' else -int(mm.group(2), 16)))
    if not pat or pat.search(s): print(s)
