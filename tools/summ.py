import re, sys, collections
log = open(sys.argv[1], encoding='latin1').read().splitlines()
c = collections.Counter(); first = {}; order = []
pat = re.compile(r'obj#(-?\d+)\(from obj#(-?\d+)\.slot(-?\d+) iid=([0-9A-F]+)\)\.slot(\d+)(\[sret\])?.*?(=> obj#\d+ \S+ iid=\{([0-9A-F]+))?[^=]*caller=exe\+0x([0-9A-F]+) tid=(\d+)')
for l in log:
    m = pat.search(l)
    if not m: continue
    k = (m.group(4), int(m.group(5)), m.group(8) or '')
    if k not in first: first[k] = (m.group(9), m.group(10)); order.append(k)
    c[k] += 1
print("logged d3d lines:", sum(c.values()), " distinct (iid,slot):", len(c))
for k in order:
    print("iid=%s slot%-3d x%-5d makes=%-8s firstcaller=exe+0x%s tid=%s" % (k[0], k[1], c[k], k[2], first[k][0], first[k][1]))
other = [l for l in log if '[d3d12_xs]' not in l and '[fs]' not in l and 'OpenFile' not in l and 'XMemVirtualAlloc' not in l]
print("--- non-d3d tail ---")
for l in other[-12:]: print(l[:260])
