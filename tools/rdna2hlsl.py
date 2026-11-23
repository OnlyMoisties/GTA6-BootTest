# RDNA2 -> HLSL translator, version 2. One HLSL thread = one wave lane; masks are 64-bit and built with wave intrinsics.
#   scalar registers   -> uint s[] (wave-uniform values, held per thread)
#   vector registers   -> uint v[]
#   exec / vcc         -> uint64_t; a lane is active when (exec >> lane) & 1; VALU writes are masked by that
#   compares           -> vcc / sdst = WaveActiveBallot(active && cond)
#   control flow       -> loop { switch (pc) { case <block>: ... } }
#   memory             -> 48-bit game addresses resolved through the GPU memory pool page table (see src/gpumem.h):
#                         t0 = page table, sections = ResourceDescriptorHeap[0..] (zero copy, live game memory)
#   VS outputs         -> exp pos0 / paramN ; PS inputs v_interp_* ; PS outputs exp mrtN
#   images             -> placeholder (returns 0) until the bindless texture layer exists
# usage: rdna2hlsl.py <spec.xml> <code.bin> <vs|ps> <out.hlsl> [user_sgpr_count]
import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rdna2dis

class Unsupported(Exception): pass

NATTR = 16      # VS->PS attributes travel as raw bits per vertex; the PS interpolates them itself, like the hardware

def sdsel(e, sel, sext):
    if sel == 6: return e
    if sel < 4: return ("asuint((int)(%s << %du) >> 24)" % (e, 24 - 8 * sel)) if sext else ("((%s >> %du) & 255u)" % (e, 8 * sel))
    w = sel - 4
    return ("asuint((int)(%s << %du) >> 16)" % (e, 16 - 16 * w)) if sext else ("((%s >> %du) & 0xFFFFu)" % (e, 16 * w))

def sx(v, bits): return v - (1 << bits) if v & (1 << (bits - 1)) else v

def ssrc(x, lit):
    if x <= 105 or 108 <= x <= 123: return "s[%d]" % x
    m = {106: "VCCLO", 107: "VCCHI", 124: "m0", 125: "0u", 126: "EXECLO", 127: "EXECHI", 253: "scc", 251: "(vcc == 0 ? 1u : 0u)", 252: "(exec == 0 ? 1u : 0u)",
         235: "0u", 236: "0u", 237: "0u", 238: "0u", 239: "0u"}
    if x in m: return m[x]
    if 128 <= x <= 192: return "%du" % (x - 128)
    if 193 <= x <= 208: return "0x%08Xu" % ((-(x - 192)) & 0xFFFFFFFF)
    fl = {240: 0x3F000000, 241: 0xBF000000, 242: 0x3F800000, 243: 0xBF800000, 244: 0x40000000, 245: 0xC0000000, 246: 0x40800000, 247: 0xC0800000, 248: 0x3E22F983}
    if x in fl: return "0x%08Xu" % fl[x]
    if x == 255: return "0x%08Xu" % lit
    raise Unsupported("source operand %d" % x)

def ssrc64(x, lit):
    """64-bit scalar source as uint64_t expression."""
    if x <= 104 or 108 <= x <= 122: return "((uint64_t)s[%d] | ((uint64_t)s[%d] << 32))" % (x, x + 1)
    if x in (105, 107, 127): return "((uint64_t)%s | ((uint64_t)%s << 32))" % (ssrc(x, lit), ssrc(x + 1, lit) if x != 127 else "0u")
    if x == 106: return "vcc"
    if x == 126: return "exec"
    if x == 125: return "0ull"
    if 128 <= x <= 192: return "%dull" % (x - 128)
    if 193 <= x <= 208: return "0x%016Xull" % ((-(x - 192)) & 0xFFFFFFFFFFFFFFFF)
    if x == 255: return "%dull" % lit
    if x == 253: return "(uint64_t)scc"
    raise Unsupported("64-bit source operand %d" % x)

def set_s(x, e):
    if x <= 105 or 108 <= x <= 123: return "s[%d] = %s;" % (x, e)
    if x == 106: return "vcc = (vcc & 0xFFFFFFFF00000000ull) | (uint64_t)(%s);" % e
    if x == 107: return "vcc = (vcc & 0xFFFFFFFFull) | ((uint64_t)(%s) << 32);" % e
    if x == 126: return "exec = (exec & 0xFFFFFFFF00000000ull) | (uint64_t)(%s);" % e
    if x == 127: return "exec = (exec & 0xFFFFFFFFull) | ((uint64_t)(%s) << 32);" % e
    if x == 124: return "m0 = %s;" % e
    if x == 125: return ""
    raise Unsupported("scalar destination %d" % x)

def set_s64(x, e):
    if x <= 104 or 108 <= x <= 122: return "{ uint64_t t_ = %s; s[%d] = (uint)t_; s[%d] = (uint)(t_ >> 32); }" % (e, x, x + 1)
    if x in (105, 107): return "{ uint64_t t_ = %s; %s %s }" % (e, set_s(x, "(uint)t_"), set_s(x + 1, "(uint)(t_ >> 32)"))
    if x == 106: return "vcc = %s;" % e
    if x == 126: return "exec = %s;" % e
    if x == 125: return ""
    raise Unsupported("64-bit scalar destination %d" % x)

def vsrc(x, lit): return "v[%d]" % (x - 256) if x >= 256 else ssrc(x, lit)
def F(e): return "asfloat(%s)" % e
U = lambda e: "asuint(%s)" % e

# VALU per-lane operations: name -> (number of sources, HLSL expression over a,b,c giving the uint result)
VOPS = {
    "V_MOV_B32": "a", "V_NOT_B32": "~a",
    "V_ADD_F32": "asuint(fa + fb)", "V_SUB_F32": "asuint(fa - fb)", "V_SUBREV_F32": "asuint(fb - fa)", "V_MUL_F32": "asuint(fa * fb)",
    "V_MUL_LEGACY_F32": "asuint((fa == 0 || fb == 0) ? 0.0f : fa * fb)",
    "V_MIN_F32": "asuint(min(fa, fb))", "V_MAX_F32": "asuint(max(fa, fb))",
    "V_FMAC_F32": "asuint(mad(fa, fb, asfloat(d)))", "V_FMA_F32": "asuint(mad(fa, fb, fc))", "V_MAD_F32": "asuint(mad(fa, fb, fc))",
    "V_FMAMK_F32": "asuint(mad(fa, asfloat(L), fb))", "V_FMAAK_F32": "asuint(mad(fa, fb, asfloat(L)))",
    "V_MED3_F32": "asuint(max(min(fa, fb), min(max(fa, fb), fc)))", "V_MIN3_F32": "asuint(min(fa, min(fb, fc)))", "V_MAX3_F32": "asuint(max(fa, max(fb, fc)))",
    "V_RCP_F32": "asuint(rcp(fa))", "V_RCP_IFLAG_F32": "asuint(rcp(fa))", "V_RSQ_F32": "asuint(rsqrt(fa))", "V_SQRT_F32": "asuint(sqrt(fa))",
    "V_EXP_F32": "asuint(exp2(fa))", "V_LOG_F32": "asuint(log2(fa))", "V_FRACT_F32": "asuint(frac(fa))", "V_FLOOR_F32": "asuint(floor(fa))",
    "V_CEIL_F32": "asuint(ceil(fa))", "V_TRUNC_F32": "asuint(trunc(fa))", "V_RNDNE_F32": "asuint(round(fa))",
    "V_SIN_F32": "asuint(sin(fa * 6.28318530718f))", "V_COS_F32": "asuint(cos(fa * 6.28318530718f))",
    "V_CVT_F32_U32": "asuint((float)a)", "V_CVT_F32_I32": "asuint((float)(int)a)", "V_CVT_U32_F32": "(uint)max(fa, 0.0f)", "V_CVT_I32_F32": "asuint((int)fa)",
    "V_CVT_F32_UBYTE0": "asuint((float)(a & 255u))", "V_CVT_F32_UBYTE1": "asuint((float)((a >> 8) & 255u))",
    "V_CVT_F32_UBYTE2": "asuint((float)((a >> 16) & 255u))", "V_CVT_F32_UBYTE3": "asuint((float)(a >> 24))",
    "V_CVT_F16_F32": "f32tof16(fa)", "V_CVT_F32_F16": "asuint(f16tof32(a))", "V_CVT_PKRTZ_F16_F32": "(f32tof16(fa) | (f32tof16(fb) << 16))",
    "V_CVT_FLR_I32_F32": "asuint((int)floor(fa))", "V_CVT_RPI_I32_F32": "asuint((int)floor(fa + 0.5f))",
    "V_ADD_NC_U32": "a + b", "V_SUB_NC_U32": "a - b", "V_SUBREV_NC_U32": "b - a", "V_ADD3_U32": "a + b + c",
    "V_AND_B32": "a & b", "V_OR_B32": "a | b", "V_XOR_B32": "a ^ b", "V_XNOR_B32": "~(a ^ b)", "V_OR3_B32": "a | b | c", "V_AND_OR_B32": "(a & b) | c",
    "V_LSHLREV_B32": "b << (a & 31u)", "V_LSHRREV_B32": "b >> (a & 31u)", "V_ASHRREV_I32": "asuint((int)b >> (int)(a & 31u))",
    "V_LSHL_ADD_U32": "(a << (b & 31u)) + c", "V_ADD_LSHL_U32": "(a + b) << (c & 31u)", "V_LSHL_OR_B32": "(a << (b & 31u)) | c",
    "V_MUL_LO_U32": "a * b", "V_MUL_HI_U32": "(uint)(((uint64_t)a * (uint64_t)b) >> 32)",
    "V_MUL_U32_U24": "(a & 0xFFFFFFu) * (b & 0xFFFFFFu)", "V_MUL_I32_I24": "asuint(((int)(a << 8) >> 8) * ((int)(b << 8) >> 8))",
    "V_MAD_U32_U24": "(a & 0xFFFFFFu) * (b & 0xFFFFFFu) + c", "V_MAD_I32_I24": "asuint(((int)(a << 8) >> 8) * ((int)(b << 8) >> 8) + (int)c)",
    "V_MIN_U32": "min(a, b)", "V_MAX_U32": "max(a, b)", "V_MIN_I32": "asuint(min((int)a, (int)b))", "V_MAX_I32": "asuint(max((int)a, (int)b))",
    "V_BFE_U32": "((c & 31u) == 0u ? 0u : (a >> (b & 31u)) & ((1u << (c & 31u)) - 1u))",
    "V_BFE_I32": "((c & 31u) == 0u ? 0u : asuint(((int)(a << (32u - (b & 31u) - (c & 31u)))) >> (32u - (c & 31u))))",
    "V_BFI_B32": "(a & b) | (~a & c)", "V_BFM_B32": "(((1u << (a & 31u)) - 1u) << (b & 31u))", "V_BFREV_B32": "reversebits(a)",
    "V_FFBH_U32": "(a == 0 ? 0xFFFFFFFFu : 31u - firstbithigh(a))", "V_FFBL_B32": "(a == 0 ? 0xFFFFFFFFu : firstbitlow(a))", "V_BCNT_U32_B32": "countbits(a) + b",
    "V_PERM_B32": "perm(a, b, c)", "V_ALIGNBIT_B32": "(uint)((((uint64_t)a << 32) | b) >> (c & 31u))", "V_ALIGNBYTE_B32": "(uint)((((uint64_t)a << 32) | b) >> (8u * (c & 3u)))",
    "V_LDEXP_F32": "asuint(ldexp(fa, (float)(int)b))", "V_FREXP_EXP_I32_F32": "asuint(fa == 0 ? 0 : (int)(asuint(fa) >> 23 & 255u) - 126)",
    "V_FREXP_MANT_F32": "asuint(fa == 0 ? 0.0f : asfloat((asuint(fa) & 0x807FFFFFu) | 0x3F000000u))",
    "V_PK_MUL_F16": "pk(f16tof32(a) * f16tof32(b), f16tof32(a >> 16) * f16tof32(b >> 16))",
    "V_PK_ADD_F16": "pk(f16tof32(a) + f16tof32(b), f16tof32(a >> 16) + f16tof32(b >> 16))",
    "V_PK_FMA_F16": "pk(mad(f16tof32(a), f16tof32(b), f16tof32(c)), mad(f16tof32(a >> 16), f16tof32(b >> 16), f16tof32(c >> 16)))",
    "V_FMA_MIX_F32": "asuint(mad(mixa, mixb, mixc))", "V_FMA_MIXLO_F16": "((d & 0xFFFF0000u) | f32tof16(mad(mixa, mixb, mixc)))",
    "V_FMA_MIXHI_F16": "((d & 0xFFFFu) | (f32tof16(mad(mixa, mixb, mixc)) << 16))",
    "V_SAD_U32": "(uint)abs((int)a - (int)b) + c", "V_MAX_F16": "f32tof16(max(f16tof32(a), f16tof32(b)))", "V_MIN_F16": "f32tof16(min(f16tof32(a), f16tof32(b)))",
    "V_MUL_F16": "f32tof16(f16tof32(a) * f16tof32(b))", "V_ADD_F16": "f32tof16(f16tof32(a) + f16tof32(b))", "V_FMA_F16": "f32tof16(mad(f16tof32(a), f16tof32(b), f16tof32(c)))",
    "V_CVT_PKNORM_U16_F32": "(cvtu16(fa) | (cvtu16(fb) << 16))", "V_CVT_PK_U8_F32": "(c & ~(255u << (8u * (b & 3u)))) | ((uint)clamp(round(fa), 0.0f, 255.0f) << (8u * (b & 3u)))",
    "V_SAT_PK_U8_I16": "satpk(a)", "V_CUBEMA_F32": "asuint(cubema(fa, fb, fc))", "V_CUBESC_F32": "asuint(cubesc(fa, fb, fc))",
    "V_CUBETC_F32": "asuint(cubetc(fa, fb, fc))", "V_CUBEID_F32": "asuint(cubeid(fa, fb, fc))",
    "V_MUL_HI_I32": "asuint((int)(((int64_t)(int)a * (int64_t)(int)b) >> 32))",
    "V_PK_MAX_F16": "pk(max(f16tof32(a), f16tof32(b)), max(f16tof32(a >> 16), f16tof32(b >> 16)))",
    "V_PK_MIN_F16": "pk(min(f16tof32(a), f16tof32(b)), min(f16tof32(a >> 16), f16tof32(b >> 16)))", "V_MED3_I32": "asuint(max(min((int)a, (int)b), min(max((int)a, (int)b), (int)c)))",
    "V_MED3_U32": "max(min(a, b), min(max(a, b), c))", "V_MAX3_U32": "max(a, max(b, c))", "V_MIN3_U32": "min(a, min(b, c))",
    "V_MAX3_I32": "asuint(max((int)a, max((int)b, (int)c)))", "V_MIN3_I32": "asuint(min((int)a, min((int)b, (int)c)))",
    "V_PACK_B32_F16": "((a & 0xFFFFu) | (b << 16))", "V_XOR3_B32": "a ^ b ^ c", "V_XAD_U32": "(a ^ b) + c",
    "V_SUB_F16": "f32tof16(f16tof32(a) - f16tof32(b))", "V_SUBREV_F16": "f32tof16(f16tof32(b) - f16tof32(a))",
    "V_FMAC_F16": "((d & 0xFFFF0000u) | f32tof16(mad(f16tof32(a), f16tof32(b), f16tof32(d))))",
    "V_MAD_U32_U16": "(a & 0xFFFFu) * (b & 0xFFFFu) + c", "V_CVT_PK_U16_U32": "(min(a, 0xFFFFu) | (min(b, 0xFFFFu) << 16))",
    "V_CVT_PK_I16_I32": "((uint)clamp((int)a, -32768, 32767) & 0xFFFFu) | ((uint)clamp((int)b, -32768, 32767) << 16)",
    "V_CVT_PKNORM_I16_F32": "(((uint)(int)round(clamp(fa, -1.0f, 1.0f) * 32767.0f) & 0xFFFFu) | ((uint)(int)round(clamp(fb, -1.0f, 1.0f) * 32767.0f) << 16))",
    "V_MAX_U16": "max(a & 0xFFFFu, b & 0xFFFFu)", "V_MIN_U16": "min(a & 0xFFFFu, b & 0xFFFFu)",
    "V_ADD_NC_U16": "(a + b) & 0xFFFFu", "V_SUB_NC_U16": "(a - b) & 0xFFFFu", "V_MUL_LO_U16": "(a * b) & 0xFFFFu",
    "V_LSHLREV_B16": "(b << (a & 15u)) & 0xFFFFu", "V_LSHRREV_B16": "(b & 0xFFFFu) >> (a & 15u)",
    "V_CVT_F16_U16": "f32tof16((float)(a & 0xFFFFu))", "V_CVT_U16_F16": "(uint)max(f16tof32(a), 0.0f)",
    "V_RCP_F16": "f32tof16(rcp(f16tof32(a)))", "V_SQRT_F16": "f32tof16(sqrt(f16tof32(a)))", "V_RSQ_F16": "f32tof16(rsqrt(f16tof32(a)))",
    "V_FRACT_F16": "f32tof16(frac(f16tof32(a)))", "V_EXP_F16": "f32tof16(exp2(f16tof32(a)))", "V_LOG_F16": "f32tof16(log2(f16tof32(a)))",
    "V_MAD_U16": "((a & 0xFFFFu) * (b & 0xFFFFu) + c) & 0xFFFFu", "V_MED3_F16": "f32tof16(max(min(f16tof32(a), f16tof32(b)), min(max(f16tof32(a), f16tof32(b)), f16tof32(c))))", "V_MOV_FED_B32": "a", "V_NOP": None,
}
CMPS = {"LT": "<", "EQ": "==", "LE": "<=", "GT": ">", "LG": "!=", "GE": ">=", "NEQ": "!=", "NE": "!=", "NLT": ">=", "NLE": ">", "NGT": "<=", "NGE": "<", "NLG": "=="}

def cmp_expr(nm):
    # V_CMP[X]_<op>_<type>
    p = nm.split("_"); typ = p[-1]; op = p[-2]
    if op in ("F", "TRU", "T"): return "false" if op == "F" else "true"
    if op == "O": return "(fa == fa && fb == fb)"
    if op == "U": return "(fa != fa || fb != fb)"
    if op == "CLASS": return "fclass(fa, b)"
    if op not in CMPS: raise Unsupported(nm)
    o = CMPS[op]
    if typ == "F32":
        if op in ("NLT", "NLE", "NGT", "NGE", "NLG", "NEQ"): return "!(fa %s fb)" % {"NLT": "<", "NLE": "<=", "NGT": ">", "NGE": ">=", "NLG": "!=", "NEQ": "=="}[op]
        return "(fa %s fb)" % o
    if typ == "F16": return "(f16tof32(a) %s f16tof32(b))" % o
    if typ in ("U32", "B32"): return "(a %s b)" % o
    if typ == "I32": return "((int)a %s (int)b)" % o
    if typ == "I16": return "(((int)(a << 16) >> 16) %s ((int)(b << 16) >> 16))" % o
    if typ == "U16": return "((a & 0xFFFFu) %s (b & 0xFFFFu))" % o
    if typ in ("U64", "I64"): return "(A64 %s B64)" % o
    raise Unsupported(nm)

def detect_stage(spec, code):
    """'vs' when the code exports a position, else 'ps' (the PSO's slot labels are not trusted)."""
    pc = 0
    while pc + 4 <= len(code):
        nm, n, _ = rdna2dis.decode(spec, code, pc)
        if nm == "EXP" and ((struct.unpack_from("<I", code, pc)[0] >> 4) & 0x3F) == 12: return "vs"
        if nm == "S_ENDPGM": break
        pc += n
    return "ps"

def translate(spec, code, stage, nuser):
    insts, pc = [], 0
    while pc + 4 <= len(code):
        nm, n, _ = rdna2dis.decode(spec, code, pc)
        if nm is None or nm.startswith("ENC_"): raise Unsupported("undecodable at 0x%X" % pc)
        d0 = struct.unpack_from("<I", code, pc)[0]; d1 = struct.unpack_from("<I", code, pc + 4)[0] if pc + 8 <= len(code) else 0
        d2 = struct.unpack_from("<I", code, pc + 8)[0] if pc + 12 <= len(code) else 0
        insts.append((pc, nm, n, d0, d1, d2)); pc += n
        if nm == "S_ENDPGM": break
    targets = {0}
    for pc, nm, n, d0, d1, d2 in insts:
        if nm == "S_BRANCH" or nm.startswith("S_CBRANCH"):
            targets.add(pc + 4 + 4 * sx(d0 & 0xFFFF, 16)); targets.add(pc + n)
    out = []; maxv = 8; attrs = set(); mrts = set(); params = set(); dpp_on = [None]
    E = out.append
    for pc, nm, n, d0, d1, d2 in insts:
        if pc in targets: E("    case 0x%X:" % pc)
        s = None; dpp_on[0] = None
        if (d0 >> 23) == 0x17F:                                                       # SOPP
            tgt = pc + 4 + 4 * sx(d0 & 0xFFFF, 16)
            br = {"S_BRANCH": "true", "S_CBRANCH_SCC0": "scc == 0u", "S_CBRANCH_SCC1": "scc != 0u", "S_CBRANCH_VCCZ": "vcc == 0",
                  "S_CBRANCH_VCCNZ": "vcc != 0", "S_CBRANCH_EXECZ": "exec == 0", "S_CBRANCH_EXECNZ": "exec != 0"}
            if nm in br: s = "if (%s) { pc = 0x%X; continue; }" % (br[nm], tgt)
            elif nm == "S_ENDPGM": s = "return o;"
            elif nm in ("S_NOP", "S_WAITCNT", "S_CLAUSE", "S_INST_PREFETCH", "S_CODE_END", "S_SETPRIO", "S_DCACHE_INV", "S_SLEEP", "S_TTRACEDATA",
                        "S_SENDMSG", "S_SENDMSGHALT", "S_BARRIER", "S_ICACHE_INV", "S_WAITCNT_DEPCTR", "S_DENORM_MODE", "S_ROUND_MODE", "S_SETHALT"): s = ""
            else: raise Unsupported(nm)
        elif (d0 >> 23) == 0x17D:                                                     # SOP1
            dst, c0 = (d0 >> 16) & 0x7F, d0 & 0xFF; lit = d1
            a = lambda: ssrc(c0, lit); a64 = lambda: ssrc64(c0, lit)
            m32 = {"S_MOV_B32": a, "S_NOT_B32": lambda: "~" + a(), "S_BREV_B32": lambda: "reversebits(%s)" % a(),
                   "S_FF1_I32_B32": lambda: "firstbitlow(%s)" % a(), "S_FLBIT_I32_B32": lambda: "(31u - firstbithigh(%s))" % a(),
                   "S_SEXT_I32_I16": lambda: "asuint((int)(%s << 16) >> 16)" % a(), "S_SEXT_I32_I8": lambda: "asuint((int)(%s << 24) >> 24)" % a(),
                   "S_BCNT1_I32_B32": lambda: "countbits(%s)" % a(), "S_BITSET1_B32": lambda: "s[%d] | (1u << (%s & 31u))" % (dst, a()) if dst <= 105 else None}
            if nm in m32: s = set_s(dst, m32[nm]())
            elif nm == "S_MOV_B64": s = set_s64(dst, a64())
            elif nm == "S_NOT_B64": s = set_s64(dst, "~" + a64())
            elif nm == "S_BITSET0_B32" and dst <= 105: s = "s[%d] &= ~(1u << (%s & 31u));" % (dst, a())
            elif nm == "S_WQM_B32" or nm == "S_WQM_B64": s = set_s64(dst, "wqm(%s)" % a64()) + " scc = 1u;"
            elif nm in ("S_AND_SAVEEXEC_B64", "S_AND_SAVEEXEC_B32"): s = "{ uint64_t t_ = %s; %s exec &= t_; scc = exec != 0 ? 1u : 0u; }" % (a64(), set_s64(dst, "exec"))
            elif nm in ("S_OR_SAVEEXEC_B64", "S_OR_SAVEEXEC_B32"): s = "{ uint64_t t_ = %s; %s exec |= t_; scc = exec != 0 ? 1u : 0u; }" % (a64(), set_s64(dst, "exec"))
            elif nm in ("S_ANDN2_SAVEEXEC_B64", "S_ANDN2_SAVEEXEC_B32"): s = "{ uint64_t t_ = %s; %s exec = t_ & ~exec; scc = exec != 0 ? 1u : 0u; }" % (a64(), set_s64(dst, "exec"))
            elif nm in ("S_ANDN1_SAVEEXEC_B64",): s = "{ uint64_t t_ = %s; %s exec = ~t_ & exec; scc = exec != 0 ? 1u : 0u; }" % (a64(), set_s64(dst, "exec"))
            elif nm in ("S_ANDN1_WREXEC_B64", "S_ANDN1_WREXEC_B32"): s = "{ exec = ~%s & exec; %s scc = exec != 0 ? 1u : 0u; }" % (a64(), set_s64(dst, "exec"))
            elif nm in ("S_ANDN2_WREXEC_B64", "S_ANDN2_WREXEC_B32"): s = "{ exec = %s & ~exec; %s scc = exec != 0 ? 1u : 0u; }" % (a64(), set_s64(dst, "exec"))
            elif nm in ("S_GETPC_B64",): s = set_s64(dst, "0x%Xull" % (pc + 4))
            elif nm in ("S_SETPC_B64", "S_SWAPPC_B64"): raise Unsupported(nm)
            else: raise Unsupported(nm)
        elif (d0 >> 23) == 0x17E:                                                     # SOPC
            lit = d1; a, b = ssrc(d0 & 0xFF, lit), ssrc((d0 >> 8) & 0xFF, lit)
            p = nm.split("_")
            if nm.startswith("S_CMP_") and p[-1] in ("I32", "U32"):
                o = {"EQ": "==", "LG": "!=", "GT": ">", "GE": ">=", "LT": "<", "LE": "<="}[p[2]]
                s = "scc = (%s %s %s) ? 1u : 0u;" % (("(int)" + a, o, "(int)" + b) if p[-1] == "I32" else (a, o, b))
            elif nm in ("S_CMP_EQ_U64", "S_CMP_LG_U64"): s = "scc = (%s %s %s) ? 1u : 0u;" % (ssrc64(d0 & 0xFF, lit), "==" if "EQ" in nm else "!=", ssrc64((d0 >> 8) & 0xFF, lit))
            elif nm == "S_BITCMP0_B32": s = "scc = ((%s >> (%s & 31u)) & 1u) == 0u ? 1u : 0u;" % (a, b)
            elif nm == "S_BITCMP1_B32": s = "scc = ((%s >> (%s & 31u)) & 1u) != 0u ? 1u : 0u;" % (a, b)
            else: raise Unsupported(nm)
        elif (d0 >> 28) == 0xB:                                                       # SOPK
            dst = (d0 >> 16) & 0x7F; imm = sx(d0 & 0xFFFF, 16)
            if nm == "S_MOVK_I32": s = set_s(dst, "0x%08Xu" % (imm & 0xFFFFFFFF))
            elif nm == "S_ADDK_I32": s = set_s(dst, "%s + 0x%08Xu" % (ssrc(dst, 0), imm & 0xFFFFFFFF))
            elif nm == "S_MULK_I32": s = set_s(dst, "%s * 0x%08Xu" % (ssrc(dst, 0), imm & 0xFFFFFFFF))
            elif nm == "S_CMOVK_I32": s = "if (scc != 0u) { %s }" % set_s(dst, "0x%08Xu" % (imm & 0xFFFFFFFF))
            elif nm.startswith("S_CMPK_"):
                o = {"EQ": "==", "LG": "!=", "GT": ">", "GE": ">=", "LT": "<", "LE": "<="}[nm.split("_")[2]]
                s = "scc = (%s %s %s) ? 1u : 0u;" % (("(int)%s" % ssrc(dst, 0), o, str(imm)) if nm.endswith("I32") else (ssrc(dst, 0), o, "%du" % (d0 & 0xFFFF)))
            elif nm in ("S_GETREG_B32", "S_SETREG_B32", "S_SETREG_IMM32_B32", "S_WAITCNT_VSCNT", "S_WAITCNT_VMCNT", "S_WAITCNT_EXPCNT", "S_WAITCNT_LGKMCNT"):
                s = set_s(dst, "0u") if nm == "S_GETREG_B32" else ""
            else: raise Unsupported(nm)
        elif (d0 >> 30) == 2:                                                         # SOP2
            dst, c0, c1 = (d0 >> 16) & 0x7F, d0 & 0xFF, (d0 >> 8) & 0xFF; lit = d1
            a, b = (lambda: ssrc(c0, lit)), (lambda: ssrc(c1, lit))
            a64, b64 = (lambda: ssrc64(c0, lit)), (lambda: ssrc64(c1, lit))
            b32 = {"S_AND_B32": "&", "S_OR_B32": "|", "S_XOR_B32": "^"}
            if nm in b32: s = "{ uint t_ = %s %s %s; %s scc = t_ != 0u ? 1u : 0u; }" % (a(), b32[nm], b(), set_s(dst, "t_"))
            elif nm == "S_ANDN2_B32": s = "{ uint t_ = %s & ~%s; %s scc = t_ != 0u ? 1u : 0u; }" % (a(), b(), set_s(dst, "t_"))
            elif nm == "S_ORN2_B32": s = "{ uint t_ = %s | ~%s; %s scc = t_ != 0u ? 1u : 0u; }" % (a(), b(), set_s(dst, "t_"))
            elif nm == "S_NAND_B32": s = "{ uint t_ = ~(%s & %s); %s scc = t_ != 0u ? 1u : 0u; }" % (a(), b(), set_s(dst, "t_"))
            elif nm == "S_NOR_B32": s = "{ uint t_ = ~(%s | %s); %s scc = t_ != 0u ? 1u : 0u; }" % (a(), b(), set_s(dst, "t_"))
            elif nm in ("S_AND_B64", "S_OR_B64", "S_XOR_B64"): s = "{ uint64_t t_ = %s %s %s; %s scc = t_ != 0 ? 1u : 0u; }" % (a64(), {"S_AND_B64": "&", "S_OR_B64": "|", "S_XOR_B64": "^"}[nm], b64(), set_s64(dst, "t_"))
            elif nm == "S_ANDN2_B64": s = "{ uint64_t t_ = %s & ~%s; %s scc = t_ != 0 ? 1u : 0u; }" % (a64(), b64(), set_s64(dst, "t_"))
            elif nm == "S_ORN2_B64": s = "{ uint64_t t_ = %s | ~%s; %s scc = t_ != 0 ? 1u : 0u; }" % (a64(), b64(), set_s64(dst, "t_"))
            elif nm == "S_NAND_B64": s = "{ uint64_t t_ = ~(%s & %s); %s scc = t_ != 0 ? 1u : 0u; }" % (a64(), b64(), set_s64(dst, "t_"))
            elif nm == "S_XNOR_B64": s = "{ uint64_t t_ = ~(%s ^ %s); %s scc = t_ != 0 ? 1u : 0u; }" % (a64(), b64(), set_s64(dst, "t_"))
            elif nm == "S_XNOR_B32": s = "{ uint t_ = ~(%s ^ %s); %s scc = t_ != 0u ? 1u : 0u; }" % (a(), b(), set_s(dst, "t_"))
            elif nm == "S_NOR_B64": s = "{ uint64_t t_ = ~(%s | %s); %s scc = t_ != 0 ? 1u : 0u; }" % (a64(), b64(), set_s64(dst, "t_"))
            elif nm == "S_CSELECT_B32": s = set_s(dst, "scc != 0u ? %s : %s" % (a(), b()))
            elif nm == "S_CSELECT_B64": s = set_s64(dst, "scc != 0u ? %s : %s" % (a64(), b64()))
            elif nm == "S_ADD_U32": s = "{ uint64_t t_ = (uint64_t)%s + (uint64_t)%s; %s scc = (uint)(t_ >> 32); }" % (a(), b(), set_s(dst, "(uint)t_"))
            elif nm == "S_ADDC_U32": s = "{ uint64_t t_ = (uint64_t)%s + (uint64_t)%s + scc; %s scc = (uint)(t_ >> 32); }" % (a(), b(), set_s(dst, "(uint)t_"))
            elif nm == "S_SUB_U32": s = "{ uint x_ = %s, y_ = %s; %s scc = y_ > x_ ? 1u : 0u; }" % (a(), b(), set_s(dst, "x_ - y_"))
            elif nm == "S_SUBB_U32": s = "{ uint x_ = %s, y_ = %s, c_ = scc; %s scc = ((uint64_t)y_ + c_ > x_) ? 1u : 0u; }" % (a(), b(), set_s(dst, "x_ - y_ - c_"))
            elif nm in ("S_ADD_I32", "S_SUB_I32"): s = set_s(dst, "%s %s %s" % (a(), "+" if "ADD" in nm else "-", b()))
            elif nm == "S_MUL_I32": s = set_s(dst, "%s * %s" % (a(), b()))
            elif nm == "S_MUL_HI_U32": s = set_s(dst, "(uint)(((uint64_t)%s * (uint64_t)%s) >> 32)" % (a(), b()))
            elif nm == "S_LSHL_B32": s = "{ uint t_ = %s << (%s & 31u); %s scc = t_ != 0u ? 1u : 0u; }" % (a(), b(), set_s(dst, "t_"))
            elif nm == "S_LSHR_B32": s = "{ uint t_ = %s >> (%s & 31u); %s scc = t_ != 0u ? 1u : 0u; }" % (a(), b(), set_s(dst, "t_"))
            elif nm == "S_ASHR_I32": s = "{ uint t_ = asuint((int)%s >> (int)(%s & 31u)); %s scc = t_ != 0u ? 1u : 0u; }" % (a(), b(), set_s(dst, "t_"))
            elif nm == "S_LSHL_B64": s = "{ uint64_t t_ = %s << (%s & 63u); %s scc = t_ != 0 ? 1u : 0u; }" % (a64(), b(), set_s64(dst, "t_"))
            elif nm == "S_LSHR_B64": s = "{ uint64_t t_ = %s >> (%s & 63u); %s scc = t_ != 0 ? 1u : 0u; }" % (a64(), b(), set_s64(dst, "t_"))
            elif nm in ("S_LSHL1_ADD_U32", "S_LSHL2_ADD_U32", "S_LSHL3_ADD_U32", "S_LSHL4_ADD_U32"):
                k = int(nm[6]); s = set_s(dst, "(%s << %d) + %s" % (a(), k, b()))
            elif nm in ("S_MIN_U32", "S_MAX_U32"): s = "{ uint x_ = %s, y_ = %s; %s scc = x_ %s y_ ? 1u : 0u; }" % (a(), b(), set_s(dst, "%s(x_, y_)" % nm[2:5].lower()), "<" if "MIN" in nm else ">")
            elif nm in ("S_MIN_I32", "S_MAX_I32"): s = "{ int x_ = (int)%s, y_ = (int)%s; %s scc = x_ %s y_ ? 1u : 0u; }" % (a(), b(), set_s(dst, "asuint(%s(x_, y_))" % nm[2:5].lower()), "<" if "MIN" in nm else ">")
            elif nm == "S_BFE_U32": s = "{ uint x_ = %s, y_ = %s, o_ = y_ & 31u, w_ = (y_ >> 16) & 127u; uint t_ = w_ == 0u ? 0u : (w_ >= 32u ? (x_ >> o_) : (x_ >> o_) & ((1u << w_) - 1u)); %s scc = t_ != 0u ? 1u : 0u; }" % (a(), b(), set_s(dst, "t_"))
            elif nm == "S_BFE_I32": s = "{ uint x_ = %s, y_ = %s, o_ = y_ & 31u, w_ = (y_ >> 16) & 127u; uint t_ = w_ == 0u ? 0u : asuint((int)(x_ << (32u - o_ - min(w_, 32u - o_))) >> (32u - min(w_, 32u - o_))); %s scc = t_ != 0u ? 1u : 0u; }" % (a(), b(), set_s(dst, "t_"))
            elif nm == "S_BFM_B32": s = set_s(dst, "(((1u << (%s & 31u)) - 1u) << (%s & 31u))" % (a(), b()))
            elif nm == "S_BFM_B64": s = set_s64(dst, "(((1ull << (%s & 63u)) - 1ull) << (%s & 63u))" % (a(), b()))
            elif nm == "S_PACK_LL_B32_B16": s = set_s(dst, "(%s & 0xFFFFu) | (%s << 16)" % (a(), b()))
            elif nm == "S_PACK_LH_B32_B16": s = set_s(dst, "(%s & 0xFFFFu) | (%s & 0xFFFF0000u)" % (a(), b()))
            elif nm == "S_PACK_HH_B32_B16": s = set_s(dst, "(%s >> 16) | (%s & 0xFFFF0000u)" % (a(), b()))
            elif nm == "S_ABSDIFF_I32": s = set_s(dst, "(uint)abs((int)%s - (int)%s)" % (a(), b()))
            else: raise Unsupported(nm)
        elif (d0 >> 26) == 0x3D:                                                      # SMEM
            sdata, sbase = (d0 >> 6) & 0x7F, (d0 & 0x3F) * 2
            off = sx(d1 & 0x1FFFFF, 21); soff = d1 >> 25
            cnt = {"S_LOAD_DWORD": 1, "S_LOAD_DWORDX2": 2, "S_LOAD_DWORDX4": 4, "S_LOAD_DWORDX8": 8, "S_LOAD_DWORDX16": 16,
                   "S_BUFFER_LOAD_DWORD": 1, "S_BUFFER_LOAD_DWORDX2": 2, "S_BUFFER_LOAD_DWORDX4": 4, "S_BUFFER_LOAD_DWORDX8": 8, "S_BUFFER_LOAD_DWORDX16": 16}.get(nm)
            if nm in ("S_DCACHE_INV", "S_GL1_INV", "S_MEMTIME", "S_MEMREALTIME"): s = "" if "INV" in nm else set_s64(sdata, "0ull")
            elif not cnt: raise Unsupported(nm)
            else:
                addr = "memaddr(s[%d], s[%d] & 0xFFFFu) + (uint64_t)(int64_t)%d%s" % (sbase, sbase + 1, off, "" if soff == 125 else " + %s" % ssrc(soff, 0))
                s = "{ uint64_t a_ = %s; %s }" % (addr, " ".join(set_s(sdata + i, "memload(a_ + %du)" % (4 * i)) for i in range(cnt)))
        elif (d0 >> 26) in (0x38, 0x3A):                                              # MUBUF / MTBUF
            is_t = (d0 >> 26) == 0x3A
            off12 = d0 & 0xFFF; offen = (d0 >> 12) & 1; idxen = (d0 >> 13) & 1
            vaddr, vdata, srsrc, soff = d1 & 0xFF, (d1 >> 8) & 0xFF, ((d1 >> 16) & 0x1F) * 4, d1 >> 24
            base = "memaddr(s[%d], s[%d] & 0xFFFFu)" % (srsrc, srsrc + 1)
            stride = "((s[%d] >> 16) & 0x3FFFu)" % (srsrc + 1)
            idx = "v[%d]" % vaddr if idxen else "0u"
            voff = ("v[%d]" % (vaddr + 1 if idxen else vaddr)) if offen else "0u"
            so = "" if soff in (125, 128) else " + %s" % ssrc(soff, 0)
            addr = "%s + %s * %s + %s + %du%s" % (base, idx, stride, voff, off12, so)
            loads = {"BUFFER_LOAD_DWORD": 1, "BUFFER_LOAD_DWORDX2": 2, "BUFFER_LOAD_DWORDX3": 3, "BUFFER_LOAD_DWORDX4": 4,
                     "BUFFER_LOAD_FORMAT_X": 1, "BUFFER_LOAD_FORMAT_XY": 2, "BUFFER_LOAD_FORMAT_XYZ": 3, "BUFFER_LOAD_FORMAT_XYZW": 4,
                     "TBUFFER_LOAD_FORMAT_X": 1, "TBUFFER_LOAD_FORMAT_XY": 2, "TBUFFER_LOAD_FORMAT_XYZ": 3, "TBUFFER_LOAD_FORMAT_XYZW": 4}
            if nm in loads:
                c = loads[nm]; maxv = max(maxv, vdata + c)
                fmt = "FORMAT" in nm
                if fmt: s = "if (ACT) { uint64_t a_ = %s; float4 f_ = fmtload(a_, s[%d]); %s }" % (addr, srsrc + 3, " ".join("v[%d] = asuint(f_[%d]);" % (vdata + i, i) for i in range(c)))
                else: s = "if (ACT) { uint64_t a_ = %s; %s }" % (addr, " ".join("v[%d] = memload(a_ + %du);" % (vdata + i, 4 * i) for i in range(c)))
            elif nm.startswith("BUFFER_STORE") or nm.startswith("TBUFFER_STORE") or nm.startswith("BUFFER_ATOMIC"): s = ""   # no writes to game memory from VS/PS yet
            elif nm in ("BUFFER_LOAD_UBYTE", "BUFFER_LOAD_USHORT", "BUFFER_LOAD_SBYTE", "BUFFER_LOAD_SSHORT"):
                maxv = max(maxv, vdata + 1)
                sh = {"BUFFER_LOAD_UBYTE": "& 255u", "BUFFER_LOAD_USHORT": "& 0xFFFFu"}.get(nm, "")
                s = "if (ACT) { uint64_t a_ = %s; v[%d] = (memload(a_) >> (8u * (uint)(a_ & 3ull))) %s; }" % (addr, vdata, sh or "")
            elif nm in ("BUFFER_GL0_INV", "BUFFER_GL1_INV"): s = ""
            else: raise Unsupported(nm)
        elif (d0 >> 26) == 0x3C:                                                      # MIMG: placeholder until bindless textures
            vdata = (d1 >> 8) & 0xFF; dmask = (d0 >> 8) & 0xF
            if nm.startswith("IMAGE_SAMPLE") or nm.startswith("IMAGE_GATHER") or nm.startswith("IMAGE_LOAD"):
                c = max(1, bin(dmask).count("1")) if not nm.startswith("IMAGE_GATHER") else 4
                maxv = max(maxv, vdata + c)
                s = "if (ACT) { %s }" % " ".join("v[%d] = 0u;" % (vdata + i) for i in range(c))
            elif nm.startswith("IMAGE_STORE") or nm.startswith("IMAGE_ATOMIC"): s = ""
            elif nm.startswith("IMAGE_GET_LOD"):
                maxv = max(maxv, vdata + 2); s = "if (ACT) { v[%d] = 0u; v[%d] = 0u; }" % (vdata, vdata + 1)
            elif nm.startswith("IMAGE_GET_RESINFO"):
                maxv = max(maxv, vdata + 4); s = "if (ACT) { v[%d] = 1u; v[%d] = 1u; v[%d] = 1u; v[%d] = 1u; }" % (vdata, vdata + 1, vdata + 2, vdata + 3)
            else: raise Unsupported(nm)
        elif (d0 >> 26) == 0x36:                                                      # DS (LDS): not available in VS/PS here
            raise Unsupported(nm)
        elif (d0 >> 26) == 0x32 and stage == "ps":                                    # VINTRP: hardware-style i/j interpolation of raw vertex attributes
            dst = (d0 >> 18) & 0xFF; attr = (d0 >> 10) & 0x3F; chan = (d0 >> 8) & 3; vs_ = d0 & 0xFF; maxv = max(maxv, dst + 1); attrs.add(attr)
            P = lambda k: "asfloat(GetAttributeAtVertex(inp.attr%d, %d)[%d])" % (attr, k, chan)
            if nm == "V_INTERP_P1_F32": s = "if (ACT) v[%d] = asuint(%s + asfloat(v[%d]) * (%s - %s));" % (dst, P(0), vs_, P(1), P(0))
            elif nm == "V_INTERP_P2_F32": s = "if (ACT) v[%d] = asuint(asfloat(v[%d]) + asfloat(v[%d]) * (%s - %s));" % (dst, dst, vs_, P(2), P(0))
            elif nm == "V_INTERP_MOV_F32": s = "if (ACT) v[%d] = asuint(%s);" % (dst, P({0: 1, 1: 2, 2: 0}.get(vs_ & 3, 0)))
            else: raise Unsupported(nm)
        elif (d0 >> 26) == 0x3E:                                                      # EXP
            en = d0 & 0xF; tgt = (d0 >> 4) & 0x3F; compr = (d0 >> 10) & 1
            vv = [(d1 >> (8 * i)) & 0xFF for i in range(4)]
            if tgt == 9: s = ""                                                       # null export
            elif stage == "vs" and tgt == 12: s = "o.pos = float4(%s);" % ", ".join("asfloat(v[%d])" % vv[i] if en >> i & 1 else ("1.0f" if i == 3 else "0.0f") for i in range(4))
            elif stage == "vs" and 13 <= tgt <= 16: s = ""                            # extra position exports (clip distances, point size)
            elif stage == "vs" and tgt == 20: s = ""                                  # primitive export
            elif stage == "vs" and 32 <= tgt < 64:
                if tgt - 32 >= NATTR: raise Unsupported("param export %d" % (tgt - 32))
                params.add(tgt - 32); s = "o.attr%d = uint4(%s);" % (tgt - 32, ", ".join("v[%d]" % vv[i] if en >> i & 1 else "0u" for i in range(4)))
            elif stage == "ps" and tgt <= 7:
                mrts.add(tgt)
                if compr: s = "o.c%d = float4(f16tof32(v[%d]), f16tof32(v[%d] >> 16), f16tof32(v[%d]), f16tof32(v[%d] >> 16));" % (tgt, vv[0], vv[0], vv[1], vv[1])
                else: s = "o.c%d = float4(%s);" % (tgt, ", ".join("asfloat(v[%d])" % vv[i] if en >> i & 1 else "0.0f" for i in range(4)))
            elif stage == "ps" and tgt == 8: s = ""                                   # depth export
            else: raise Unsupported("exp target %d" % tgt)
        elif (d0 >> 25) in (0x3F, 0x3E) or (d0 >> 31) == 0 or (d0 >> 26) in (0x35, 0x33):   # VOP1/VOPC/VOP2/VOP3/VOP3P
            vop3 = (d0 >> 26) == 0x35; vop3p = (d0 >> 26) == 0x33; sdwa = None; dpp = None
            if vop3 or vop3p:
                lit = d2 if n == 12 else 0
                dst = d0 & 0xFF; c0, c1, c2 = d1 & 0x1FF, (d1 >> 9) & 0x1FF, (d1 >> 18) & 0x1FF
                absm = (d0 >> 8) & 7 if vop3 else 0; negm = d1 >> 29; clamp = (d0 >> 15) & 1; omod = (d1 >> 27) & 3 if vop3 else 0
                sdst_b = (d0 >> 8) & 0x7F
                opsel = (d0 >> 11) & 15; opsel_hi = ((d1 >> 27) & 3) | (((d0 >> 14) & 1) << 2) if vop3p else 0
            else:
                lit = d1; c0 = d0 & 0x1FF; c1 = 256 + ((d0 >> 9) & 0xFF); c2 = None; dst = (d0 >> 17) & 0xFF
                absm = negm = clamp = omod = 0; sdst_b = 106; opsel = opsel_hi = 0
                if c0 == 249:                                                         # SDWA: sub-dword operand / result selection
                    sdwa = d1; lit = 0
                    c0 = (sdwa & 0xFF) + (0 if (sdwa >> 23) & 1 else 256)
                    if (sdwa >> 31) & 1: c1 -= 256
                    absm = ((sdwa >> 21) & 1) | (((sdwa >> 29) & 1) << 1); negm = ((sdwa >> 20) & 1) | (((sdwa >> 28) & 1) << 1)
                    if (d0 >> 25) == 0x3E:
                        if (sdwa >> 15) & 1: sdst_b = (sdwa >> 8) & 0x7F
                    else: clamp = (sdwa >> 13) & 1; omod = (sdwa >> 14) & 3
                elif c0 == 250:                                                       # DPP16: cross-lane source read
                    dpp = d1; lit = 0; c0 = 256 + (dpp & 0xFF)
                    absm = ((dpp >> 21) & 1) | (((dpp >> 23) & 1) << 1); negm = ((dpp >> 20) & 1) | (((dpp >> 22) & 1) << 1)
                elif c0 in (233, 234): raise Unsupported("DPP8 " + nm)
            a, b = vsrc(c0, lit), vsrc(c1, lit); c = vsrc(c2, lit) if c2 is not None else "0u"
            if sdwa is not None:
                a, b = sdsel(a, (sdwa >> 16) & 7, (sdwa >> 19) & 1), sdsel(b, (sdwa >> 24) & 7, (sdwa >> 27) & 1)
            dpp_pre = dpp_cond = None
            if dpp is not None:
                ctrl, bc, rowm, bankm = (dpp >> 8) & 0x1FF, (dpp >> 19) & 1, (dpp >> 28) & 15, (dpp >> 24) & 15
                x = "v[%d]" % (c0 - 256)
                if ctrl <= 0xFF: sl, valid, rd = "((lane & ~3u) | ((0x%Xu >> (2u * (lane & 3u))) & 3u))" % ctrl, "true", "dpq(%s, 0x%Xu, lane)" % (x, ctrl)
                elif 0x101 <= ctrl <= 0x10F: n_ = ctrl & 15; sl, valid = "(lane + %du)" % n_, "((lane & 15u) + %du < 16u)" % n_
                elif 0x111 <= ctrl <= 0x11F: n_ = ctrl & 15; sl, valid = "(lane - %du)" % n_, "((lane & 15u) >= %du)" % n_
                elif 0x121 <= ctrl <= 0x12F: n_ = ctrl & 15; sl, valid = "((lane & ~15u) | ((lane + %du) & 15u))" % n_, "true"
                elif ctrl == 0x140: sl, valid = "((lane & ~15u) | (15u - (lane & 15u)))", "true"
                elif ctrl == 0x141: sl, valid = "((lane & ~7u) | (7u - (lane & 7u)))", "true"
                elif 0x150 <= ctrl <= 0x15F: sl, valid = "((lane & ~15u) | %du)" % (ctrl & 15), "true"
                elif 0x160 <= ctrl <= 0x16F: sl, valid = "((lane & ~15u) | ((lane & 15u) ^ %du))" % (ctrl & 15), "true"
                else: raise Unsupported("DPP ctrl 0x%X" % ctrl)
                if ctrl > 0xFF: rd = "WaveReadLaneAt(%s, %s & 63u)" % (x, sl)
                dpp_pre = "uint dsl_ = %s & 63u; bool dok_ = %s && ((exec >> dsl_) & 1ull) != 0; uint dv_ = %s; dv_ = dok_ ? dv_ : 0u;" % (sl, valid, rd)
                dpp_cond = "(dok_ || %s) && ((0x%Xu >> (lane >> 4)) & 1u) != 0u && ((0x%Xu >> ((lane >> 2) & 3u)) & 1u) != 0u" % ("true" if bc else "false", rowm, bankm)
                a = "dv_"
            dpp_on[0] = (dpp_pre, dpp_cond) if dpp is not None else None
            def wr(r):
                if sdwa is None or (d0 >> 25) == 0x3E or ((sdwa >> 8) & 7) == 6: return "v[%d] = %s;" % (dst, r)
                ds, du = (sdwa >> 8) & 7, (sdwa >> 11) & 3
                sh, w = (8 * ds, 8) if ds < 4 else (16 * (ds - 4), 16)
                m = ((1 << w) - 1) << sh
                if du == 2: return "v[%d] = (v[%d] & 0x%08Xu) | ((%s << %d) & 0x%08Xu);" % (dst, dst, ~m & 0xFFFFFFFF, r, sh, m)
                if du == 1: return "v[%d] = asuint((int)((%s) << %du) >> %d);" % (dst, r, 32 - w, 32 - w - sh)
                return "v[%d] = ((%s) << %d) & 0x%08Xu;" % (dst, r, sh, m)
            if nm in ("V_INTERP_P1LL_F16", "V_INTERP_P2_F16") and stage == "ps":     # VOP3 f16 interpolation: src0 = attr | chan<<6 | high<<8
                at, ch, hi = c0 & 0x3F, (c0 >> 6) & 3, (c0 >> 8) & 1; attrs.add(at); maxv = max(maxv, dst + 1)
                P = lambda k: "f16tof32(GetAttributeAtVertex(inp.attr%d, %d)[%d] >> %du)" % (at, k, ch, 16 * hi)
                if nm == "V_INTERP_P1LL_F16": s = "if (ACT) v[%d] = asuint(%s + asfloat(%s) * (%s - %s));" % (dst, P(0), b, P(1), P(0))
                else: s = "if (ACT) v[%d] = (v[%d] & 0xFFFF0000u) | f32tof16(asfloat(%s) + asfloat(%s) * (%s - %s));" % (dst, dst, c, b, P(2), P(0))
                E("        " + s); continue
            def fsrc(e, i):
                x = "asfloat(%s)" % e
                if absm >> i & 1: x = "abs(%s)" % x
                if negm >> i & 1: x = "(-%s)" % x
                return x
            pre = "uint a = %s, b = %s, c = %s, d = v[%d]; float fa = %s, fb = %s, fc = %s; uint L = 0x%08Xu;" % (a, b, c, dst, fsrc("a", 0), fsrc("b", 1), fsrc("c", 2), lit)
            if nm == "V_FMA_MIX_F32" or nm.startswith("V_FMA_MIX"):
                def mix(e, i): return "(((opsel_hi_ >> %d) & 1u) != 0u ? f16tof32((%s >> (((opsel_ >> %d) & 1u) * 16u)) & 0xFFFFu) : asfloat(%s))" % (i, e, i, e)
                pre += " uint opsel_ = %du, opsel_hi_ = %du; float mixa = %s, mixb = %s, mixc = %s;" % (opsel, opsel_hi, mix("a", 0), mix("b", 1), mix("c", 2))
                if negm & 1: pre += " mixa = -mixa;"
                if negm & 2: pre += " mixb = -mixb;"
                if negm & 4: pre += " mixc = -mixc;"
            if nm.startswith("V_CMP") or nm.startswith("V_CMPX"):
                ce = cmp_expr(nm.replace("V_CMPX_", "V_CMP_"))
                if "64" in nm.split("_")[-1]: pre += " uint64_t A64 = %s, B64 = %s;" % ("((uint64_t)v[%d] | ((uint64_t)v[%d] << 32))" % (c0 - 256, c0 - 255) if c0 >= 256 else ssrc64(c0, lit), "((uint64_t)v[%d] | ((uint64_t)v[%d] << 32))" % (c1 - 256, c1 - 255) if c1 >= 256 else ssrc64(c1, lit))
                dstm = sdst_b if vop3 else 106
                ball = "{ %s uint64_t m_ = ballot(ACT && %s); %s }" % (pre, ce, ("exec = m_;" if nm.startswith("V_CMPX") else set_s64(dstm, "m_")))
                s = ball
            elif nm in ("V_CNDMASK_B32",):
                sel = ("(((%s) >> lane) & 1ull)" % ssrc64(c2, lit)) if vop3 else "((vcc >> lane) & 1ull)"
                s = "if (ACT) { %s %s }" % (pre, wr("(%s != 0 ? %s : %s)" % (sel, ("asuint(%s)" % fsrc("b", 1)) if (absm or negm) else "b", ("asuint(%s)" % fsrc("a", 0)) if (absm or negm) else "a")))
                maxv = max(maxv, dst + 1)
            elif nm in ("V_PERMLANEX16_B32", "V_PERMLANE16_B32"):
                xr = "16u" if "X16" in nm else "0u"
                s = "{ uint64_t sel_ = (uint64_t)%s | ((uint64_t)%s << 32); uint sl_ = (((lane & ~15u) ^ %s) | (uint)((sel_ >> (4u * (lane & 15u))) & 15ull)) & 63u; uint pv_ = WaveReadLaneAt(%s, sl_); if (ACT) v[%d] = pv_; }" % (
                    ssrc(c1, lit) if c1 < 256 else "v[%d]" % (c1 - 256), ssrc(c2, lit) if c2 < 256 else "v[%d]" % (c2 - 256), xr, a, dst)
                maxv = max(maxv, dst + 1)
            elif nm == "V_MBCNT_LO_U32_B32": s = "if (ACT) { %s v[%d] = countbits(lane >= 32u ? a : (a & ((1u << lane) - 1u))) + b; }" % (pre, dst); maxv = max(maxv, dst + 1)
            elif nm == "V_MBCNT_HI_U32_B32": s = "if (ACT) { %s v[%d] = countbits(lane < 32u ? 0u : (a & ((1u << (lane - 32u)) - 1u))) + b; }" % (pre, dst); maxv = max(maxv, dst + 1)
            elif nm == "V_SWAP_B32": s = "if (ACT) { uint t_ = v[%d]; v[%d] = v[%d]; v[%d] = t_; }" % (dst, dst, c0 - 256, c0 - 256); maxv = max(maxv, dst + 1)
            elif nm == "V_READFIRSTLANE_B32": s = set_s(dst, "WaveReadLaneAt(%s, firstlane(exec))" % a)
            elif nm == "V_READLANE_B32": s = set_s(sdst_b if not vop3 else dst, "WaveReadLaneAt(%s, %s & 63u)" % (a, ssrc(c1, lit) if vop3 else "0u"))
            elif nm == "V_WRITELANE_B32": s = "if (lane == (%s & 63u)) v[%d] = %s;" % (ssrc(c1, lit), dst, ssrc(c0, lit)); maxv = max(maxv, dst + 1)
            elif nm in ("V_ADD_CO_U32", "V_ADD_CO_CI_U32", "V_SUB_CO_U32", "V_SUBREV_CO_U32", "V_SUB_CO_CI_U32", "V_SUBREV_CO_CI_U32"):
                cin = ("((%s >> lane) & 1ull)" % (ssrc64(c2, lit) if vop3 else "vcc")) if "CI" in nm else "0ull"
                if "SUBREV" in nm: ex, cout = "(uint64_t)b - (uint64_t)a - %s" % cin, "((uint64_t)a + %s > (uint64_t)b)" % cin
                elif "SUB" in nm: ex, cout = "(uint64_t)a - (uint64_t)b - %s" % cin, "((uint64_t)b + %s > (uint64_t)a)" % cin
                else: ex, cout = "(uint64_t)a + (uint64_t)b + %s" % cin, "(((uint64_t)a + (uint64_t)b + %s) >> 32) != 0" % cin
                cdst = sdst_b if vop3 else 106
                s = "{ %s uint64_t r_ = %s; bool co_ = %s; if (ACT) v[%d] = (uint)r_; %s }" % (pre, ex, cout, dst, set_s64(cdst, "ballot(ACT && co_)"))
                maxv = max(maxv, dst + 1)
            elif nm in ("V_MAD_U64_U32", "V_MAD_I64_I32"):
                pre64 = "uint64_t C64 = %s;" % ("((uint64_t)v[%d] | ((uint64_t)v[%d] << 32))" % (c2 - 256, c2 - 255) if c2 >= 256 else ssrc64(c2, lit))
                mul = "(uint64_t)a * (uint64_t)b" if "U64" in nm else "(uint64_t)((int64_t)(int)a * (int64_t)(int)b)"
                s = "if (ACT) { %s %s uint64_t r_ = %s + C64; v[%d] = (uint)r_; v[%d] = (uint)(r_ >> 32); }" % (pre, pre64, mul, dst, dst + 1)
                maxv = max(maxv, dst + 2)
            elif nm in ("V_LSHRREV_B64", "V_LSHLREV_B64", "V_ASHRREV_I64"):
                b64 = "((uint64_t)v[%d] | ((uint64_t)v[%d] << 32))" % (c1 - 256, c1 - 255) if c1 >= 256 else ssrc64(c1, lit)
                op = {"V_LSHRREV_B64": ">>", "V_LSHLREV_B64": "<<"}.get(nm)
                ex = "(%s %s (a & 63u))" % (b64, op) if op else "(uint64_t)((int64_t)%s >> (a & 63u))" % b64
                s = "if (ACT) { %s uint64_t r_ = %s; v[%d] = (uint)r_; v[%d] = (uint)(r_ >> 32); }" % (pre, ex, dst, dst + 1); maxv = max(maxv, dst + 2)
            elif nm in VOPS:
                e = VOPS[nm]
                if e is None: s = ""
                else:
                    r = e
                    if (omod or clamp) and e.startswith("asuint("):
                        r = "asuint(%s(%s%s))" % ("saturate" if clamp else "", "asfloat(%s)" % e, {0: "", 1: " * 2.0f", 2: " * 4.0f", 3: " * 0.5f"}[omod])
                    s = "if (ACT) { %s %s }" % (pre, wr(r)); maxv = max(maxv, dst + 1)
            else: raise Unsupported(nm)
        else:
            raise Unsupported(nm)
        if s and dpp_on[0]:
            s = "{ %s %s }" % (dpp_on[0][0], s.replace("if (ACT)", "if (ACT && %s)" % dpp_on[0][1], 1))
        if s: E("        " + s)
    body = "\n".join(out)
    import re
    nv = max([8, maxv] + [int(x) + 1 for x in re.findall(r"\bv\[(\d+)\]", body)])
    if nv > 256: raise Unsupported("vgpr %d" % nv)
    ns = max([106] + [int(x) + 1 for x in re.findall(r"\bs\[(\d+)\]", body)])
    if ns > 124: raise Unsupported("sgpr %d" % (ns - 1))
    hdr = r"""// generated by tools/rdna2hlsl.py (v2) from RDNA2 machine code - do not edit
cbuffer Regs : register(REGS_SLOT, space0) { uint4 g_user[8]; uint4 g_draw; };   // 32 user SGPRs; g_draw = index buffer lo/hi, flags (1 indexed, 2 32-bit), spare. VS: b0, PS: b1
ByteAddressBuffer g_ptab : register(t0, space0);                                         // pool page table: dir[65536] then 64K-entry chunks
uint64_t memaddr(uint lo, uint hi) { return (uint64_t)lo | ((uint64_t)hi << 32); }
uint memload(uint64_t a) {
    uint slot = g_ptab.Load(((uint)(a >> 32) & 0xFFFFu) * 4u); if (slot == 0u) return 0u;
    uint e = g_ptab.Load((65536u + (slot - 1u) * 65536u + ((uint)(a >> 16) & 0xFFFFu)) * 4u); if (e == 0u) return 0u;
    ByteAddressBuffer sec = ResourceDescriptorHeap[NonUniformResourceIndex((e >> 24) - 1u)];
    return sec.Load(((e & 0xFFFFFFu) << 16) | ((uint)a & 0xFFFCu));
}
uint64_t ballot(bool b) { uint4 m = WaveActiveBallot(b); return (uint64_t)m.x | ((uint64_t)m.y << 32); }
uint firstlane(uint64_t m) { uint lo = (uint)m; return lo != 0u ? firstbitlow(lo) : 32u + firstbitlow((uint)(m >> 32)); }
uint64_t wqm(uint64_t m) { uint64_t r = 0; [unroll] for (uint q = 0; q < 16; q++) if (((m >> (q * 4u)) & 15ull) != 0) r |= 15ull << (q * 4u); return r; }
uint pk(float lo, float hi) { return f32tof16(lo) | (f32tof16(hi) << 16); }
uint cvtu16(float f) { return (uint)round(saturate(f) * 65535.0f); }
uint satpk(uint a) { return (uint)clamp((int)(a << 16) >> 16, 0, 255) | ((uint)clamp((int)a >> 16, 0, 255) << 8); }
uint perm(uint a, uint b, uint sel) { uint64_t src = ((uint64_t)a << 32) | b; uint r = 0; [unroll] for (uint i = 0; i < 4; i++) { uint k = (sel >> (8u * i)) & 255u; uint by = k < 8u ? (uint)((src >> (8u * k)) & 255ull) : (k == 12u ? 0u : (k == 13u ? 255u : 0u)); r |= by << (8u * i); } return r; }
uint dpq(uint x, uint p, uint lane) { uint q0 = QuadReadLaneAt(x, 0), q1 = QuadReadLaneAt(x, 1), q2 = QuadReadLaneAt(x, 2), q3 = QuadReadLaneAt(x, 3);
    uint k = (p >> (2u * (lane & 3u))) & 3u; return k == 0u ? q0 : (k == 1u ? q1 : (k == 2u ? q2 : q3)); }
// AMD cube-map helpers (major axis, s/t coordinates, face id)
float cubema(float x, float y, float z) { float ax = abs(x), ay = abs(y), az = abs(z); return 2.0f * (az >= ax && az >= ay ? z : (ay >= ax ? y : x)); }
float cubeid(float x, float y, float z) { float ax = abs(x), ay = abs(y), az = abs(z); return az >= ax && az >= ay ? (z < 0 ? 5.0f : 4.0f) : (ay >= ax ? (y < 0 ? 3.0f : 2.0f) : (x < 0 ? 1.0f : 0.0f)); }
float cubesc(float x, float y, float z) { float ax = abs(x), ay = abs(y), az = abs(z); return az >= ax && az >= ay ? (z < 0 ? -x : x) : (ay >= ax ? x : (x < 0 ? z : -z)); }
float cubetc(float x, float y, float z) { float ax = abs(x), ay = abs(y), az = abs(z); return az >= ax && az >= ay ? -y : (ay >= ax ? (y < 0 ? -z : z) : -y); }
bool fclass(float f, uint m) { return (m != 0u); }
float4 fmtload(uint64_t off, uint w3) { return float4(asfloat(memload(off)), asfloat(memload(off + 4u)), asfloat(memload(off + 8u)), asfloat(memload(off + 12u))); }
"""
    if stage == "vs":
        io = "struct VSOut { float4 pos : SV_Position; %s };\n" % " ".join("uint4 attr%d : TEXCOORD%d;" % (i, i) for i in range(NATTR))
        entry = "VSOut main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {\n    VSOut o = (VSOut)0;\n"
        init_v = ("    uint vi = vid;\n"          # indexed draws: the vertex shader fetches its own index from game memory
                  "    if ((g_draw.z & 1u) != 0u) { uint64_t ia = ((uint64_t)g_draw.x | ((uint64_t)g_draw.y << 32)) + (uint64_t)vid * ((g_draw.z & 2u) != 0u ? 4ull : 2ull);\n"
                  "        uint w = memload(ia); vi = (g_draw.z & 2u) != 0u ? w : ((w >> (8u * (uint)(ia & 2ull))) & 0xFFFFu); }\n"
                  "    v[0] = vi; v[3] = iid;\n")
    else:
        if attrs and max(attrs) >= NATTR: raise Unsupported("attribute %d" % max(attrs))
        n_mrt = max(mrts) + 1 if mrts else 1
        io = "struct PSIn { float4 pos : SV_Position; %s float3 bary : SV_Barycentrics; };\nstruct PSOut { %s };\n" % (" ".join("nointerpolation uint4 attr%d : TEXCOORD%d;" % (i, i) for i in range(NATTR)), " ".join("float4 c%d : SV_Target%d;" % (i, i) for i in range(n_mrt)))
        entry = "PSOut main(PSIn inp) {\n    PSOut o = (PSOut)0;\n"
        init_v = "    v[0] = asuint(inp.bary.y); v[1] = asuint(inp.bary.z);\n"     # PERSP_CENTER i/j arrive in v0/v1
    entry += "    uint s[%d]; uint v[%d]; uint scc = 0u, m0 = 0u; uint64_t exec = ballot(true), vcc = 0ull; uint lane = WaveGetLaneIndex();\n" % (ns, nv)
    entry += "    [unroll] for (int i = 0; i < %d; i++) s[i] = 0u; [unroll] for (int j = 0; j < %d; j++) v[j] = 0u;\n" % (ns, nv)
    entry += "    [unroll] for (int u = 0; u < %d; u++) s[u] = g_user[u >> 2][u & 3];\n" % min(nuser, 32) + init_v
    macros = "#define ACT (((exec >> lane) & 1ull) != 0)\n#define VCCLO ((uint)vcc)\n#define VCCHI ((uint)(vcc >> 32))\n#define EXECLO ((uint)exec)\n#define EXECHI ((uint)(exec >> 32))\n"
    loop = "    uint pc = 0u;\n    [loop] for (uint guard = 0u; guard < 200000u; guard++) {\n    switch (pc) {\n" + body + "\n        return o;\n    default: return o;\n    }\n    }\n    return o;\n}\n"
    hdr = hdr.replace("REGS_SLOT", "b0" if stage == "vs" else "b1")
    return hdr + macros + io + entry + loop

if __name__ == "__main__":
    spec = rdna2dis.load_spec(sys.argv[1])
    code = open(sys.argv[2], "rb").read()
    try:
        h = translate(spec, code, sys.argv[3], int(sys.argv[5]) if len(sys.argv) > 5 else 16)
    except Unsupported as e:
        print("UNSUPPORTED:", e); sys.exit(2)
    open(sys.argv[4], "w", newline="\n").write(h)
    print("wrote", sys.argv[4])
