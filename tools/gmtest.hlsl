// GPU memory pool self-test: read game addresses through the page table (same code as translated shaders) into u0.
// build: dxc -T cs_6_6 -E main -Fo bin/gmtest.cso tools/gmtest.hlsl
cbuffer Args : register(b0) { uint4 g_addr[8]; };            // 16 addresses (lo, hi pairs)
ByteAddressBuffer g_ptab : register(t0);
RWByteAddressBuffer g_out : register(u0);
uint memload(uint64_t a) {
    uint slot = g_ptab.Load(((uint)(a >> 32) & 0xFFFFu) * 4u); if (slot == 0u) return 0xDEAD0001u;
    uint e = g_ptab.Load((65536u + (slot - 1u) * 65536u + ((uint)(a >> 16) & 0xFFFFu)) * 4u); if (e == 0u) return 0xDEAD0002u;
    ByteAddressBuffer sec = ResourceDescriptorHeap[NonUniformResourceIndex((e >> 24) - 1u)];
    return sec.Load(((e & 0xFFFFFFu) << 16) | ((uint)a & 0xFFFCu));
}
[numthreads(16, 1, 1)]
void main(uint i : SV_DispatchThreadID) {
    uint2 p = uint2(0, 0);
    [unroll] for (uint k = 0; k < 16u; k++) if (k == i) p = (k & 1u) ? g_addr[k >> 1].zw : g_addr[k >> 1].xy;   // constant indices only
    g_out.Store(i * 4u, memload((uint64_t)p.x | ((uint64_t)p.y << 32)));
}
