# Coverage survey for the translator over captured main-renderer shaders (*.vsc / *.psc).
# usage: xlat_survey.py <spec.xml> <shaders dir> [sample count] [dxc.exe out_dir compile_count]
#   translates an evenly spaced sample, prints what blocks the rest; with dxc also compiles the first N translations
import sys, os, collections, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rdna2dis, rdna2hlsl

spec = rdna2dis.load_spec(sys.argv[1]); src = sys.argv[2]
want = int(sys.argv[3]) if len(sys.argv) > 3 else 600
dxc = sys.argv[4] if len(sys.argv) > 4 else None
outd = sys.argv[5] if len(sys.argv) > 5 else None
ncomp = int(sys.argv[6]) if len(sys.argv) > 6 else 0
files = sorted(f for f in os.listdir(src) if f.endswith((".vsc", ".psc")))
fails, ok, n = collections.Counter(), collections.Counter(), collections.Counter()
comp, cerr = 0, collections.Counter()
if outd: os.makedirs(outd, exist_ok=True)
for fn in files[::max(1, len(files) // want)]:
    try: raw = open(os.path.join(src, fn), "rb").read()
    except OSError: continue
    e = raw.find(b"\x00\x00\x81\xbf"); code = raw[:e + 4] if e >= 0 else raw
    st = rdna2hlsl.detect_stage(spec, code); n[st] += 1
    try: hl = rdna2hlsl.translate(spec, code, st, 16); ok[st] += 1
    except rdna2hlsl.Unsupported as x: fails[str(x)] += 1; continue
    if dxc and comp < ncomp:
        comp += 1
        hp = os.path.join(outd, "%s.%s.hlsl" % (fn[:-4], st)); open(hp, "w", newline="\n").write(hl)
        r = subprocess.run([dxc, "-T", "%s_6_6" % st, "-E", "main", "-O1", "-Fo", hp[:-5] + ".cso", hp], capture_output=True, text=True)
        if r.returncode:
            msg = next((l for l in r.stderr.splitlines() if "error" in l), r.stderr[:200])
            cerr[msg.split("error:", 1)[-1].strip()[:140]] += 1
        else: cerr["OK"] += 1
print("sampled", dict(n), "translated", dict(ok))
for k, v in fails.most_common(30): print("  %4d  %s" % (v, k))
if dxc:
    print("dxc results over %d:" % comp)
    for k, v in cerr.most_common(15): print("  %4d  %s" % (v, k))
