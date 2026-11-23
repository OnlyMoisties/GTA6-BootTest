# Translate + compile the shader pairs a run asked for (logs/xlat_needed.txt, written by realgpu when a pipeline has
# no translation yet). Writes <hash>.vs.cso / <hash>.ps.cso into the out dir; skips ones already there.
# usage: xlat_needed.py <spec.xml> <logs dir> <out dir> <dxc.exe> [max pairs]
import sys, os, subprocess, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rdna2dis, rdna2hlsl, xbox_shader

spec = rdna2dis.load_spec(sys.argv[1]); logs, out, dxc = sys.argv[2], sys.argv[3], sys.argv[4]
limit = int(sys.argv[5]) if len(sys.argv) > 5 else 1 << 30
os.makedirs(out, exist_ok=True)
src = os.path.join(logs, "shaders")
pairs = []
for line in open(os.path.join(logs, "xlat_needed.txt")):
    p = line.split()
    if len(p) == 2 and tuple(p) not in pairs: pairs.append(tuple(p))
def code_for(h):
    for ext in ("vsc", "psc"):                       # main-renderer raw code
        f = os.path.join(src, "%s.%s" % (h, ext))
        if os.path.exists(f):
            raw = open(f, "rb").read(); e = raw.find(b"\x00\x00\x81\xbf"); return raw[:e + 4] if e >= 0 else raw
    for ext in ("vs", "ps"):                         # exe-embedded containers
        f = os.path.join(src, "%s.%s" % (h, ext))
        if os.path.exists(f):
            fs = xbox_shader.fssm_from_container(open(f, "rb").read())
            r = xbox_shader.code_range(spec, fs) if fs else None
            if r: return fs[r[0]:r[1]]
    return None
res = collections.Counter(); done = 0
for vh, ph in pairs[:limit]:
    for h, want in ((vh, "vs"), (ph, "ps")):
        cso = os.path.join(out, "%s.%s.cso" % (h, want))
        if os.path.exists(cso): res["already there"] += 1; continue
        code = code_for(h)
        if code is None: res["%s code not captured" % want] += 1; continue
        st = rdna2hlsl.detect_stage(spec, code)
        if st != want: res["%s: code looks like %s" % (want, st)] += 1
        try: hl = rdna2hlsl.translate(spec, code, want, 32)
        except rdna2hlsl.Unsupported as e: res["unsupported: %s" % e] += 1; continue
        hp = cso[:-4] + ".hlsl"; open(hp, "w", newline="\n").write(hl)
        r = subprocess.run([dxc, "-T", "%s_6_6" % want, "-E", "main", "-O1", "-Fo", cso, hp], capture_output=True, text=True)
        if r.returncode: res["dxc: " + next((l for l in r.stderr.splitlines() if "error" in l), "?")[:120]] += 1
        else: res["compiled"] += 1
    done += 1
print("pairs: %d listed, %d processed" % (len(pairs), done))
for k, v in res.most_common(): print("  %5d  %s" % (v, k))
