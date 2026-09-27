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

        // Rectangles and circles: the body (fill, inner stroke, glow) and its shadow. Under a solid body the shadow
        // is in the body's instance (the pixel shader composites it underneath); otherwise it is an instance of its own.
        void EmitRoundRect(UiImpl& ui, Rect rect, CornerRadius radius, const ShapeStyle& style)
        {
            if (!(rect.Width > 0 && rect.Height > 0))
                return;
            float limit = Min(rect.Width, rect.Height) * 0.5f;
            float radii[4] = { Clamp(radius.TopLeft, 0.0f, limit), Clamp(radius.TopRight, 0.0f, limit),
                               Clamp(radius.BottomRight, 0.0f, limit), Clamp(radius.BottomLeft, 0.0f, limit) };
            float aa = 1 / ui.Scale;

            const Shadow& shadow = style.Shadow;
            bool hasShadow = shadow.Color.A != 0 && (shadow.Blur > 0 || shadow.Offset.X != 0 || shadow.Offset.Y != 0);
            bool stroke = !style.Stroke.IsNone() && style.StrokeThickness > 0;
            bool hasBody = !style.Fill.IsNone() || stroke || HasGlow(style.Glow);
            bool merged = hasShadow && hasBody && style.Fill.Type != Brush::Kind::LinearGradient; // P2 holds the gradient
            Rect shadowRect = { rect.X + shadow.Offset.X, rect.Y + shadow.Offset.Y, rect.Width, rect.Height };
            float sigma = Max(shadow.Blur, 0.0f) * 0.5f; // Gaussian
            float shadowPad = 3 * sigma + aa;

            if (hasShadow && !merged)
            {
                if (GpuShape* s = ui.EmitShapes(1))
                {
                    SetRoundRect(*s, shadowRect, radii);
                    s->Fill0 = PackColor(shadow.Color);
                    s->Softness = sigma;
                    SetBounds(*s, shadowRect.Left(), shadowRect.Top(), shadowRect.Right(), shadowRect.Bottom(), shadowPad);
                }
            }
            if (!hasBody)
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
            if (merged)
            {
                s->TypeFlags |= FlagShadow;
                s->Fill1 = PackColor(shadow.Color);
                s->P2[0] = shadow.Offset.X;
                s->P2[1] = shadow.Offset.Y;
                s->P2[2] = sigma;
                s->Bounds[0] = Min(s->Bounds[0], shadowRect.Left() - shadowPad);
                s->Bounds[1] = Min(s->Bounds[1], shadowRect.Top() - shadowPad);
                s->Bounds[2] = Max(s->Bounds[2], shadowRect.Right() + shadowPad);
                s->Bounds[3] = Max(s->Bounds[3], shadowRect.Bottom() + shadowPad);
            }
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
            MemCopy(stored, points, sizeof(Vec2) * count);
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
        // Wrapped: an angle that grows without bound (a spinner) keeps its precision on the CPU and the GPU.
        s->P1[0] = (startAngle - Floor(startAngle * (1.0f / 360)) * 360) * (Pi / 180);
        s->P1[1] = sweepAngle * (Pi / 180);
        ApplyPaint(*s, MakePaint(style.Stroke, { center.X - radius, center.Y - radius, 2 * radius, 2 * radius }));

        // Tight box of the centerline: its ends and the axis directions it passes; the caps and the stroke are
        // within half of it.
        float lo[2] = { center.X + radius, center.Y }, hi[2] = { center.X + radius, center.Y }; // any point on it
        float first = s->P1[0], last = s->P1[0] + s->P1[1];
        if (last < first)
        {
            float t = first;
            first = last;
            last = t;
        }
        auto include = [&](float x, float y)
        {
            lo[0] = Min(lo[0], x);
            lo[1] = Min(lo[1], y);
            hi[0] = Max(hi[0], x);
            hi[1] = Max(hi[1], y);
        };
        if (last - first >= 2 * Pi)
        {
            include(center.X - radius, center.Y - radius);
            include(center.X + radius, center.Y + radius);
        }
        else
        {
            lo[0] = hi[0] = center.X + radius * Cos(first);
            lo[1] = hi[1] = center.Y + radius * Sin(first);
            include(center.X + radius * Cos(last), center.Y + radius * Sin(last));
            // Directions k * 90 degrees (+X, +Y, -X, -Y) within [first, last].
            constexpr float Quarter = Pi * 0.5f;
            static constexpr float AxisX[4] = { 1, 0, -1, 0 };
            static constexpr float AxisY[4] = { 0, 1, 0, -1 };
            float k = Ceil(first / Quarter);
            for (uint32_t i = 0; i < 4 && k * Quarter <= last; ++i, k += 1)
            {
                int32_t axis = int32_t(k) & 3; // two's complement: the right quadrant for negative k too
                include(center.X + radius * AxisX[axis], center.Y + radius * AxisY[axis]);
            }
        }
        float pad = half + ApplyGlow(*s, style.Glow) + 1 / ui.Scale;
        SetBounds(*s, lo[0], lo[1], hi[0], hi[1], pad);
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
        // Simple text in one color goes straight into glyph instances; the rest is laid out, then drawn.
        if (style.Foreground.Type != Brush::Kind::LinearGradient)
        {
            GpuGlyph* glyphs = ui.ReserveGlyphs(uint32_t(text.size()));
            if (!glyphs)
                return;
            Vec2 origin = { Round(position.X * ui.Scale) / ui.Scale, Round(position.Y * ui.Scale) / ui.Scale };
            uint32_t count = 0;
            ClipRect ink;
            const TextLayout* shaped = ui.Text.LayoutInto(text, style, ui.Scale, ui.Frame,
                                                          { glyphs, origin, PackColor(style.Foreground.From), ui.CurrentClip }, count, ink);
            if (shaped)
                ui.DrawLayout(position, *shaped, style);
            else
                ui.CommitGlyphs(count, ink);
            return;
        }
        ui.DrawLayout(position, ui.LayoutText(text, style), style);
    }

    void UiImpl::DrawLayout(Vec2 position, const TextLayout& layout, const TextStyle& style)
    {
        const uint32_t count = layout.GlyphCount;
        const Brush& brush = style.Foreground;
        if (count == 0 || brush.IsNone())
            return;
        GpuGlyph* glyphs = ReserveGlyphs(count);
        if (!glyphs)
            return;

        // Glyph quads are pixel-snapped relative to the origin, so the origin is snapped too (as in DrawString).
        Vec2 origin = { Round(position.X * Scale) / Scale, Round(position.Y * Scale) / Scale };
        Paint paint = MakePaint(brush, { origin.X, origin.Y, layout.Size.X, layout.Size.Y });
        const GlyphQuad* quads = layout.Glyphs;
        uint32_t clip = CurrentClip;
        for (uint32_t i = 0; i < count; ++i)
        {
            const GlyphQuad& q = quads[i];
            GpuGlyph& g = glyphs[i];
            g.Rect[0] = origin.X + q.Rect[0];
            g.Rect[1] = origin.Y + q.Rect[1];
            g.Rect[2] = origin.X + q.Rect[2];
            g.Rect[3] = origin.Y + q.Rect[3];
            g.Texel0 = q.Texel0;
            g.Texel1 = q.Texel1;
            g.Color = paint.Fill0;
            g.Clip = clip;
        }

        if (paint.Flags & FlagGradient)
        {
            // A glyph has one color: the gradient at its center, blended like the shape shader does
            // (premultiplied, linear space). The ends are converted once.
            float from[4], to[4];
            ToPremultipliedLinear(brush.From, from);
            ToPremultipliedLinear(brush.To, to);
            Vec2 start = { paint.Axis[0], paint.Axis[1] };
            Vec2 axis = Vec2{ paint.Axis[2], paint.Axis[3] } - start;
            float axisScale = 1 / Max(Dot(axis, axis), 1e-8f);
            for (uint32_t i = 0; i < count; ++i)
            {
                GpuGlyph& g = glyphs[i];
                Vec2 center = { (g.Rect[0] + g.Rect[2]) * 0.5f, (g.Rect[1] + g.Rect[3]) * 0.5f };
                float t = Saturate(Dot(center - start, axis) * axisScale);
                float mixed[4];
                for (int k = 0; k < 4; ++k)
                    mixed[k] = Lerp(from[k], to[k], t);
                g.Color = PackColor(FromPremultipliedLinear(mixed));
            }
        }

        // BuildFrame keeps shapes drawn later over this text above it.
        const ClipRect& ink = layout.Ink;
        CommitGlyphs(count, { origin.X + ink.MinX, origin.Y + ink.MinY, origin.X + ink.MaxX, origin.Y + ink.MaxY });
    }
}
