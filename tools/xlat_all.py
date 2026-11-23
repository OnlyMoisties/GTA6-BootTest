# Translate every captured Xbox VS/PS shell (logs/shaders/*.vs|*.ps) to HLSL and compile with dxc.
# Output: logs/xlat/<hash>.<vs|ps>.hlsl and .cso ; prints a summary of what failed and why.
# usage: xlat_all.py <spec.xml> <shaders dir> <out dir> <dxc.exe> [limit]   (env XLAT_EXT=vs,ps limits file types, XLAT_ONLY=hash,hash limits shaders)
import sys, os, subprocess, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rdna2dis, xbox_shader, rdna2hlsl

spec = rdna2dis.load_spec(sys.argv[1])
src, out, dxc = sys.argv[2], sys.argv[3], sys.argv[4]
os.makedirs(out, exist_ok=True)
ok, fails = 0, collections.Counter()
limit = int(sys.argv[5]) if len(sys.argv) > 5 else 1 << 30
done = 0
for fn in sorted(os.listdir(src)):
    ext = fn.rsplit(".", 1)[-1]
    if ext not in ("vs", "ps", "vsc", "psc"): continue
    if os.environ.get("XLAT_EXT") and ext not in os.environ["XLAT_EXT"].split(","): continue
    if os.environ.get("XLAT_ONLY") and fn.rsplit(".", 1)[0] not in os.environ["XLAT_ONLY"].split(","): continue
    stage = ext[:2]
    h = fn.rsplit(".", 1)[0]
    if done >= limit: break
    done += 1
    raw = open(os.path.join(src, fn), "rb").read()
    if ext in ("vsc", "psc"):                       # raw RDNA2 code (main renderer): ends at the first s_endpgm
        e = raw.find(b"\x00\x00\x81\xbf"); code = raw[:e + 4] if e >= 0 else raw
        stage = rdna2hlsl.detect_stage(spec, code)
    else:
        try: f = xbox_shader.fssm_from_container(raw)
        except Exception: f = None
        if f is None: fails["no FSSM payload"] += 1; continue
        r = xbox_shader.code_range(spec, f)
        if not r: fails["code not found"] += 1; continue
        code = f[r[0]:r[1]]
    try: hl = rdna2hlsl.translate(spec, code, stage, 32)
    except rdna2hlsl.Unsupported as e: fails["unsupported: %s" % e] += 1; continue
    hp = os.path.join(out, "%s.%s.hlsl" % (h, stage)); open(hp, "w", newline="\n").write(hl)
    res = subprocess.run([dxc, "-T", "%s_6_6" % stage, "-E", "main", "-Fo", os.path.join(out, "%s.%s.cso" % (h, stage)), hp], capture_output=True, text=True)
    if res.returncode: fails["dxc error: " + (res.stderr.strip().splitlines() or ["?"])[0][:120]] += 1
    else: ok += 1
print("compiled: %d" % ok)
for k, v in fails.most_common(): print("  %4d  %s" % (v, k))
