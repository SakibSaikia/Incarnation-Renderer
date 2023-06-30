#ifndef __ENCODING_HLSLI_
#define __ENCODING_HLSLI_

#define NUM_TRIANGLE_BITS (20)
#define TRIANGLE_MASK ((1 << NUM_TRIANGLE_BITS) - 1)

uint EncodeVisibilityBuffer(uint objectId, uint triangleId)
{
	return objectId << NUM_TRIANGLE_BITS | triangleId;
}

void DecodeVisibilityBuffer(uint data, out uint objectId, out uint triangleId)
{
	objectId = data >> NUM_TRIANGLE_BITS;
	triangleId = data & TRIANGLE_MASK;
}


// Octahedron normal encoding/decoding based on 
// https://knarkowicz.wordpress.com/2014/04/16/octahedron-normal-vector-encoding/
float2 OctWrap(float2 v)
{
    return (1.0 - abs(v.yx)) * (v.xy >= 0.0 ? 1.0 : -1.0);
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
    n.xy += n.xy >= 0.0 ? -t : t;
    return normalize(n);
}

#endif // __ENCODING_HLSLI_