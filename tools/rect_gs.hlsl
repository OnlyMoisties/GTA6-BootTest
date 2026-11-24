// Rect-list expansion for translated draws (AMD primitive type 17: three vertices per axis-aligned rectangle).
// The vertex opposite the longest edge is the corner between the two given edges; the missing corner is
// a + b - corner, for the position and for every attribute (attributes are raw 32-bit values, treated as floats).
// Build: dxc -T gs_6_6 -E main rect_gs.hlsl -Fo rect_gs.cso
struct V { float4 pos : SV_Position; uint4 a[16] : TEXCOORD0; };

V lerp4(V a, V b, V c) {             // a + b - c
    V r; r.pos = a.pos + b.pos - c.pos;
    [unroll] for (uint i = 0; i < 16; i++) r.a[i] = asuint(asfloat(a.a[i]) + asfloat(b.a[i]) - asfloat(c.a[i]));
    return r;
}
float d2(float4 p, float4 q) { float2 d = p.xy / p.w - q.xy / q.w; return dot(d, d); }

[maxvertexcount(6)]
void main(triangle V v[3], inout TriangleStream<V> s) {
    float e01 = d2(v[0].pos, v[1].pos), e12 = d2(v[1].pos, v[2].pos), e20 = d2(v[2].pos, v[0].pos);
    uint c = (e12 >= e01 && e12 >= e20) ? 0u : ((e20 >= e01) ? 1u : 2u);   // corner = vertex opposite the longest edge
    uint a = (c + 1u) % 3u, b = (c + 2u) % 3u;
    V d = lerp4(v[a], v[b], v[c]);
    s.Append(v[0]); s.Append(v[1]); s.Append(v[2]); s.RestartStrip();
    s.Append(v[a]); s.Append(d); s.Append(v[b]); s.RestartStrip();
}
