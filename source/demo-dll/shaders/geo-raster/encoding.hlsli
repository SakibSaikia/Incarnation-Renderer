#ifndef __ENCODING_HLSLI_
#define __ENCODING_HLSLI_

#define PRIM_TRIANGLE_BIT_COUNT (20)
#define MESHLET_TRIANGLE_BIT_COUNT (7)
#define TRIANGLE_MASK(X) ((1 << X) - 1)

uint EncodePrimitiveVisibility(uint primitiveId, uint triangleId)
{
    return primitiveId << PRIM_TRIANGLE_BIT_COUNT | triangleId;
}

void DecodePrimitiveVisibility(uint data, out uint primitiveId, out uint triangleId)
{
    primitiveId = data >> PRIM_TRIANGLE_BIT_COUNT;
    triangleId = data & TRIANGLE_MASK(PRIM_TRIANGLE_BIT_COUNT);
}

uint EncodeMeshletVisibility(uint meshletId, uint triangleId)
{
    return meshletId << MESHLET_TRIANGLE_BIT_COUNT | triangleId;
}

void DecodeMeshletVisibility(uint data, out uint meshletId, out uint triangleId)
{
    meshletId = data >> MESHLET_TRIANGLE_BIT_COUNT;
    triangleId = data & TRIANGLE_MASK(MESHLET_TRIANGLE_BIT_COUNT);
}


// Octahedron normal encoding/decoding based on 
// https://knarkowicz.wordpress.com/2014/04/16/octahedron-normal-vector-encoding/
float2 OctWrap(float2 v)
{
    return (1.0 - abs(v.yx)) * select(v.xy >= 0.0, 1.0, -1.0);
}

float2 OctEncode(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    n.xy = n.z >= 0.0 ? n.xy : OctWrap(n.xy);
    n.xy = n.xy * 0.5 + 0.5;
    return n.xy;
}

float3 OctDecode(float2 f)
{
    f = f * 2.0 - 1.0;

    // https://twitter.com/Stubbesaurus/status/937994790553227264
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = saturate(-n.z);
    n.xy += select(n.xy >= 0.0, -t, t);
    return normalize(n);
}

#endif // __ENCODING_HLSLI_