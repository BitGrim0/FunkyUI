// One vertex shader and one pixel shader for every shape. Each instance is a screen-aligned quad;
// the pixel shader evaluates the shape's signed distance (DIPs, negative inside) and turns it into
// coverage in physical pixels. Colors are sRGB with straight alpha; output is premultiplied sRGB.
// struct Shape and the constants below mirror Funky::GpuShape and friends in src/Internal.h.

struct Shape
{
    float4 Bounds;      // quad (minX, minY, maxX, maxY), DIPs
    float4 P0;
    float4 P1;
    float4 P2;          // gradient start (xy) and end (zw)
    uint Fill0;
    uint Fill1;
    uint Stroke;
    uint Glow;
    float StrokeWidth;
    float GlowRadius;
    float Softness;
    uint TypeFlags;
};

static const uint ShapeRoundRect = 0;
static const uint ShapePolyline = 1;
static const uint ShapeArc = 2;
static const uint ShapeTriangle = 3;
static const uint ShapeGlyph = 4;
static const uint ShapeImage = 5;

static const uint ShapeTypeMask = 0xFF;
static const uint FlagGradient = 1u << 8;
static const uint FlagEdgeAB = 1u << 9; // BC and CA follow
static const uint ShapeClipShift = 16;

static const float Pi = 3.14159265;
static const float FarOutside = 1e6;    // distance of pixels cut off by a hard triangle edge
static const float GlowFalloff = 4;     // exp(-4) ~ 2%: the glow has faded out at GlowRadius
static const float GlowTail = 0.0183156; // exp(-GlowFalloff)

StructuredBuffer<Shape> Shapes : register(t0);
StructuredBuffer<float4> Clips : register(t1); // (minX, minY, maxX, maxY), DIPs; [0] = no clipping
Texture2D<float4> Atlas : register(t2);        // glyph atlas (R8G8: coverage, distance field) or an image
StructuredBuffer<float2> Points : register(t3); // polyline vertices, DIPs
SamplerState LinearClamp : register(s0);

cbuffer Constants : register(b0)
{
    float2 ViewportSize;    // DIPs
    float DpiScale;         // physical pixels per DIP
    uint FirstShape;        // SV_InstanceID does not include the draw's start instance
    float2 AtlasTexelSize;  // 1 / atlas size: glyph coordinates are in texels
};

struct Varyings
{
    float4 Position : SV_Position;
    nointerpolation uint Index : TEXCOORD0;
};

// ----------------------------------------------------------------------------------------------
// Vertex shader
// ----------------------------------------------------------------------------------------------

Varyings VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    uint index = FirstShape + instanceId;
    float4 bounds = Shapes[index].Bounds;

    // Triangle strip corners: (min, min), (max, min), (min, max), (max, max).
    float2 dip = lerp(bounds.xy, bounds.zw, float2(vertexId & 1, vertexId >> 1));

    Varyings output;
    output.Position = float4(dip / ViewportSize * float2(2, -2) + float2(-1, 1), 0, 1);
    output.Index = index;
    return output;
}

// ----------------------------------------------------------------------------------------------
// Signed distance functions (DIPs, negative inside)
// ----------------------------------------------------------------------------------------------

float Cross(float2 a, float2 b)
{
    return a.x * b.y - a.y * b.x;
}

// Rectangle with a radius per corner: radii = (TL, TR, BR, BL), y down.
float SdRoundRect(float2 p, float4 rect, float4 radii)
{
    float2 halfSize = (rect.zw - rect.xy) * 0.5;
    float2 q = p - (rect.xy + rect.zw) * 0.5;
    float2 side = q.x < 0 ? radii.xw : radii.yz; // (top, bottom) radius of the left / right half
    float r = clamp(q.y < 0 ? side.x : side.y, 0, min(halfSize.x, halfSize.y));
    float2 e = abs(q) - halfSize + r;
    return min(max(e.x, e.y), 0) + length(max(e, 0)) - r;
}

// Capsule around the segment a-b.
float SdSegment(float2 p, float2 a, float2 b, float halfThickness)
{
    float2 pa = p - a;
    float2 ba = b - a;
    float h = saturate(dot(pa, ba) / max(dot(ba, ba), 1e-8));
    return length(pa - ba * h) - halfThickness;
}

// Stroke through Points[first .. first + count): the nearest segment's capsule, so joints are round.
float SdPolyline(float2 p, uint first, uint count, float halfThickness)
{
    float d = FarOutside;
    for (uint i = first + 1; i < first + count; ++i)
        d = min(d, SdSegment(p, Points[i - 1], Points[i], halfThickness));
    return d;
}

// Ring sector with round caps. Angles in radians, clockwise from +X in y-down space;
// the sweep may be negative, |sweep| >= 2 pi is a full ring.
float SdArc(float2 p, float2 center, float radius, float halfThickness, float start, float sweep)
{
    float2 q = p - center;
    float halfAperture = abs(sweep) * 0.5;
    float d = abs(length(q) - radius); // distance to the full ring
    if (halfAperture < Pi)
    {
        // Rotate the middle of the arc onto +Y and fold the symmetric halves together.
        float2 mid;
        sincos(start + sweep * 0.5, mid.y, mid.x);
        q = float2(abs(Cross(q, mid)), dot(q, mid));

        // Beyond the end direction the nearest point is the cap center.
        float2 end;
        sincos(halfAperture, end.x, end.y);
        if (Cross(q, end) > 0)
            d = length(q - end * radius);
    }
    return d - halfThickness;
}

// Triangle whose edges are antialiased only if their FlagEdge* bit is set. The other edges are
// internal seams of a triangle fan: pixels beyond them are simply outside, so neighbouring
// triangles meet without a visible seam.
float SdTriangle(float2 p, float2 a, float2 b, float2 c, uint flags)
{
    float2 vertices[3] = { a, b, c };
    float orientation = Cross(b - a, c - a) < 0 ? -1 : 1;
    float inside = -FarOutside; // max edge-line distance over the AA edges (exact inside)
    float outside = FarOutside; // min edge-segment distance over the AA edges (exact outside)
    bool cut = false;           // beyond a hard edge

    [unroll]
    for (uint i = 0; i < 3; ++i)
    {
        float2 u = vertices[i];
        float2 v = vertices[(i + 1) % 3];
        float2 e = v - u;
        float2 w = p - u;

        if (flags & (FlagEdgeAB << i))
        {
            float beyond = -orientation * Cross(e, w); // > 0 outside the edge, scaled by |e|
            float lengthSq = max(dot(e, e), 1e-8);
            inside = max(inside, beyond * rsqrt(lengthSq));
            outside = min(outside, length(w - e * saturate(dot(w, e) / lengthSq)));
        }
        else
        {
            // The neighbour sharing this edge walks it the other way. Both evaluate the side of p
            // from the same endpoint order without reassociation (precise), so their values are
            // exact negatives. Pixels exactly on the edge belong to the triangle whose outward
            // normal points left (or up): every pixel is drawn exactly once.
            bool flip = v.x < u.x || (v.x == u.x && v.y < u.y);
            float2 from = flip ? v : u;
            precise float side = Cross((flip ? u : v) - from, p - from);
            float beyond = -orientation * (flip ? -side : side);
            float2 normal = orientation * float2(e.y, -e.x);
            if (beyond > 0 || (beyond == 0 && (normal.x > 0 || (normal.x == 0 && normal.y > 0))))
                cut = true;
        }
    }
    return cut ? FarOutside : (inside > 0 ? outside : inside);
}

float SignedDistance(Shape s, float2 p, uint type)
{
    switch (type)
    {
    case ShapePolyline:
        return SdPolyline(p, (uint)s.P0.x, (uint)s.P0.y, s.P0.z);
    case ShapeArc:
        return SdArc(p, s.P0.xy, s.P0.z, s.P0.w, s.P1.x, s.P1.y);
    case ShapeTriangle:
        return SdTriangle(p, s.P0.xy, s.P0.zw, s.P1.xy, s.TypeFlags);
    case ShapeImage:
        return SdRoundRect(p, s.Bounds, s.P1);
    default:
        return SdRoundRect(p, s.P0, s.P1);
    }
}

// ----------------------------------------------------------------------------------------------
// Color
// ----------------------------------------------------------------------------------------------

float4 UnpackColor(uint c)
{
    return float4(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, c >> 24) / 255.0;
}

float3 SrgbToLinear(float3 c)
{
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float3 LinearToSrgb(float3 c)
{
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(abs(c), 1 / 2.4) - 0.055;
}

float4 Premultiply(float4 c)
{
    return float4(c.rgb * c.a, c.a);
}

// Fill color at p: Fill0, or the gradient Fill0 -> Fill1 along P2.xy -> P2.zw.
float4 FillColor(Shape s, float2 p)
{
    float4 color = UnpackColor(s.Fill0);
    if (s.TypeFlags & FlagGradient)
    {
        float4 to = UnpackColor(s.Fill1);
        float2 axis = s.P2.zw - s.P2.xy;
        float t = saturate(dot(p - s.P2.xy, axis) / max(dot(axis, axis), 1e-8));

        // Blend premultiplied linear colors: no muddy midtones, and fading to transparent does not darken.
        float4 mixed = lerp(float4(SrgbToLinear(color.rgb) * color.a, color.a), float4(SrgbToLinear(to.rgb) * to.a, to.a), t);
        color = float4(LinearToSrgb(mixed.rgb / max(mixed.a, 1e-6)), mixed.a);
    }
    return color;
}

// Atlas / image coordinates: Bounds maps linearly onto the rect P0 (glyphs: texels, images: UV).
float2 BoundsUv(Shape s, float2 p)
{
    return lerp(s.P0.xy, s.P0.zw, (p - s.Bounds.xy) / (s.Bounds.zw - s.Bounds.xy));
}

// Abramowitz & Stegun 7.1.27, max error 5e-4.
float Erf(float x)
{
    float a = abs(x);
    float t = 1 + (0.278393 + (0.230389 + (0.000972 + 0.078108 * a) * a) * a) * a;
    t *= t;
    float e = 1 - 1 / (t * t);
    return x < 0 ? -e : e;
}

// ----------------------------------------------------------------------------------------------
// Pixel shader
// ----------------------------------------------------------------------------------------------

float4 PSMain(Varyings input) : SV_Target
{
    Shape s = Shapes[input.Index];
    // The pixel center, not an interpolated position: bit-identical in every instance covering the
    // pixel, which the triangle fan's seam test relies on.
    float2 p = input.Position.xy / DpiScale;

    uint clipIndex = s.TypeFlags >> ShapeClipShift;
    if (clipIndex != 0)
    {
        float4 rect = Clips[clipIndex];
        if (any(p < rect.xy) || any(p >= rect.zw))
            discard;
    }

    uint type = s.TypeFlags & ShapeTypeMask;
    if (type == ShapeGlyph)
        return Premultiply(FillColor(s, p)) * Atlas.SampleLevel(LinearClamp, BoundsUv(s, p) * AtlasTexelSize, 0).r;

    float d = SignedDistance(s, p, type);

    // Blurred silhouette (shadow): the edge of a Gaussian-blurred half-plane. Sigma never drops
    // below ~0.4 px, where the curve matches the slope of plain antialiasing.
    if (s.Softness > 0)
    {
        float sigma = max(s.Softness * DpiScale, 0.4);
        return Premultiply(UnpackColor(s.Fill0)) * (0.5 - 0.5 * Erf(d * DpiScale / (sigma * sqrt(2))));
    }

    float4 fill = FillColor(s, p);
    if (type == ShapeImage) // stage 3 stub: the image tinted by the fill, masked by a rounded rect
        fill *= Atlas.SampleLevel(LinearClamp, BoundsUv(s, p), 0);

    // Area coverage of the shape and of its interior inside the stroke band. The band shows the
    // stroke over the fill; without a stroke both coverages are equal.
    float coverage = saturate(0.5 - d * DpiScale);
    float inner = saturate(0.5 - (d + s.StrokeWidth) * DpiScale);
    float4 fillColor = Premultiply(fill);
    float4 strokeColor = Premultiply(UnpackColor(s.Stroke));
    float4 color = fillColor * inner + (strokeColor + fillColor * (1 - strokeColor.a)) * (coverage - inner);

    // Glow fills the area outside the shape, under the body; shifted so it reaches exactly zero
    // at GlowRadius (the quad edge).
    if (s.GlowRadius > 0)
    {
        float4 glow = UnpackColor(s.Glow);
        float x = max(d, 0) / s.GlowRadius;
        glow.a *= saturate((exp(-GlowFalloff * x * x) - GlowTail) / (1 - GlowTail));
        color += Premultiply(glow) * (1 - coverage);
    }
    return color;
}
