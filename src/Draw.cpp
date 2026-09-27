// Draw — primitives → GpuShape instances (one screen-aligned quad per shape, SDF in the pixel shader) and
// text → GpuGlyph instances (one quad per glyph). Shape bounds cover everything the shader may draw:
// antialiasing (one physical pixel), glow, shadow blur.

#include "Internal.h"

namespace Funky
{
    namespace
    {
        constexpr uint32_t MaxBezierSegments = 64;
        constexpr uint32_t MaxPolylineSegments = 64; // per instance (the pixel shader loops over them): a Bézier is one instance
        constexpr float BezierTolerancePx = 0.2f; // max distance between the curve and its segments

        // Fill of a shape: a solid color or a linear gradient across the shape's bounding box.
        struct Paint
        {
            uint32_t Fill0 = 0;
            uint32_t Fill1 = 0;
            uint32_t Flags = 0;
            float Axis[4] = {}; // gradient start (xy) and end (zw)
        };

        // CSS-style gradient line: through the box center, long enough that the corners get the end colors.
        Paint MakePaint(const Brush& brush, Rect box)
        {
            Paint paint;
            if (brush.IsNone())
                return paint;
            paint.Fill0 = PackColor(brush.From);
            if (brush.Type != Brush::Kind::LinearGradient)
                return paint;

            float radians = brush.Angle * (Pi / 180);
            Vec2 direction = { Cos(radians), Sin(radians) };
            float half = (Abs(box.Width * direction.X) + Abs(box.Height * direction.Y)) * 0.5f;
            Vec2 from = box.Center() - direction * half;
            Vec2 to = box.Center() + direction * half;
            paint.Fill1 = PackColor(brush.To);
            paint.Flags = FlagGradient;
            paint.Axis[0] = from.X;
            paint.Axis[1] = from.Y;
            paint.Axis[2] = to.X;
            paint.Axis[3] = to.Y;
            return paint;
        }

        void ApplyPaint(GpuShape& s, const Paint& paint)
        {
            s.Fill0 = paint.Fill0;
            s.Fill1 = paint.Fill1;
            s.TypeFlags |= paint.Flags;
            MemCopy(s.P2, paint.Axis, sizeof(s.P2));
        }

        bool HasGlow(const Glow& glow)
        {
            return glow.Radius > 0 && glow.Color.A != 0;
        }

        // Returns how far the glow reaches outside the shape.
        float ApplyGlow(GpuShape& s, const Glow& glow)
        {
            if (!HasGlow(glow))
                return 0;
            s.Glow = PackColor(glow.Color);
            s.GlowRadius = glow.Radius;
            return glow.Radius;
        }

        void SetBounds(GpuShape& s, float minX, float minY, float maxX, float maxY, float pad)
        {
            s.Bounds[0] = minX - pad;
            s.Bounds[1] = minY - pad;
            s.Bounds[2] = maxX + pad;
            s.Bounds[3] = maxY + pad;
        }

        void SetRoundRect(GpuShape& s, Rect r, const float* radii)
        {
            s.TypeFlags |= ShapeRoundRect;
            s.P0[0] = r.Left();
            s.P0[1] = r.Top();
            s.P0[2] = r.Right();
            s.P0[3] = r.Bottom();
            MemCopy(s.P1, radii, sizeof(s.P1));
        }

        float Cross(Vec2 a, Vec2 b)
        {
            return a.X * b.Y - a.Y * b.X;
        }

        float Dot(Vec2 a, Vec2 b)
        {
            return a.X * b.X + a.Y * b.Y;
        }

        float Length(Vec2 v)
        {
            return Sqrt(Dot(v, v));
        }

        Rect BoundingBox(const Vec2* points, uint32_t count)
        {
            Vec2 lo = points[0];
            Vec2 hi = points[0];
            for (uint32_t i = 1; i < count; ++i)
            {
                lo = { Min(lo.X, points[i].X), Min(lo.Y, points[i].Y) };
                hi = { Max(hi.X, points[i].X), Max(hi.Y, points[i].Y) };
            }
            return { lo.X, lo.Y, hi.X - lo.X, hi.Y - lo.Y };
        }

        // Rectangles and circles: an optional shadow instance, then the body (fill, inner stroke, glow).
        void EmitRoundRect(UiImpl& ui, Rect rect, CornerRadius radius, const ShapeStyle& style)
        {
            if (!(rect.Width > 0 && rect.Height > 0))
                return;
            float limit = Min(rect.Width, rect.Height) * 0.5f;
            float radii[4] = { Clamp(radius.TopLeft, 0.0f, limit), Clamp(radius.TopRight, 0.0f, limit),
                               Clamp(radius.BottomRight, 0.0f, limit), Clamp(radius.BottomLeft, 0.0f, limit) };
            float aa = 1 / ui.Scale;

            const Shadow& shadow = style.Shadow;
            if (shadow.Color.A != 0 && (shadow.Blur > 0 || shadow.Offset.X != 0 || shadow.Offset.Y != 0))
            {
                if (GpuShape* s = ui.EmitShapes(1))
                {
                    Rect r = { rect.X + shadow.Offset.X, rect.Y + shadow.Offset.Y, rect.Width, rect.Height };
                    SetRoundRect(*s, r, radii);
                    s->Fill0 = PackColor(shadow.Color);
                    s->Softness = Max(shadow.Blur, 0.0f) * 0.5f; // Gaussian sigma
                    SetBounds(*s, r.Left(), r.Top(), r.Right(), r.Bottom(), 3 * s->Softness + aa);
                }
            }

            bool stroke = !style.Stroke.IsNone() && style.StrokeThickness > 0;
            if (style.Fill.IsNone() && !stroke && !HasGlow(style.Glow))
                return;
            GpuShape* s = ui.EmitShapes(1);
            if (!s)
                return;
            SetRoundRect(*s, rect, radii);
            ApplyPaint(*s, MakePaint(style.Fill, rect));
            if (stroke)
            {
                s->Stroke = PackColor(style.Stroke.From); // strokes are solid: a gradient uses its start color
                s->StrokeWidth = style.StrokeThickness;
            }
            float glow = ApplyGlow(*s, style.Glow);
            SetBounds(*s, rect.Left(), rect.Top(), rect.Right(), rect.Bottom(), glow + aa);
        }

        // The points go to UiImpl::Points; the shader takes the distance to the nearest segment of the
        // instance, so body, antialiasing and glow are composited once per pixel and joints are round.
        // Longer polylines take several instances, which overlap only around their shared points.
        // The gradient spans the whole polyline.
        void EmitPolyline(UiImpl& ui, const Vec2* points, uint32_t count, const LineStyle& style)
        {
            if (count < 2 || !(style.Thickness > 0) || (style.Stroke.IsNone() && !HasGlow(style.Glow)))
                return;
            uint32_t first = ui.Points.Count;
            Vec2* stored = ui.Points.Append(count);
            if (!stored)
                return;
            MemCopy(stored, points, sizeof(Vec2) * count); // before EmitShapes, which hashes them (see FoldEmitted)
            uint32_t instances = (count - 2) / MaxPolylineSegments + 1;
            GpuShape* shapes = ui.EmitShapes(instances);
            if (!shapes)
                return;

            Paint paint = MakePaint(style.Stroke, BoundingBox(points, count));
            float half = style.Thickness * 0.5f;
            float aa = 1 / ui.Scale;
            for (uint32_t i = 0; i < instances; ++i)
            {
                uint32_t start = i * MaxPolylineSegments;
                uint32_t n = Min(count - start, MaxPolylineSegments + 1);
                GpuShape& s = shapes[i];
                s.TypeFlags |= ShapePolyline;
                s.P0[0] = float(first + start);
                s.P0[1] = float(n);
                s.P0[2] = half;
                ApplyPaint(s, paint);
                float glow = ApplyGlow(s, style.Glow);
                Rect box = BoundingBox(points + start, n);
                SetBounds(s, box.Left(), box.Top(), box.Right(), box.Bottom(), half + glow + aa);
            }
        }

        // edges: FlagEdge* bits of the outer (antialiased) edges.
        void SetTriangle(GpuShape& s, Vec2 a, Vec2 b, Vec2 c, uint32_t edges, const Paint& paint, const Glow& glow, float aa)
        {
            s.TypeFlags |= ShapeTriangle | edges;
            s.P0[0] = a.X;
            s.P0[1] = a.Y;
            s.P0[2] = b.X;
            s.P0[3] = b.Y;
            s.P1[0] = c.X;
            s.P1[1] = c.Y;
            ApplyPaint(s, paint);
            float pad = ApplyGlow(s, glow) + aa;
            SetBounds(s, Min(Min(a.X, b.X), c.X), Min(Min(a.Y, b.Y), c.Y), Max(Max(a.X, b.X), c.X), Max(Max(a.Y, b.Y), c.Y), pad);
        }
    }

    void Ui::DrawRect(Rect rect, const ShapeStyle& style)
    {
        EmitRoundRect(*Impl(this), rect, style.CornerRadius, style);
    }

    void Ui::DrawCircle(Vec2 center, float radius, const ShapeStyle& style)
    {
        EmitRoundRect(*Impl(this), { center.X - radius, center.Y - radius, 2 * radius, 2 * radius }, radius, style);
    }

    void Ui::DrawArc(Vec2 center, float radius, float startAngle, float sweepAngle, const LineStyle& style)
    {
        UiImpl& ui = *Impl(this);
        if (!(radius > 0) || !(style.Thickness > 0) || (style.Stroke.IsNone() && !HasGlow(style.Glow)))
            return;
        GpuShape* s = ui.EmitShapes(1);
        if (!s)
            return;
        float half = style.Thickness * 0.5f;
        s->TypeFlags |= ShapeArc;
        s->P0[0] = center.X;
        s->P0[1] = center.Y;
        s->P0[2] = radius;
        s->P0[3] = half;
        s->P1[0] = startAngle * (Pi / 180);
        s->P1[1] = sweepAngle * (Pi / 180);
        ApplyPaint(*s, MakePaint(style.Stroke, { center.X - radius, center.Y - radius, 2 * radius, 2 * radius }));
        float pad = radius + half + ApplyGlow(*s, style.Glow) + 1 / ui.Scale;
        SetBounds(*s, center.X, center.Y, center.X, center.Y, pad);
    }

    void Ui::DrawLine(Vec2 from, Vec2 to, const LineStyle& style)
    {
        Vec2 points[2] = { from, to };
        EmitPolyline(*Impl(this), points, 2, style);
    }

    void Ui::DrawPolyline(std::span<const Vec2> points, const LineStyle& style)
    {
        EmitPolyline(*Impl(this), points.data(), uint32_t(points.size()), style);
    }

    void Ui::DrawBezier(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, const LineStyle& style)
    {
        UiImpl& ui = *Impl(this);
        // Wang's bound: n segments keep the curve within the tolerance of its chords. It grows
        // with the control polygon's second differences, so a straight curve needs one segment.
        float bend = Max(Length(p0 - p1 * 2 + p2), Length(p1 - p2 * 2 + p3)) * ui.Scale;
        float n = Ceil(Sqrt(0.75f * bend / BezierTolerancePx));
        uint32_t segments = Clamp(uint32_t(n), 1u, MaxBezierSegments);

        Vec2 points[MaxBezierSegments + 1];
        for (uint32_t i = 0; i <= segments; ++i)
        {
            float t = float(i) / float(segments);
            float u = 1 - t;
            points[i] = p0 * (u * u * u) + p1 * (3 * u * u * t) + p2 * (3 * u * t * t) + p3 * (t * t * t);
        }
        EmitPolyline(ui, points, segments + 1, style);
    }

    void Ui::DrawTriangle(Vec2 a, Vec2 b, Vec2 c, const FillStyle& style)
    {
        UiImpl& ui = *Impl(this);
        float area = Cross(b - a, c - a);
        if (area == 0 || (style.Fill.IsNone() && !HasGlow(style.Glow)))
            return;
        if (area < 0) // one winding for every triangle
        {
            Vec2 t = b;
            b = c;
            c = t;
        }
        GpuShape* s = ui.EmitShapes(1);
        if (!s)
            return;
        Vec2 points[3] = { a, b, c };
        SetTriangle(*s, a, b, c, FlagEdgeAB | FlagEdgeBC | FlagEdgeCA, MakePaint(style.Fill, BoundingBox(points, 3)), style.Glow, 1 / ui.Scale);
    }

    void Ui::DrawPolygon(std::span<const Vec2> convexPoints, const FillStyle& style)
    {
        UiImpl& ui = *Impl(this);
        const Vec2* points = convexPoints.data();
        uint32_t count = uint32_t(convexPoints.size());
        if (count < 3 || (style.Fill.IsNone() && !HasGlow(style.Glow)))
            return;

        float area = 0;
        for (uint32_t i = 0; i < count; ++i)
            area += Cross(points[i], points[(i + 1) % count]);
        if (area == 0)
            return;
        // Walk the points so that every fan triangle has the same winding as DrawTriangle.
        auto at = [&](uint32_t i) { return points[area > 0 ? i : count - 1 - i]; };

        GpuShape* shapes = ui.EmitShapes(count);
        if (!shapes)
            return;
        Vec2 center = {};
        for (uint32_t i = 0; i < count; ++i)
            center = center + points[i];
        center = center * (1 / float(count));
        Paint paint = MakePaint(style.Fill, BoundingBox(points, count));
        float aa = 1 / ui.Scale;
        // Fan from an interior point: the triangles split the plane into disjoint wedges, so every pixel
        // outside a vertex gets its glow and edge AA from one triangle (a fan from a vertex doubles them).
        for (uint32_t i = 0; i < count; ++i)
            SetTriangle(shapes[i], center, at(i), at((i + 1) % count), FlagEdgeBC, paint, style.Glow, aa);
    }

    void Ui::DrawString(Vec2 position, std::string_view text, const TextStyle& style)
    {
        UiImpl& ui = *Impl(this);
        if (text.empty() || style.Foreground.IsNone())
            return;
        const TextLayout* layout = ui.Text.Layout(text, style, ui.Scale, ui.Frame);
        const uint32_t count = layout->GlyphCount;
        if (count == 0)
            return;

        // Glyph quads are pixel-snapped relative to the origin, so the origin is snapped too.
        Vec2 origin = { Round(position.X * ui.Scale) / ui.Scale, Round(position.Y * ui.Scale) / ui.Scale };
        // The union of the quads (computed like their rects below): BuildFrame keeps shapes drawn later
        // over this text above it.
        Vec2 first = origin + Vec2{ layout->Glyphs[0].X, layout->Glyphs[0].Y };
        ClipRect bounds = { first.X, first.Y, first.X, first.Y };
        for (uint32_t i = 0; i < count; ++i)
        {
            const GlyphQuad& q = layout->Glyphs[i];
            float x = origin.X + q.X;
            float y = origin.Y + q.Y;
            bounds = { Min(bounds.MinX, x), Min(bounds.MinY, y), Max(bounds.MaxX, x + q.Width), Max(bounds.MaxY, y + q.Height) };
        }
        GpuGlyph* glyphs = ui.EmitGlyphs(count, bounds);
        if (!glyphs)
            return;

        // A glyph has one color: a gradient is sampled at each glyph's center, blended like the shape
        // shader does (premultiplied, linear space).
        const Brush& brush = style.Foreground;
        Paint paint = MakePaint(brush, { origin.X, origin.Y, layout->Size.X, layout->Size.Y });
        Vec2 from = { paint.Axis[0], paint.Axis[1] };
        Vec2 axis = Vec2{ paint.Axis[2], paint.Axis[3] } - from;
        float axisScale = 1 / Max(Dot(axis, axis), 1e-8f);
        for (uint32_t i = 0; i < count; ++i)
        {
            const GlyphQuad& q = layout->Glyphs[i];
            GpuGlyph& g = glyphs[i];
            g.Rect[0] = origin.X + q.X;
            g.Rect[1] = origin.Y + q.Y;
            g.Rect[2] = g.Rect[0] + q.Width;
            g.Rect[3] = g.Rect[1] + q.Height;
            // Atlas texels are whole numbers (the atlas is at most 65535 texels wide).
            g.Texel0 = uint32_t(q.U0 + 0.5f) | (uint32_t(q.V0 + 0.5f) << 16);
            g.Texel1 = uint32_t(q.U1 + 0.5f) | (uint32_t(q.V1 + 0.5f) << 16);
            g.Color = paint.Fill0;
            if (paint.Flags & FlagGradient)
            {
                Vec2 center = { (g.Rect[0] + g.Rect[2]) * 0.5f, (g.Rect[1] + g.Rect[3]) * 0.5f };
                g.Color = PackColor(Lerp(brush.From, brush.To, Saturate(Dot(center - from, axis) * axisScale)));
            }
            g.Clip = ui.CurrentClip;
        }
    }
}
