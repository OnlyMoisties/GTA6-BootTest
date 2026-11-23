import re, sys
log = open(sys.argv[1], encoding='latin1').read().splitlines()
target = int(sys.argv[3], 16) if len(sys.argv) > 3 else int([l for l in open(sys.argv[2], encoding="latin1") if "access violation reading" in l][-1].split("0x")[-1], 16)
print("fault address 0x%X" % target)
pat = re.compile(r'\[xmem\] (XMem\w+)(\[[^\]]*\])?\((0x[0-9A-F]+), (0x[0-9A-F]+), (0x[0-9A-F]+), (0x[0-9A-F]+), (0x[0-9A-F]+)\) -> (0x[0-9A-F]+) caller=(\S+)')
n = 0
for l in log:
    m = pat.search(l)
    if not m: continue
    fn, tag = m.group(1), m.group(2) or ''
    a = [int(m.group(i), 16) for i in range(3, 9)]
    if fn == 'XMemVirtualAlloc':
        base, size = a[5], a[1]; hit = base <= target < base + size
        n += 1
        if hit or n <= 40: print("%s%s addr=0x%X size=0x%X type=0x%X a4=0x%X prot=0x%X -> 0x%X %s %s" % (fn, tag, a[0], a[1], a[2], a[3], a[4], a[5], m.group(9), "<== CONTAINS FAULT" if hit else ""))
for l in log:
    if 'XMemMapPhysicalPages RAW' in l:
        m = re.search(r'a=(0x[0-9A-F]+)\{[^}]*\} b=(0x[0-9A-F]+)', l)
        base, cnt = int(m.group(1), 16), int(m.group(2), 16)
        if base <= target < base + (cnt << 16): print("MAP contains fault:", l[:160])
