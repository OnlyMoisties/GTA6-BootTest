# RDNA2 -> HLSL translator (first cut). One HLSL thread = one wave lane.
#   scalar registers -> uint s[] (uniform values, kept per thread), vector registers -> uint v[]
#   control flow     -> loop { switch (pc) { case ...: } } over basic-block entry points
#   memory           -> "memory window": ByteAddressBuffer g_mem holding a snapshot of game memory starting at g_membase
#   VS outputs       -> exp pos0 / paramN ; PS inputs v_interp_* ; PS output exp mrt0
# usage: rdna2hlsl.py <spec.xml> <code.bin> <vs|ps> <out.hlsl> [user_sgpr_count]
import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rdna2dis

class Unsupported(Exception): pass

def lit_of(code, pc, size):
    return struct.unpack_from("<I", code, pc + size)[0] if pc + size + 4 <= len(code) else 0

def ssrc(x, lit):
    if x <= 105: return "s[%d]" % x
    m = {106: "vcc", 107: "0u", 124: "m0", 125: "0u", 126: "1u", 127: "0u", 253: "scc", 251: "(vcc==0u?1u:0u)", 252: "0u"}
    if x in m: return m[x]
    if 128 <= x <= 192: return "%du" % (x - 128)
    if 193 <= x <= 208: return "asuint(%d)" % -(x - 192)
    fl = {240: 0.5, 241: -0.5, 242: 1.0, 243: -1.0, 244: 2.0, 245: -2.0, 246: 4.0, 247: -4.0}
    if x in fl: return "asuint(%sf)" % repr(fl[x])
    if x == 248: return "0x3E22F983u"
    if x == 255: return "0x%08Xu" % lit
    raise Unsupported("source operand %d" % x)

def vsrc(x, lit): return "v[%d]" % (x - 256) if x >= 256 else ssrc(x, lit)
def sdst(x):
    if x <= 105: return "s[%d]" % x
    return {106: "vcc", 124: "m0", 126: "exec_dummy", 107: "vcc_hi", 127: "exec_hi"}.get(x, "sink")
F = lambda e: "asfloat(%s)" % e

def translate(spec, code, stage, nuser):
    # pass 1: decode, collect branch targets
    insts, pc = [], 0
    while pc < len(code):
        nm, n, _ = rdna2dis.decode(spec, code, pc)
        if nm is None: raise Unsupported("undecodable at 0x%X" % pc)
        d0 = struct.unpack_from("<I", code, pc)[0]; d1 = struct.unpack_from("<I", code, pc + 4)[0] if pc + 8 <= len(code) else 0
        insts.append((pc, nm, n, d0, d1))
        pc += n
        if nm == "S_ENDPGM": break
    targets = {0}
    for pc, nm, n, d0, d1 in insts:
        if nm.startswith("S_BRANCH") or nm.startswith("S_CBRANCH"):
            targets.add(pc + 4 + 4 * struct.unpack("<h", struct.pack("<H", d0 & 0xFFFF))[0]); targets.add(pc + n)
    out = []
    E = out.append
    maxv = 0
    for pc, nm, n, d0, d1 in insts:
        if pc in targets: E("    case 0x%X:" % pc)
        lit = lit_of(code, pc, 4 if n in (8,) and (d0 >> 26) not in (0x35, 0x3D, 0x38, 0x3A, 0x3C, 0x36, 0x37, 0x3E) and (d0 >> 30) != 3 else 8) if n > 4 else 0
        if n == 8 and (d0 >> 26) not in (0x35, 0x3D, 0x38, 0x3A, 0x3C, 0x36, 0x37) : lit = d1                     # 32-bit encoding + literal
        if n == 12: lit = lit_of(code, pc, 8)
        s = None
        # ---- scalar ----
        if (d0 >> 23) == 0x17F:                                              # SOPP
            imm = struct.unpack("<h", struct.pack("<H", d0 & 0xFFFF))[0]; tgt = pc + 4 + 4 * imm
            s = {"S_ENDPGM": "return;", "S_NOP": "", "S_WAITCNT": "", "S_CLAUSE": "", "S_INST_PREFETCH": "", "S_CODE_END": "",
                 "S_BRANCH": "pc = 0x%X; continue;" % tgt,
                 "S_CBRANCH_SCC0": "if (scc == 0u) { pc = 0x%X; continue; }" % tgt, "S_CBRANCH_SCC1": "if (scc != 0u) { pc = 0x%X; continue; }" % tgt,
                 "S_CBRANCH_VCCZ": "if (vcc == 0u) { pc = 0x%X; continue; }" % tgt, "S_CBRANCH_VCCNZ": "if (vcc != 0u) { pc = 0x%X; continue; }" % tgt}.get(nm)
            if s is None: raise Unsupported(nm)
        elif (d0 >> 23) == 0x17D:                                            # SOP1
            dst, a = (d0 >> 16) & 0x7F, ssrc(d0 & 0xFF, lit)
            s = {"S_MOV_B32": "%s = %s;" % (sdst(dst), a)}.get(nm)
            if s is None: raise Unsupported(nm)
        elif (d0 >> 23) == 0x17E:                                            # SOPC
            a, b = ssrc(d0 & 0xFF, lit), ssrc((d0 >> 8) & 0xFF, lit)
            ops = {"S_CMP_EQ_U32": "==", "S_CMP_LG_U32": "!=", "S_CMP_EQ_I32": "==", "S_CMP_LG_I32": "!=", "S_CMP_GT_U32": ">", "S_CMP_LT_U32": "<", "S_CMP_GE_U32": ">=", "S_CMP_LE_U32": "<="}
            if nm not in ops: raise Unsupported(nm)
            s = "scc = (%s %s %s) ? 1u : 0u;" % (a, ops[nm], b)
        elif (d0 >> 28) == 0xB:                                              # SOPK
            if nm == "S_MOVK_I32": s = "%s = asuint((int)%d);" % (sdst((d0 >> 16) & 0x7F), struct.unpack("<h", struct.pack("<H", d0 & 0xFFFF))[0])
            else: raise Unsupported(nm)
        elif (d0 >> 30) == 2:                                                # SOP2
            dst, a, b = (d0 >> 16) & 0x7F, ssrc(d0 & 0xFF, lit), ssrc((d0 >> 8) & 0xFF, lit)
            bin_ = {"S_AND_B32": "&", "S_OR_B32": "|", "S_XOR_B32": "^", "S_LSHR_B32": ">>", "S_LSHL_B32": "<<", "S_ADD_U32": "+", "S_SUB_U32": "-", "S_MUL_I32": "*"}
            if nm in bin_: s = "%s = %s %s %s; scc = (%s != 0u) ? 1u : 0u;" % (sdst(dst), a, bin_[nm], ("(%s & 31u)" % b) if nm in ("S_LSHR_B32", "S_LSHL_B32") else b, sdst(dst))
            elif nm == "S_CSELECT_B32": s = "%s = scc != 0u ? %s : %s;" % (sdst(dst), a, b)
            else: raise Unsupported(nm)
        elif (d0 >> 26) == 0x3D:                                             # SMEM
            sdata, sbase = (d0 >> 6) & 0x7F, (d0 & 0x3F) * 2
            off = struct.unpack("<i", struct.pack("<I", (d1 & 0x1FFFFF) << 11))[0] >> 11; soff = d1 >> 25
            cnt = {"S_LOAD_DWORD": 1, "S_LOAD_DWORDX2": 2, "S_LOAD_DWORDX4": 4, "S_LOAD_DWORDX8": 8, "S_LOAD_DWORDX16": 16,
                   "S_BUFFER_LOAD_DWORD": 1, "S_BUFFER_LOAD_DWORDX2": 2, "S_BUFFER_LOAD_DWORDX4": 4, "S_BUFFER_LOAD_DWORDX8": 8, "S_BUFFER_LOAD_DWORDX16": 16}.get(nm)
            if not cnt: raise Unsupported(nm)
            addr = "memaddr(s[%d], s[%d] & 0xFFFFu) + %d%s" % (sbase, sbase + 1, off, "" if soff == 125 else " + %s" % ssrc(soff, lit))
            s = "{ uint a_ = %s; %s }" % (addr, " ".join("s[%d] = memload(a_ + %d);" % (sdata + i, 4 * i) for i in range(cnt)))
        # ---- vector ----
        elif (d0 >> 25) == 0x3F or (d0 >> 31) == 0:                         # VOP1 / VOP2
            c0 = d0 & 0x1FF; dst = (d0 >> 17) & 0xFF; vs1 = "v[%d]" % ((d0 >> 9) & 0xFF); a = vsrc(c0, lit); maxv = max(maxv, dst + 1)
            m = {"V_MOV_B32": "v[%d] = %s;" % (dst, a), "V_NOP": "",
                 "V_ADD_NC_U32": "v[%d] = %s + %s;" % (dst, a, vs1), "V_SUB_NC_U32": "v[%d] = %s - %s;" % (dst, a, vs1),
                 "V_AND_B32": "v[%d] = %s & %s;" % (dst, a, vs1), "V_OR_B32": "v[%d] = %s | %s;" % (dst, a, vs1),
                 "V_MUL_F32": "v[%d] = asuint(%s * %s);" % (dst, F(a), F(vs1)), "V_ADD_F32": "v[%d] = asuint(%s + %s);" % (dst, F(a), F(vs1)),
                 "V_SUB_F32": "v[%d] = asuint(%s - %s);" % (dst, F(a), F(vs1)), "V_CVT_F32_U32": "v[%d] = asuint((float)%s);" % (dst, a),
                 "V_CVT_PKRTZ_F16_F32": "v[%d] = f32tof16(%s) | (f32tof16(%s) << 16);" % (dst, F(a), F(vs1))}
            if (d0 >> 25) == 0x3F: nm2 = nm
            else: nm2 = nm
            if nm2 not in m: raise Unsupported(nm)
            s = m[nm2]
        elif (d0 >> 26) == 0x35:                                             # VOP3
            dst = d0 & 0xFF; c0, c1, c2 = d1 & 0x1FF, (d1 >> 9) & 0x1FF, (d1 >> 18) & 0x1FF; maxv = max(maxv, dst + 1)
            a, b, c = vsrc(c0, lit), vsrc(c1, lit), vsrc(c2, lit)
            m = {"V_NOP": "", "V_MUL_LO_U32": "v[%d] = %s * %s;" % (dst, a, b), "V_ADD_NC_U32": "v[%d] = %s + %s;" % (dst, a, b),
                 "V_CVT_PKRTZ_F16_F32": "v[%d] = f32tof16(%s) | (f32tof16(%s) << 16);" % (dst, F(a), F(b)),
                 "V_FMA_F32": "v[%d] = asuint(mad(%s, %s, %s));" % (dst, F(a), F(b), F(c))}
            if nm not in m: raise Unsupported(nm)
            s = m[nm]
        elif (d0 >> 26) == 0x38:                                             # MUBUF
            op_off = d0 & 0xFFF; offen = (d0 >> 12) & 1; vaddr, vdata, srsrc, soff = d1 & 0xFF, (d1 >> 8) & 0xFF, ((d1 >> 16) & 0x1F) * 4, d1 >> 24
            cnt = {"BUFFER_LOAD_DWORD": 1, "BUFFER_LOAD_DWORDX2": 2, "BUFFER_LOAD_DWORDX3": 3, "BUFFER_LOAD_DWORDX4": 4}.get(nm)
            if not cnt: raise Unsupported(nm)
            maxv = max(maxv, vdata + cnt)
            addr = "memaddr(s[%d], s[%d] & 0xFFFFu) + %d + %s%s" % (srsrc, srsrc + 1, op_off, "v[%d]" % vaddr if offen else "0u", "" if soff in (125, 128) else " + %s" % ssrc(soff, lit))
            s = "{ uint a_ = %s; %s }" % (addr, " ".join("v[%d] = memload(a_ + %d);" % (vdata + i, 4 * i) for i in range(cnt)))
        elif (d0 >> 26) == 0x32 and stage == "ps":                           # VINTRP
            dst = (d0 >> 18) & 0xFF; op = (d0 >> 16) & 3; attr = (d0 >> 10) & 0x3F; chan = (d0 >> 8) & 3; maxv = max(maxv, dst + 1)
            s = "v[%d] = asuint(inp.attr%d[%d]);" % (dst, attr, chan)
        elif (d0 >> 26) == 0x3E:                                             # EXP
            en = d0 & 0xF; tgt = (d0 >> 4) & 0x3F; compr = (d0 >> 10) & 1
            vv = [(d1 >> (8 * i)) & 0xFF for i in range(4)]
            if stage == "vs" and tgt == 12: s = "o.pos = float4(%s);" % ", ".join("asfloat(v[%d])" % vv[i] if en >> i & 1 else ("1.0" if i == 3 else "0.0") for i in range(4))
            elif stage == "vs" and 32 <= tgt < 64: s = "o.attr%d = float4(%s);" % (tgt - 32, ", ".join("asfloat(v[%d])" % vv[i] if en >> i & 1 else "0.0" for i in range(4)))
            elif stage == "ps" and tgt == 0 and compr: s = "o.c0 = float4(f16tof32(v[%d]), f16tof32(v[%d] >> 16), f16tof32(v[%d]), f16tof32(v[%d] >> 16));" % (vv[0], vv[0], vv[1], vv[1])
            elif stage == "ps" and tgt == 0: s = "o.c0 = float4(%s);" % ", ".join("asfloat(v[%d])" % vv[i] for i in range(4))
            else: raise Unsupported("exp target %d" % tgt)
        else:
            raise Unsupported(nm)
        if s: E("        " + s)
    body = "\n".join(out)
    nv = max(maxv, 8)
    hdr = r"""// generated by tools/rdna2hlsl.py from RDNA2 machine code - do not edit
cbuffer Regs : register(b0, space0) { uint4 g_user[4]; uint2 g_membase; uint2 g_pad; };   // user SGPRs, memory-window base address
ByteAddressBuffer g_mem : register(t0, space0);                                          // snapshot of game memory at g_membase
uint memaddr(uint lo, uint hi) { return lo - g_membase.x; }                                // address -> offset into the window (low 32 bits)
uint memload(uint off) { return g_mem.Load(off & ~3u); }
"""
    if stage == "vs":
        io = "struct VSOut { float4 pos : SV_Position; nointerpolation float4 attr0 : TEXCOORD0; };\n"
        entry = "VSOut main(uint vid : SV_VertexID) {\n    VSOut o = (VSOut)0; uint s[106]; uint v[%d]; uint scc = 0u, vcc = 0u, m0 = 0u, sink = 0u, vcc_hi = 0u, exec_dummy = 0u, exec_hi = 0u;\n" % nv
        entry += "    [unroll] for (int i = 0; i < 106; i++) s[i] = 0u; [unroll] for (int j = 0; j < %d; j++) v[j] = 0u;\n" % nv
        entry += "    [unroll] for (int u = 0; u < %d; u++) s[u] = g_user[u >> 2][u & 3];\n    v[0] = vid;\n" % min(nuser, 16)
    else:
        io = "struct PSIn { float4 pos : SV_Position; nointerpolation float4 attr0 : TEXCOORD0; };\nstruct PSOut { float4 c0 : SV_Target0; };\n"
        entry = "PSOut main(PSIn inp) {\n    PSOut o = (PSOut)0; uint s[106]; uint v[%d]; uint scc = 0u, vcc = 0u, m0 = 0u, sink = 0u, vcc_hi = 0u, exec_dummy = 0u, exec_hi = 0u;\n" % nv
        entry += "    [unroll] for (int i = 0; i < 106; i++) s[i] = 0u; [unroll] for (int j = 0; j < %d; j++) v[j] = 0u;\n" % nv
        entry += "    [unroll] for (int u = 0; u < %d; u++) s[u] = g_user[u >> 2][u & 3];\n" % min(nuser, 16)
    loop = "    uint pc = 0u;\n    [loop] for (uint guard = 0u; guard < 100000u; guard++) {\n    switch (pc) {\n" + body + "\n        return o;\n    default: return o;\n    }\n    }\n    return o;\n}\n"
    # 'return;' inside VS/PS must return o
    loop = loop.replace("return;", "return o;")
    return hdr + io + entry + loop

if __name__ == "__main__":
    spec = rdna2dis.load_spec(sys.argv[1])
    code = open(sys.argv[2], "rb").read()
    try:
        h = translate(spec, code, sys.argv[3], int(sys.argv[5]) if len(sys.argv) > 5 else 16)
    except Unsupported as e:
        print("UNSUPPORTED:", e); sys.exit(2)
    open(sys.argv[4], "w", newline="\n").write(h)
    print("wrote", sys.argv[4])
