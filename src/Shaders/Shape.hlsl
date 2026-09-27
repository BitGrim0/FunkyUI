// One vertex shader and one pixel shader for every shape. Each instance is a screen-aligned quad;
// the pixel shader evaluates the shape's signed distance (DIPs, negative inside) and turns it into
// coverage in physical pixels. struct Shape and the constants below mirror Funky::GpuShape and
// friends in src/Internal.h.

#include "Common.hlsl"

struct Shape
{
    float4 Bounds;      // quad (minX, minY, maxX, maxY), DIPs
    float4 P0;
    float4 P1;
    float4 P2;          // gradient start (xy) and end (zw); FlagShadow: shadow offset (xy), sigma (z)
    uint Fill0;
    uint Fill1;         // gradient end; FlagShadow: shadow color
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
static const uint ShapeImage = 4;

static const uint ShapeTypeMask = 0xFF;
static const uint FlagGradient = 1u << 8;
static const uint FlagEdgeAB = 1u << 9; // BC and CA follow
static const uint FlagShadow = 1u << 12;
static const uint ShapeClipShift = 16;

static const float Pi = 3.14159265;
static const float FarOutside = 1e6;    // distance of pixels cut off by a hard triangle edge
static const float GlowFalloff = 4;     // exp(-4) ~ 2%: the glow has faded out at GlowRadius
static const float GlowTail = 0.0183156; // exp(-GlowFalloff)

StructuredBuffer<Shape> Shapes : register(t0);
StructuredBuffer<float2> Points : register(t3); // polyline vertices, DIPs

struct Varyings
{
    float4 Position : SV_Position;
    nointerpolation uint Index : TEXCOORD0;
    nointerpolation float4 Clip : TEXCOORD1;         // DIPs
    nointerpolation float4 Interior : TEXCOORD2;     // fast path area: (center, core half size), see InInterior
    nointerpolation float InteriorRadius : TEXCOORD3;
    nointerpolation float4 Fill : TEXCOORD4;         // fast path color: premultiplied, shadow under it, x Opacity;
                                                     // gradient: its start, premultiplied linear (no fast path)
    nointerpolation float4 GradientEnd : TEXCOORD5;  // premultiplied linear
};

// ----------------------------------------------------------------------------------------------
// Vertex shader
// ----------------------------------------------------------------------------------------------

static const float4 NoInterior = float4(0, 0, -1, -1);

// The pixels of a round rect (corner radii <= radius) at distance < -inset: the rect inset by inset, with
// radius - inset corners. As a (min, max) box and its corner radius.
void InsetRoundRect(float4 rect, float radius, float inset, out float4 box, out float r)
{
    box = rect + float4(inset, inset, -inset, -inset);
    r = max(radius - inset, 0);
}

// The round rect as (center, core half size): the core box grown by r. Empty (and r = 0) when the box is too
// small for r. A circle's core is 0 give or take rounding: a hundredth of a DIP is well within the pixel of
// antialiasing the area keeps from the edge.
float4 InteriorArea(float4 box, inout float r)
{
    float2 core = max((box.zw - box.xy) * 0.5 - r, 0);
    bool empty = any((box.zw - box.xy) * 0.5 - r < -0.01);
    r = empty ? 0 : r;
    return empty ? NoInterior : float4((box.xy + box.zw) * 0.5, core);
}

// How far inside a shadow's edge its coverage is full: 3.5 sigma leaves 2.3e-4 (the Erf approximation
// itself is 5e-4); a hard shadow is antialiased like any edge.
float ShadowInset(float sigma)
{
    return sigma > 0 ? 3.5 * max(sigma, 0.4 / DpiScale) : 1 / DpiScale;
}

float3 SrgbToLinear(float3 c)
{
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float4 PremultipliedLinear(uint packed)
{
    float4 c = UnpackColor(packed);
    return float4(SrgbToLinear(c.rgb) * c.a, c.a);
}

Varyings VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    uint index = FirstInstance + instanceId;
    Shape s = Shapes[index];
    uint type = s.TypeFlags & ShapeTypeMask;

    Varyings output;
    output.Position = ToClipSpace(lerp(s.Bounds.xy, s.Bounds.zw, QuadCorner(vertexId)));
    output.Index = index;
    output.Clip = Clips[s.TypeFlags >> ShapeClipShift];

    // Where the pixels of a solid round rect are surely the fill alone: inside by the stroke and a pixel of
    // antialiasing (glow is outside). A blurred silhouette: where its coverage is full. A shadow under the
    // body: also where the shadow's coverage is full.
    float4 fill = Premultiply(UnpackColor(s.Fill0)) * Opacity;
    output.Interior = NoInterior;
    output.InteriorRadius = 0;
    if (type == ShapeRoundRect && !(s.TypeFlags & FlagGradient))
    {
        float radius = max(max(s.P1.x, s.P1.y), max(s.P1.z, s.P1.w));
        float4 box;
        float r;
        InsetRoundRect(s.P0, radius, s.Softness > 0 ? ShadowInset(s.Softness) : s.StrokeWidth + 1 / DpiScale, box, r);
        if (s.TypeFlags & FlagShadow)
        {
            // The intersection holds the round rect of both boxes with the larger radius.
            float4 shadowBox;
            float shadowRadius;
            InsetRoundRect(s.P0 + s.P2.xyxy, radius, ShadowInset(s.P2.z), shadowBox, shadowRadius);
            box = float4(max(box.xy, shadowBox.xy), min(box.zw, shadowBox.zw));
            r = max(r, shadowRadius);
            fill += Premultiply(UnpackColor(s.Fill1)) * Opacity * (1 - fill.a); // as PSMain composites it
        }
        output.Interior = InteriorArea(box, r);
        output.InteriorRadius = r;
    }
    output.Fill = fill;
    output.GradientEnd = 0;
    [branch]
    if (s.TypeFlags & FlagGradient)
    {
        output.Fill = PremultipliedLinear(s.Fill0);
        output.GradientEnd = PremultipliedLinear(s.Fill1);
    }
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

float3 LinearToSrgb(float3 c)
{
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(abs(c), 1 / 2.4) - 0.055;
}

// Fill color at p (straight alpha): Fill0, or the gradient Fill0 -> Fill1 along P2.xy -> P2.zw. The
// gradient blends premultiplied linear colors (ends converted by the vertex shader): no muddy midtones,
// and fading to transparent does not darken.
float4 FillColor(Shape s, float2 p, Varyings input)
{
    float4 color = UnpackColor(s.Fill0);
    [branch]
    if (s.TypeFlags & FlagGradient)
    {
        float2 axis = s.P2.zw - s.P2.xy;
        float t = saturate(dot(p - s.P2.xy, axis) / max(dot(axis, axis), 1e-8));
        float4 mixed = lerp(input.Fill, input.GradientEnd, t);
        color = float4(LinearToSrgb(mixed.rgb / max(mixed.a, 1e-6)), mixed.a);
    }
    return color;
}

// Image coordinates: Bounds maps linearly onto the UV rect P0.
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

// Coverage of a shadow at distance d (DIPs) from its silhouette: the edge of a Gaussian-blurred half-plane.
// Sigma never drops below ~0.4 px, where the curve matches the slope of plain antialiasing; no sigma is a
// hard (antialiased) edge.
float ShadowCoverage(float d, float sigma)
{
    float coverage = saturate(0.5 - d * DpiScale);
    if (sigma > 0)
    {
        float pixels = max(sigma * DpiScale, 0.4);
        coverage = 0.5 - 0.5 * Erf(d * DpiScale / (pixels * sqrt(2)));
    }
    return coverage;
}

// Inside the round rect (center, core half size) grown by radius.
bool InInterior(float2 p, float4 interior, float radius)
{
    float2 q = abs(p - interior.xy) - interior.zw;
    float2 m = max(q, 0);
    return max(q.x, q.y) < 0 || dot(m, m) < radius * radius;
}

// ----------------------------------------------------------------------------------------------
// Pixel shader
// ----------------------------------------------------------------------------------------------

float4 PSMain(Varyings input) : SV_Target
{
    // The pixel center, not an interpolated position: bit-identical in every instance covering the
    // pixel, which the triangle fan's seam test relies on.
    float2 p = PixelCenter(input.Position);
    if (IsClipped(p, input.Clip))
        discard;

    // Fast path for the bulk of large fills and shadows: exactly what the rest returns there (full
    // coverage, no stroke band, no glow), without loading the shape or evaluating its distance.
    if (InInterior(p, input.Interior, input.InteriorRadius))
        return input.Fill;

    Shape s = Shapes[input.Index];
    uint type = s.TypeFlags & ShapeTypeMask;
    float d; // not ?: which evaluates both sides
    [branch]
    if (type == ShapeRoundRect) // the common case, ahead of the switch
        d = SdRoundRect(p, s.P0, s.P1);
    else
        d = SignedDistance(s, p, type);

    // A blurred silhouette alone (a shadow without a body): its color is the fast path's.
    if (s.Softness > 0)
        return input.Fill * ShadowCoverage(d, s.Softness);

    // Area coverage of the shape and of its interior inside the stroke band. The band shows the
    // stroke over the fill.
    float coverage = saturate(0.5 - d * DpiScale);
    float4 color = 0;
    [branch]
    if (coverage > 0)
    {
        float4 fill = FillColor(s, p, input);
        if (type == ShapeImage) // stage 3 stub: the image tinted by the fill, masked by a rounded rect
            fill *= Atlas.SampleLevel(LinearClamp, BoundsUv(s, p), 0);
        float4 fillColor = Premultiply(fill);
        color = fillColor * coverage;
        [branch]
        if (s.StrokeWidth > 0)
        {
            float inner = saturate(0.5 - (d + s.StrokeWidth) * DpiScale);
            float4 strokeColor = Premultiply(UnpackColor(s.Stroke));
            color = fillColor * inner + (strokeColor + fillColor * (1 - strokeColor.a)) * (coverage - inner);
        }
    }

    // Glow fills the area outside the shape, under the body; shifted so it reaches exactly zero
    // at GlowRadius (the quad edge).
    [branch]
    if (s.GlowRadius > 0 && coverage < 1)
    {
        float4 glow = UnpackColor(s.Glow);
        float x = max(d, 0) / s.GlowRadius;
        glow.a *= saturate((exp(-GlowFalloff * x * x) - GlowTail) / (1 - GlowTail));
        color += Premultiply(glow) * (1 - coverage);
    }
    color *= Opacity;

    // Shadow under everything above (premultiplied "over" is associative: the same as drawing it first).
    [branch]
    if (s.TypeFlags & FlagShadow)
    {
        float shadow = ShadowCoverage(SdRoundRect(p - s.P2.xy, s.P0, s.P1), s.P2.z);
        color += Premultiply(UnpackColor(s.Fill1)) * (shadow * Opacity * (1 - color.a));
    }
    return color;
}
