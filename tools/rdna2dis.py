# RDNA2 (gfx10.3) disassembler driven by AMD's machine-readable ISA spec (amdgpu_isa_rdna2.xml).
# usage:
#   rdna2dis.py <spec.xml> <shader.bin> [...]          print disassembly
#   rdna2dis.py <spec.xml> --stats <dir>                 instruction histogram over all *.bin in dir
import sys, os, struct, pickle, collections
import xml.etree.ElementTree as ET

BASE_ENCODINGS = ["ENC_SOP1", "ENC_SOPC", "ENC_SOPP", "ENC_SOPK", "ENC_SOP2", "ENC_SMEM", "ENC_VOP1", "ENC_VOPC", "ENC_VOP2",
                  "ENC_VINTRP", "ENC_VOP3", "ENC_VOP3P", "ENC_DS", "ENC_MUBUF", "ENC_MTBUF", "ENC_MIMG", "ENC_EXP", "ENC_FLAT",
                  "ENC_FLAT_GLBL", "ENC_FLAT_SCRATCH"]
# literal-only variants: opcodes listed only here always carry a trailing 32-bit literal
LITERAL_VARIANTS = {"SOP1_INST_LITERAL": "ENC_SOP1", "SOP2_INST_LITERAL": "ENC_SOP2", "SOPC_INST_LITERAL": "ENC_SOPC",
                    "SOPK_INST_LITERAL": "ENC_SOPK", "VOP1_INST_LITERAL": "ENC_VOP1", "VOP2_INST_LITERAL": "ENC_VOP2",
                    "VOPC_INST_LITERAL": "ENC_VOPC", "VOP3_INST_LITERAL": "ENC_VOP3", "VOP3P_INST_LITERAL": "ENC_VOP3P"}

def load_spec(path):
    cache = path + ".pickle"
    if os.path.exists(cache) and os.path.getmtime(cache) > os.path.getmtime(path):
        return pickle.load(open(cache, "rb"))
    isa = ET.parse(path).getroot().find("ISA")
    encs = {}
    variant_ids = collections.defaultdict(set)
    for e in isa.find("Encodings"):
        name = e.findtext("EncodingName")
        if name in LITERAL_VARIANTS:                      # their identifiers (low dword) also identify the base encoding
            variant_ids[LITERAL_VARIANTS[name]] |= {int(x.text, 2) & 0xFFFFFFFF for x in e.find("EncodingIdentifiers")}
        if name in ("VOP3_SDST_ENC", "VOP3_SDST_ENC_INST_LITERAL"):
            variant_ids["ENC_VOP3"] |= {int(x.text, 2) & 0xFFFFFFFF for x in e.find("EncodingIdentifiers")}
        if name not in BASE_ENCODINGS: continue
        bits = int(e.findtext("BitCount"))
        mask = int(e.find("EncodingIdentifierMask").text, 2)
        ids = {int(x.text, 2) for x in e.find("EncodingIdentifiers")}
        fields = {}
        for f in e.find("MicrocodeFormat").find("BitMap"):
            rngs = [(int(r.findtext("BitOffset")), int(r.findtext("BitCount"))) for r in f.find("BitLayout")]
            fields[f.findtext("FieldName")] = rngs
        encs[name] = dict(bits=bits, mask=mask, ids=ids, fields=fields)
    ops = collections.defaultdict(dict)        # encoding -> opcode -> (name, [operand field names], always_literal)
    litonly = []
    for i in isa.find("Instructions"):
        nm = i.findtext("InstructionName")
        for ie in i.find("InstructionEncodings"):
            en = ie.findtext("EncodingName")
            opc = int(ie.findtext("Opcode"))
            operands = [o.findtext("FieldName") for o in ie.find("Operands")] if ie.find("Operands") is not None else []
            if en in encs: ops[en].setdefault(opc, (nm, operands, False))
            elif en in LITERAL_VARIANTS: litonly.append((LITERAL_VARIANTS[en], opc, nm, operands))
    for base, opc, nm, operands in litonly:
        if opc not in ops[base]: ops[base][opc] = (nm, operands, True)
    # VOP3 forms with a scalar carry/condition destination (VOP3b) live in their own encodings: same size, no literal
    for i in isa.find("Instructions"):
        nm = i.findtext("InstructionName")
        for ie in i.find("InstructionEncodings"):
            if ie.findtext("EncodingName") in ("VOP3_SDST_ENC",):
                opc = int(ie.findtext("Opcode"))
                if opc not in ops["ENC_VOP3"]: ops["ENC_VOP3"][opc] = (nm, [o.findtext("FieldName") for o in ie.find("Operands")] if ie.find("Operands") is not None else [], False)
    for base, ids in variant_ids.items():
        m = encs[base]["mask"] & 0xFFFFFFFF
        encs[base]["ids"] = set(encs[base]["ids"]) | {i & m for i in ids}
    spec = dict(encs=encs, ops=dict(ops), version=2)
    pickle.dump(spec, open(cache, "wb"))
    return spec

def field(v, rngs):
    out, shift = 0, 0
    for off, cnt in rngs:
        out |= ((v >> off) & ((1 << cnt) - 1)) << shift
        shift += cnt
    return out

def src_name(x):
    if x <= 105: return "s%d" % x
    names = {106: "vcc_lo", 107: "vcc_hi", 124: "m0", 125: "null", 126: "exec_lo", 127: "exec_hi", 235: "src_shared_base",
             236: "src_shared_limit", 237: "src_private_base", 238: "src_private_limit", 240: "0.5", 241: "-0.5", 242: "1.0",
             243: "-1.0", 244: "2.0", 245: "-2.0", 246: "4.0", 247: "-4.0", 248: "1/(2pi)", 249: "sdwa", 250: "dpp16", 251: "vccz",
             252: "execz", 253: "scc", 254: "lds_direct", 255: "lit", 233: "dpp8", 234: "dpp8fi"}
    if x in names: return names[x]
    if 128 <= x <= 192: return str(x - 128)
    if 193 <= x <= 208: return str(-(x - 192))
    if 256 <= x <= 511: return "v%d" % (x - 256)
    return "?%d" % x

_CLS = None
def classify(spec, d0):
    # precomputed: (mask, ids32, name) sorted by mask popcount, most specific first; result cached per top 9 bits + full fallback
    global _CLS
    if _CLS is None:
        _CLS = sorted(((e["mask"] & 0xFFFFFFFF, frozenset(i & 0xFFFFFFFF for i in e["ids"]), n) for n, e in spec["encs"].items()),
                      key=lambda t: -bin(t[0]).count("1"))
    for m, ids, n in _CLS:
        if m and (d0 & m) in ids: return n
    return None

_CC = {}
def decode(spec, code, pc):
    d0 = struct.unpack_from("<I", code, pc)[0]
    en = _CC.get(d0 >> 18, -1)
    if en == -1:
        en = classify(spec, d0)
        # encoding identifiers never use bits below 18 in RDNA2, so the top 14 bits are a safe cache key
        _CC[d0 >> 18] = en
    if not en: return None, 4, "?? 0x%08X" % d0
    e = spec["encs"][en]
    size = e["bits"] // 8
    v = d0 if size == 4 else d0 | (struct.unpack_from("<I", code, pc + 4)[0] << 32 if pc + 8 <= len(code) else 0)
    f = {k: field(v, r) for k, r in e["fields"].items()}
    opc = f.get("OP", 0)
    nm, operands, always_lit = spec["ops"].get(en, {}).get(opc, ("%s_op%d" % (en, opc), [], False))
    extra = 0
    srcs = [k for k in ("SSRC0", "SSRC1", "SRC0", "SRC1", "SRC2") if k in f]
    if always_lit or any(f[k] == 255 for k in srcs): extra = 4         # 32-bit literal follows
    if en in ("ENC_VOP1", "ENC_VOP2", "ENC_VOPC") and f.get("SRC0") in (249, 250, 233, 234): extra = 4   # SDWA / DPP word
    if en == "ENC_MIMG" and f.get("NSA"): extra = 4 * f["NSA"]         # non-sequential address dwords
    parts = []
    for k in operands:
        if k not in f: continue
        val = f[k]
        if k in ("SSRC0", "SSRC1", "SRC0", "SRC1", "SRC2"): parts.append(src_name(val))
        elif k in ("VSRC1",): parts.append("v%d" % val)
        elif k in ("SDST",): parts.append(src_name(val))
        elif k in ("VDST", "VADDR", "VDATA", "DATA0", "DATA1", "ADDR"): parts.append("v%d" % val)
        elif k in ("SBASE",): parts.append("s[%d:%d]" % (val * 2, val * 2 + 1))
        elif k in ("SDATA",): parts.append("s%d" % val)
        elif k in ("SRSRC", "SSAMP"): parts.append("s[%d:]" % (val * 4))
        else: parts.append("%s=%d" % (k.lower(), val))
    # fields the operand list omits but that matter for memory access
    for k in ("OFFSET", "OFFSET0", "OFFSET1", "SOFFSET", "DMASK", "DIM", "OFFEN", "IDXEN", "GLC", "FORMAT"):
        if k in f and k not in operands and f[k]: parts.append("%s=%s" % (k.lower(), hex(f[k]) if k.startswith("OFFSET") else f[k]))
    lit = ""
    if extra and pc + size + 4 <= len(code): lit = "  ; extra=0x%08X" % struct.unpack_from("<I", code, pc + size)[0]
    return nm, size + extra, "%-28s %s%s" % (nm.lower(), ", ".join(parts), lit)

def disasm(spec, code, out=True):
    pc, names = 0, []
    while pc + 4 <= len(code):
        nm, n, text = decode(spec, code, pc)
        if out: print("%06X: %s" % (pc, text))
        names.append(nm or "??")
        pc += n
        if nm == "S_ENDPGM":
            # code may be followed by padding / data; stop at the first terminator past which only zeros or s_code_end follow
            rest = code[pc:pc + 64]
            if not rest.strip(b"\x00") or rest[:4] == b"\x00\x00\x9f\xbf": break
    return names

if __name__ == "__main__":
    spec = load_spec(sys.argv[1])
    if sys.argv[2] == "--stats":
        hist, files, unknown, ends = collections.Counter(), 0, 0, 0
        for fn in sorted(os.listdir(sys.argv[3])):
            if not fn.endswith(".bin"): continue
            names = disasm(spec, open(os.path.join(sys.argv[3], fn), "rb").read(), False)
            files += 1; hist.update(names); unknown += names.count("??"); ends += ("S_ENDPGM" in names)
        print("shaders: %d   with s_endpgm: %d   undecodable dwords: %d   distinct instructions: %d" % (files, ends, unknown, len(hist)))
        for nm, c in hist.most_common(): print("%8d  %s" % (c, nm))
    else:
        for p in sys.argv[2:]:
            print("==", p); disasm(spec, open(p, "rb").read())
