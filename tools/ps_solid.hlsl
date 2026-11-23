// Debug pixel shader for translated draws (GTA6_XLAT_SOLID=1): colours each triangle by its primitive id so the
// game's geometry is visible without textures. Reads only SV_Position, so it links with any translated VS.
// build: dxc -T ps_6_6 -E main -Fo bin/ps_solid.cso tools/ps_solid.hlsl
float4 main(float4 pos : SV_Position, uint prim : SV_PrimitiveID) : SV_Target0 {
    uint h = prim * 2654435761u;
    float3 c = float3((h >> 8) & 255u, (h >> 16) & 255u, (h >> 24) & 255u) / 255.0f;
    return float4(0.25f + 0.75f * c, 1.0f);
}
