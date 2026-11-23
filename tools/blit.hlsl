// Present blit: scales a cached game render target onto the window's back buffer.
// build: dxc -T vs_6_6 -E vs -Fo bin/blit_vs.cso tools/blit.hlsl ; dxc -T ps_6_6 -E ps -Fo bin/blit_ps.cso tools/blit.hlsl
cbuffer Blit : register(b1) { uint g_srv; uint3 g_pad; };     // b1 = the pixel-stage constant slot of the translated root signature
SamplerState g_lin : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {                     // one triangle covering the screen
    V o; float2 p = float2((id << 1) & 2, id & 2);
    o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = p; return o;
}
float4 ps(V i) : SV_Target0 {
    Texture2D<float4> t = ResourceDescriptorHeap[g_srv];
    return float4(t.SampleLevel(g_lin, i.uv, 0).rgb, 1);
}
