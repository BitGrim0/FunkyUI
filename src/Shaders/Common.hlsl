// Shared by Shape.hlsl and Glyph.hlsl: constants, clip rects, the atlas and color helpers.
// Colors are sRGB with straight alpha; the shaders output premultiplied sRGB. The constants mirror
// ShaderConstants in src/Renderer.cpp.

StructuredBuffer<float4> Clips : register(t1); // (minX, minY, maxX, maxY), DIPs; [0] = no clipping
Texture2D<float4> Atlas : register(t2);        // glyph atlas (R8G8: coverage, distance field) or an image
SamplerState LinearClamp : register(s0);

cbuffer Constants : register(b0)
{
    float2 ViewportSize;    // DIPs
    float DpiScale;         // physical pixels per DIP
    uint FirstInstance;     // SV_InstanceID does not include the draw's start instance
    float2 AtlasTexelSize;  // 1 / atlas size: glyph coordinates are in texels
    float Opacity;          // the batch's layer opacity (panel fade / Opacity), multiplies the output
};

// Corner of the instance's quad for a triangle strip vertex: (0, 0), (1, 0), (0, 1), (1, 1).
float2 QuadCorner(uint vertexId)
{
    return float2(vertexId & 1, vertexId >> 1);
}

float4 ToClipSpace(float2 dip)
{
    return float4(dip / ViewportSize * float2(2, -2) + float2(-1, 1), 0, 1);
}

// p = pixel center in DIPs, the same value in every instance covering the pixel.
float2 PixelCenter(float4 position)
{
    return position.xy / DpiScale;
}

bool IsClipped(float2 p, float4 clip)
{
    return any(p < clip.xy) || any(p >= clip.zw);
}

float4 UnpackColor(uint c)
{
    return float4(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, c >> 24) / 255.0;
}

float4 Premultiply(float4 c)
{
    return float4(c.rgb * c.a, c.a);
}
