// Built-in controls: Label, Button, CheckBox, Slider.
// Built on the public API (Widget, Transition, Draw*) like a client's own control, except for text: a control
// lays its text out once, sizes itself by the layout and draws that same layout (MeasureText + DrawString
// would lay it out twice).

#include "Internal.h"

namespace Funky
{
    using namespace Literals;

    // Hand-tuned timings and derived-color amounts of the built-in look.
    constexpr Duration ColorDuration = 90_ms;       // background color changes on hover, press and check
    constexpr Duration GlowDuration = 140_ms;       // glow fading in and out
    constexpr Duration CheckDuration = 140_ms;      // check mark drawing in
    constexpr Duration ThumbDuration = 110_ms;      // slider thumb growing on hover

    constexpr float HoverLighten = 0.08f;           // hover = Background mixed toward white
    constexpr float PressedDarken = 0.12f;          // pressed = Background mixed toward black
    constexpr float DisabledOpacity = 0.45f;

    constexpr float CheckBoxSpacing = 8;            // between the box and the text
    constexpr float CheckMarkThickness = 0.12f;     // of BoxSize
    constexpr float CheckMarkSplit = 0.33f;         // the short stroke is ~1/3 of the mark's length

    constexpr float SliderDefaultHeight = 22;
    constexpr float SliderDefaultWidth = 160;
    constexpr float ThumbGrow = 1.2f;               // thumb radius multiplier on hover / drag
    constexpr Shadow ThumbShadow = { { 0, 1 }, 3, Rgba(0x00000070) };

    namespace
    {
        // Mix in sRGB space: perceptually even steps for hover/pressed shading. Alpha is kept.
        Color Mix(Color c, Color toward, float amount)
        {
            auto channel = [amount](uint8_t a, uint8_t b) { return uint8_t(Round(Lerp(float(a), float(b), amount))); };
            return { channel(c.R, toward.R), channel(c.G, toward.G), channel(c.B, toward.B), c.A };
        }

        Brush Mix(Brush b, Color toward, float amount)
        {
            b.From = Mix(b.From, toward, amount);
            b.To = Mix(b.To, toward, amount);
            return b;
        }

        Brush Fade(Brush b, float opacity)
        {
            b.From = WithOpacity(b.From, opacity);
            b.To = WithOpacity(b.To, opacity);
            return b;
        }

        Glow Fade(Glow g, float opacity)
        {
            g.Color = WithOpacity(g.Color, opacity);
            return g;
        }

        // Animates both gradient stops towards target. A "none" target fades to transparent
        // (its stops are transparent), so switching between none and a color animates too.
        Brush TransitionBrush(Ui& ui, const Widget& w, Key fromSlot, Key toSlot, const Brush& target, Duration duration)
        {
            Color from = ui.Transition(w, fromSlot, target.From, duration);
            Color to = ui.Transition(w, toSlot, target.To, duration);
            if (from.A == 0 && to.A == 0)
                return {};
            return from == to ? Brush(from) : Brush::Linear(from, to, target.Angle);
        }

        // Crisp 1-DIP borders at fractional DPI (DrawString snaps text itself).
        Vec2 SnapToPixel(const Ui& ui, Vec2 p)
        {
            float scale = ui.DpiScale();
            return { Round(p.X * scale) / scale, Round(p.Y * scale) / scale };
        }

        float DesiredSize(float explicitSize, float contentSize)
        {
            return explicitSize > 0 ? explicitSize : contentSize;
        }

        // An explicit size beats Stretch (as in WPF): the control keeps that size, centered in its slot.
        HorizontalAlignment ExplicitAlignment(HorizontalAlignment a, float explicitWidth)
        {
            return explicitWidth > 0 && a == HorizontalAlignment::Stretch ? HorizontalAlignment::Center : a;
        }

        VerticalAlignment ExplicitAlignment(VerticalAlignment a, float explicitHeight)
        {
            return explicitHeight > 0 && a == VerticalAlignment::Stretch ? VerticalAlignment::Center : a;
        }
    }

    void Ui::Label(std::string_view text, const LabelProps& props)
    {
        TextStyle style = {
            .Font = props.Font,
            .FontSize = props.FontSize,
            .FontWeight = props.FontWeight,
            .Italic = props.Italic,
            .Foreground = Fade(props.Foreground, props.Opacity),
            .Glow = Fade(props.Glow, props.Opacity),
            .Shadow = props.Shadow,
            .Outline = props.Outline,
        };
        style.Shadow.Color = WithOpacity(style.Shadow.Color, props.Opacity);
        style.Outline.Color = WithOpacity(style.Outline.Color, props.Opacity);

        UiImpl& ui = *Impl(this);
        const TextLayout& layout = ui.LayoutText(text, style);
        Vec2 size = layout.Size;
        Funky::Widget w = Widget(Key(), {
            .Width = DesiredSize(props.Width, size.X),
            .Height = DesiredSize(props.Height, size.Y),
            .IsEnabled = false,
            .Margin = props.Margin,
            .HorizontalAlignment = ExplicitAlignment(props.HorizontalAlignment, props.Width),
            .VerticalAlignment = ExplicitAlignment(props.VerticalAlignment, props.Height),
            .MinWidth = props.MinWidth,
            .MaxWidth = props.MaxWidth,
            .MinHeight = props.MinHeight,
            .MaxHeight = props.MaxHeight,
        });

        ui.DrawLayout({ w.Rect.X, w.Rect.CenterY() - size.Y * 0.5f }, layout, style);
    }

    bool Ui::Button(std::string_view text, const ButtonProps& props)
    {
        UiImpl& ui = *Impl(this);
        TextStyle style = { .Font = props.Font, .FontSize = props.FontSize, .FontWeight = props.FontWeight, .Foreground = props.Foreground };
        const TextLayout& layout = ui.LayoutText(text, style);
        Vec2 textSize = layout.Size;

        Funky::Widget w = Widget(props.Key.IsNone() ? Key::FromString(text) : props.Key, {
            .Width = DesiredSize(props.Width, textSize.X + props.Padding.Horizontal()),
            .Height = DesiredSize(props.Height, textSize.Y + props.Padding.Vertical()),
            .IsEnabled = props.IsEnabled,
            .Margin = props.Margin,
            .HorizontalAlignment = ExplicitAlignment(props.HorizontalAlignment, props.Width),
            .VerticalAlignment = ExplicitAlignment(props.VerticalAlignment, props.Height),
            .MinWidth = props.MinWidth,
            .MaxWidth = props.MaxWidth,
            .MinHeight = props.MinHeight,
            .MaxHeight = props.MaxHeight,
        });

        // Pressed look only while the pointer is still over the button (releasing outside cancels the click).
        Brush background = props.Background;
        if (w.Pressed && w.Hovered)
            background = props.PressedBackground.IsNone() ? Mix(props.Background, Colors::Black, PressedDarken) : props.PressedBackground;
        else if (w.Hovered || w.Pressed)
            background = props.HoverBackground.IsNone() ? Mix(props.Background, Colors::White, HoverLighten) : props.HoverBackground;
        background = TransitionBrush(*this, w, "bg.from", "bg.to", background, ColorDuration);

        float glowRadius = Transition(w, "glow", w.Hovered || w.Pressed ? props.Glow.Radius : 0.0f, GlowDuration);
        Glow glow = {};
        if (props.Glow.Radius > 0)
            glow = { WithOpacity(props.Glow.Color, glowRadius / props.Glow.Radius), glowRadius };

        float opacity = (w.IsEnabled ? 1.0f : DisabledOpacity) * props.Opacity;
        Shadow shadow = props.Shadow;
        shadow.Color = WithOpacity(shadow.Color, opacity);
        DrawRect(w.Rect, {
            .Fill = Fade(background, opacity),
            .Stroke = Fade(props.BorderBrush, opacity),
            .StrokeThickness = props.BorderThickness,
            .CornerRadius = props.CornerRadius,
            .Glow = glow,
            .Shadow = shadow,
        });

        style.Foreground = Fade(props.Foreground, opacity);
        ui.DrawLayout(w.Rect.Center() - textSize * 0.5f, layout, style);
        return w.Clicked;
    }

    bool Ui::CheckBox(std::string_view text, bool& value, const CheckBoxProps& props)
    {
        UiImpl& ui = *Impl(this);
        TextStyle style = { .Font = props.Font, .FontSize = props.FontSize, .FontWeight = props.FontWeight, .Foreground = props.Foreground };
        const TextLayout* layout = text.empty() ? nullptr : &ui.LayoutText(text, style);
        Vec2 textSize = layout ? layout->Size : Vec2();
        float textOffset = text.empty() ? 0.0f : CheckBoxSpacing;

        Funky::Widget w = Widget(props.Key.IsNone() ? Key::FromPointer(&value) : props.Key, {
            .Width = DesiredSize(props.Width, props.BoxSize + textOffset + textSize.X),
            .Height = DesiredSize(props.Height, Max(props.BoxSize, textSize.Y)),
            .IsEnabled = props.IsEnabled,
            .Margin = props.Margin,
            .HorizontalAlignment = ExplicitAlignment(props.HorizontalAlignment, props.Width),
            .VerticalAlignment = ExplicitAlignment(props.VerticalAlignment, props.Height),
            .MinWidth = props.MinWidth,
            .MaxWidth = props.MaxWidth,
            .MinHeight = props.MinHeight,
            .MaxHeight = props.MaxHeight,
        });

        if (w.Clicked)
            value = !value;

        float opacity = (w.IsEnabled ? 1.0f : DisabledOpacity) * props.Opacity;
        float check = Transition(w, "check", value ? 1.0f : 0.0f, CheckDuration);

        Brush background = value ? props.CheckedBackground : props.BoxBackground;
        if (w.Pressed && w.Hovered)
            background = Mix(background, Colors::Black, PressedDarken);
        else if (w.Hovered || w.Pressed)
            background = Mix(background, Colors::White, HoverLighten);
        background = TransitionBrush(*this, w, "bg.from", "bg.to", background, ColorDuration);

        Vec2 boxPosition = SnapToPixel(*this, { w.Rect.X, w.Rect.CenterY() - props.BoxSize * 0.5f });
        Rect box = { boxPosition.X, boxPosition.Y, props.BoxSize, props.BoxSize };
        DrawRect(box, {
            .Fill = Fade(background, opacity),
            .Stroke = Fade(props.BoxBorderBrush, opacity * (1 - check)), // the checked fill replaces the border
            .StrokeThickness = 1,
            .CornerRadius = props.CornerRadius,
            .Glow = { WithOpacity(props.Glow.Color, opacity * check), props.Glow.Radius * check },
        });

        // Check mark: two strokes drawn in progressively along the path.
        if (check > 0.01f)
        {
            Vec2 points[3] = {
                box.Position() + Vec2 { 0.27f, 0.52f } * box.Width,
                box.Position() + Vec2 { 0.43f, 0.68f } * box.Width,
                box.Position() + Vec2 { 0.74f, 0.34f } * box.Width,
            };
            uint32_t count = 3;
            if (check < CheckMarkSplit)
            {
                points[1] = Lerp(points[0], points[1], check / CheckMarkSplit);
                count = 2;
            }
            else
            {
                points[2] = Lerp(points[1], points[2], (check - CheckMarkSplit) / (1 - CheckMarkSplit));
            }
            DrawPolyline({ points, count }, { .Stroke = Fade(props.CheckMark, opacity), .Thickness = props.BoxSize * CheckMarkThickness });
        }

        if (layout)
        {
            style.Foreground = Fade(props.Foreground, opacity);
            ui.DrawLayout({ box.Right() + textOffset, w.Rect.CenterY() - textSize.Y * 0.5f }, *layout, style);
        }
        return w.Clicked;
    }

    bool Ui::Slider(float& value, float minimum, float maximum, const SliderProps& props)
    {
        // A stretched slider fills its slot; the default width still counts as its desired size
        // so auto-sized panels and grid columns leave it a usable width.
        Funky::Widget w = Widget(props.Key.IsNone() ? Key::FromPointer(&value) : props.Key, {
            .Width = DesiredSize(props.Width, SliderDefaultWidth),
            .Height = DesiredSize(props.Height, SliderDefaultHeight),
            .IsEnabled = props.IsEnabled,
            .Margin = props.Margin,
            .HorizontalAlignment = ExplicitAlignment(props.HorizontalAlignment, props.Width),
            .VerticalAlignment = ExplicitAlignment(props.VerticalAlignment, props.Height),
            .MinWidth = props.MinWidth,
            .MaxWidth = props.MaxWidth,
            .MinHeight = props.MinHeight,
            .MaxHeight = props.MaxHeight,
        });

        float range = maximum - minimum;
        float trackStart = w.Rect.X + props.ThumbRadius;
        float trackLength = Max(w.Rect.Width - 2 * props.ThumbRadius, 0.0f);

        bool changed = false;
        if (w.Pressed && range != 0 && trackLength > 0)
        {
            float t = Saturate((PointerPosition().X - trackStart) / trackLength);
            float v = minimum + t * range;
            if (props.Step > 0)
            {
                // The last step may fall short of maximum: the end of the track still reaches it.
                float snapped = minimum + Round((v - minimum) / props.Step) * props.Step;
                v = Abs(v - maximum) < Abs(v - snapped) ? maximum : snapped;
            }
            v = Clamp(v, Min(minimum, maximum), Max(minimum, maximum));
            if (v != value)
            {
                value = v;
                changed = true;
            }
        }

        // The thumb follows the value directly (no Transition) so it never lags behind the pointer.
        float t = range != 0 ? Saturate((value - minimum) / range) : 0.0f;
        float thumbX = trackStart + t * trackLength;
        float centerY = w.Rect.CenterY();
        float opacity = (w.IsEnabled ? 1.0f : DisabledOpacity) * props.Opacity;
        bool active = w.Hovered || w.Pressed;

        float thickness = props.TrackThickness;
        Rect track = { trackStart, centerY - thickness * 0.5f, trackLength, thickness };
        DrawRect(track, { .Fill = Fade(props.Track, opacity), .CornerRadius = thickness * 0.5f });
        if (thumbX > trackStart)
            DrawRect({ track.X, track.Y, thumbX - trackStart, thickness }, { .Fill = Fade(props.Fill, opacity), .CornerRadius = thickness * 0.5f });

        float radius = Transition(w, "thumb", active ? props.ThumbRadius * ThumbGrow : props.ThumbRadius, ThumbDuration);
        float glowRadius = Transition(w, "glow", active ? props.Glow.Radius : 0.0f, GlowDuration);
        Glow glow = {};
        if (props.Glow.Radius > 0)
            glow = { WithOpacity(props.Glow.Color, opacity * glowRadius / props.Glow.Radius), glowRadius };

        Shadow shadow = ThumbShadow;
        shadow.Color = WithOpacity(shadow.Color, opacity);
        DrawCircle({ thumbX, centerY }, radius, { .Fill = Fade(props.Thumb, opacity), .Glow = glow, .Shadow = shadow });
        return changed;
    }
}
