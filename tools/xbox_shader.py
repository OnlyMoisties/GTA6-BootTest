# Unpack an Xbox "DXBC shell" shader (DXIL part holds NOBC + XDAT/XBCH zlib -> FSSM) and extract the RDNA2 code.
# usage: xbox_shader.py <spec.xml> <shader.vs|.ps> <out.bin>   (prints the debug name and code range)
import sys, os, struct, zlib, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rdna2dis

def fssm_from_container(d):
    n = struct.unpack_from("<I", d, 0x1C)[0]
    for o in struct.unpack_from("<%dI" % n, d, 0x20):
        if d[o:o+4] == b"DXIL":
            body = d[o+8: o+8+struct.unpack_from("<I", d, o+4)[0]]
            x = body.find(b"XBCH")
            if x < 0: return None
            return zlib.decompressobj(15).decompress(body[x + 24:])
    return None

def code_range(spec, f):
    ends = [i for i in range(0, len(f) - 3, 4) if f[i:i+4] == b"\x00\x00\x81\xbf"]
    for end in ends:
        best = None
        for start in range(end - 4, 0, -4):
            pc, ok = start, True
            while pc < end:
                nm, n, _ = rdna2dis.decode(spec, f, pc)
                if nm is None or nm.startswith("ENC_"): ok = False; break
                pc += n
            if ok and pc == end: best = start
            elif best is not None and start < best - 64: break
        if best is not None: return best, end + 4
    return None

if __name__ == "__main__":
    spec = rdna2dis.load_spec(sys.argv[1])
    f = fssm_from_container(open(sys.argv[2], "rb").read())
    if f is None: print("no XBCH/FSSM payload"); sys.exit(1)
    names = [s.decode() for s in re.findall(rb"[A-Za-z_][A-Za-z0-9_]{6,}", f)]
    r = code_range(spec, f)
    print("name:", names[:3], "fssm:", len(f), "bytes; code:", "0x%X-0x%X" % r if r else None)
    if r: open(sys.argv[3], "wb").write(f[r[0]:r[1]])
