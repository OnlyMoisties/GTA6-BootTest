# Correlate graphics-pipeline stage metadata (logs/pso57/*.txt, written with GTA6_PSO57DUMP=1) with the
# scalar registers each stage's shader reads before writing them.
# usage: pso57_meta.py <spec.xml> <logs dir>
import sys, os, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rdna2dis
from usersgpr_survey import fields

spec = rdna2dis.load_spec(sys.argv[1]); logs = sys.argv[2]
def inputs(path):
    raw = open(path, "rb").read(); e = raw.find(b"\x00\x00\x81\xbf"); code = raw[:e + 4] if e >= 0 else raw
    written, ins, pc = set(), set(), 0
    while pc + 4 <= len(code):
        nm, n, R, W = fields(spec, code, pc)
        ins |= {r for r in R if r not in written and r < 32}; written |= W; pc += n
        if nm == "S_ENDPGM": break
    return sorted(ins)
for fn in sorted(os.listdir(os.path.join(logs, "pso57"))):
    txt = open(os.path.join(logs, "pso57", fn)).read()
    blocks = {}
    for m in re.finditer(r"block at \+0x([0-9A-F]+) -> [0-9A-F]+ \((\d+) bytes\)([^\n]*)\n((?:   \+[0-9A-F]+:[^\n]*\n)*)", txt):
        off = int(m.group(1), 16); words = [int(w, 16) for line in m.group(4).splitlines() for w in line.split(":", 1)[1].split()]
        h = re.search(r"hash ([0-9A-F]{16})\.(vsc|psc)", m.group(3))
        blocks[off] = (words, h.group(1) + "." + h.group(2) if h else None)
    print("==", fn)
    for code_off, meta_off, st in ((0x48, 0x58, "VS"), (0x28, 0x38, "PS")):
        if code_off not in blocks: continue
        name = blocks[code_off][1]; p = os.path.join(logs, "shaders", name) if name else None
        ins = inputs(p) if p and os.path.exists(p) else "?"
        meta = blocks.get(meta_off, ([], None))[0]
        print("  %s %s reads %s" % (st, name, " ".join("s%d" % r for r in ins) if ins != "?" else "?"))
        print("     meta: %s" % " ".join("%X" % w for w in meta))
