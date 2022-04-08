#ifndef __ENCODING_HLSLI_
#define __ENCODING_HLSLI_

#define NUM_TRIANGLE_BITS (17)
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

#endif // __ENCODING_HLSLI_