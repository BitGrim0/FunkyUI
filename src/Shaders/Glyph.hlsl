// Glyphs: a textured quad per glyph, coverage from the atlas's R channel times the glyph's color.
// Kept apart from Shape.hlsl so text pays for neither the 96-byte shape nor the SDF shader.
// struct Glyph mirrors Funky::GpuGlyph in src/Internal.h.

#include "Common.hlsl"

struct Glyph
{
    float4 Rect;        // quad (minX, minY, maxX, maxY), DIPs
    uint Texel0;        // atlas texel of the top-left corner: x | y << 16
    uint Texel1;        // bottom-right corner (exclusive)
    uint Color;         // sRGB, straight alpha
    uint Clip;          // index into Clips
};

StructuredBuffer<Glyph> Glyphs : register(t4);

struct Varyings
{
    float4 Position : SV_Position;
    float2 Uv : TEXCOORD0;                      // atlas UV
    nointerpolation float4 Color : TEXCOORD1;   // premultiplied, x Opacity
    nointerpolation float4 Clip : TEXCOORD2;    // DIPs
};

float2 UnpackTexel(uint texel)
{
    return float2(texel & 0xFFFF, texel >> 16);
}

Varyings VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    Glyph g = Glyphs[FirstInstance + instanceId];
    float2 corner = QuadCorner(vertexId);

    Varyings output;
    output.Position = ToClipSpace(lerp(g.Rect.xy, g.Rect.zw, corner));
    output.Uv = lerp(UnpackTexel(g.Texel0), UnpackTexel(g.Texel1), corner) * AtlasTexelSize;
    output.Color = Premultiply(UnpackColor(g.Color)) * Opacity;
    output.Clip = Clips[g.Clip];
    return output;
}

float4 PSMain(Varyings input) : SV_Target
{
    if (IsClipped(PixelCenter(input.Position), input.Clip))
        discard;
    return input.Color * Atlas.SampleLevel(LinearClamp, input.Uv, 0).r;
}
