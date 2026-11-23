# Which incoming scalar registers (user SGPRs) do captured main-renderer shaders read before writing them?
# usage: usersgpr_survey.py <spec.xml> <shaders dir> [sample]
# Prints, per stage, how often each s0..s31 is an input, and the most common input sets.
import sys, os, struct, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rdna2dis, rdna2hlsl

def fields(spec, code, pc):
    """(reads, writes) of scalar registers for one instruction, from the operand fields (approximate but enough here)."""
    d0 = struct.unpack_from("<I", code, pc)[0]; d1 = struct.unpack_from("<I", code, pc + 4)[0] if pc + 8 <= len(code) else 0
    nm, n, _ = rdna2dis.decode(spec, code, pc)
    R, W = set(), set()
    def sreg(x, cnt=1):
        return {x + i for i in range(cnt)} if x <= 105 else set()
    wide = 2 if nm and ("_B64" in nm or "_U64" in nm or "SAVEEXEC" in nm) else 1
    top9 = d0 >> 23
    if top9 == 0x17D:   R |= sreg(d0 & 0xFF, wide); W |= sreg((d0 >> 16) & 0x7F, wide)                       # SOP1
    elif top9 == 0x17E: R |= sreg(d0 & 0xFF, wide) | sreg((d0 >> 8) & 0xFF, wide)                             # SOPC
    elif top9 == 0x17F: pass                                                                                  # SOPP
    elif (d0 >> 28) == 0xB: R |= sreg((d0 >> 16) & 0x7F); W |= sreg((d0 >> 16) & 0x7F)                        # SOPK
    elif (d0 >> 30) == 2: R |= sreg(d0 & 0xFF, wide) | sreg((d0 >> 8) & 0xFF, wide); W |= sreg((d0 >> 16) & 0x7F, wide)   # SOP2
    elif (d0 >> 26) == 0x3D:                                                                                  # SMEM
        cnt = {"X2": 2, "X4": 4, "X8": 8, "16": 16}.get(nm[-2:], 1) if nm else 1
        R |= sreg((d0 & 0x3F) * 2, 2); W |= sreg((d0 >> 6) & 0x7F, cnt)
        if (d1 >> 25) != 125: R |= sreg(d1 >> 25)
    elif (d0 >> 26) in (0x38, 0x3A): R |= sreg(((d1 >> 16) & 0x1F) * 4, 4) | sreg(d1 >> 24)                  # MUBUF/MTBUF
    elif (d0 >> 26) == 0x3C: R |= sreg(((d1 >> 16) & 0x1F) * 4, 8) | sreg(((d1 >> 21) & 0x1F) * 4, 4)        # MIMG (T# + S#)
    elif (d0 >> 26) in (0x35, 0x33):                                                                          # VOP3/VOP3P
        for c in (d1 & 0x1FF, (d1 >> 9) & 0x1FF, (d1 >> 18) & 0x1FF): R |= sreg(c)
        if nm and nm.startswith("V_READLANE"): W |= sreg(d0 & 0xFF)
    elif (d0 >> 25) in (0x3F, 0x3E) or (d0 >> 31) == 0: R |= sreg(d0 & 0x1FF)                                 # VOP1/VOP2/VOPC
    return nm, n, R, W

spec = rdna2dis.load_spec(sys.argv[1]); src = sys.argv[2]; want = int(sys.argv[3]) if len(sys.argv) > 3 else 400
files = sorted(f for f in os.listdir(src) if f.endswith((".vsc", ".psc")))
per = {"vs": collections.Counter(), "ps": collections.Counter()}; sets = {"vs": collections.Counter(), "ps": collections.Counter()}; nst = collections.Counter()
for fn in files[::max(1, len(files) // want)]:
    raw = open(os.path.join(src, fn), "rb").read(); e = raw.find(b"\x00\x00\x81\xbf"); code = raw[:e + 4] if e >= 0 else raw
    st = rdna2hlsl.detect_stage(spec, code); nst[st] += 1
    written, inputs, pc = set(), set(), 0
    while pc + 4 <= len(code):
        nm, n, R, W = fields(spec, code, pc)
        inputs |= {r for r in R if r not in written and r < 32}
        written |= W; pc += n
        if nm == "S_ENDPGM": break
    per[st].update(inputs); sets[st][tuple(sorted(inputs))] += 1
for st in ("vs", "ps"):
    print("== %s (%d shaders): input s-register frequency" % (st, nst[st]))
    print("   " + "  ".join("s%d:%d" % (r, per[st][r]) for r in range(32) if per[st][r]))
    for k, v in sets[st].most_common(6): print("   %4d  %s" % (v, " ".join("s%d" % r for r in k)))
