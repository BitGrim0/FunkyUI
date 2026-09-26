// FunkyUI — immediate-mode UI for overlays on top of other applications.
// Single public header. See docs/REQUIREMENTS.md and docs/ARCHITECTURE.md.
//
// Conventions:
//   * All coordinates and sizes are in DIPs (1 DIP = 1 pixel at 96 DPI), origin = top-left
//     corner of the target window's client area. DpiScale() converts DIPs to physical pixels.
//   * A size of 0 in props means "automatic".
//   * Props structs are aggregates meant for designated initializers:
//       ui.Button("Apply", { .Background = Rgb(0x3AA8FF), .CornerRadius = 6 });

#pragma once

// Omitting props fields is the intended use: silence clang's /W4 warning about it. Not popped,
// since the warning fires in the client's code after this header.
#if defined(__has_warning)
    #if __has_warning("-Wmissing-designated-field-initializers")
        #pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
    #endif
#endif

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace Funky
{
    // ------------------------------------------------------------------------------------
    // Basic types
    // ------------------------------------------------------------------------------------

    struct Vec2
    {
        float X = 0;
        float Y = 0;
    };

    constexpr Vec2 operator+(Vec2 a, Vec2 b) { return { a.X + b.X, a.Y + b.Y }; }
    constexpr Vec2 operator-(Vec2 a, Vec2 b) { return { a.X - b.X, a.Y - b.Y }; }
    constexpr Vec2 operator*(Vec2 a, float s) { return { a.X * s, a.Y * s }; }
    constexpr Vec2 operator/(Vec2 a, float s) { return { a.X / s, a.Y / s }; }
    constexpr bool operator==(Vec2 a, Vec2 b) { return a.X == b.X && a.Y == b.Y; }

    struct Thickness
    {
        float Left = 0;
        float Top = 0;
        float Right = 0;
        float Bottom = 0;

        constexpr Thickness() = default;
        constexpr Thickness(float uniform) : Left(uniform), Top(uniform), Right(uniform), Bottom(uniform) {}
        constexpr Thickness(float horizontal, float vertical) : Left(horizontal), Top(vertical), Right(horizontal), Bottom(vertical) {}
        constexpr Thickness(float left, float top, float right, float bottom) : Left(left), Top(top), Right(right), Bottom(bottom) {}

        constexpr float Horizontal() const { return Left + Right; }
        constexpr float Vertical() const { return Top + Bottom; }
    };

    struct Rect
    {
        float X = 0;
        float Y = 0;
        float Width = 0;
        float Height = 0;

        constexpr float Left() const { return X; }
        constexpr float Top() const { return Y; }
        constexpr float Right() const { return X + Width; }
        constexpr float Bottom() const { return Y + Height; }
        constexpr Vec2 Position() const { return { X, Y }; }
        constexpr Vec2 Size() const { return { Width, Height }; }
        constexpr Vec2 Center() const { return { X + Width * 0.5f, Y + Height * 0.5f }; }
        constexpr float CenterX() const { return X + Width * 0.5f; }
        constexpr float CenterY() const { return Y + Height * 0.5f; }

        constexpr bool Contains(Vec2 p) const { return p.X >= X && p.Y >= Y && p.X < X + Width && p.Y < Y + Height; }
        constexpr Rect Deflate(Thickness t) const { return { X + t.Left, Y + t.Top, Width - t.Horizontal(), Height - t.Vertical() }; }
        constexpr Rect Inflate(Thickness t) const { return { X - t.Left, Y - t.Top, Width + t.Horizontal(), Height + t.Vertical() }; }
    };

    struct CornerRadius
    {
        float TopLeft = 0;
        float TopRight = 0;
        float BottomRight = 0;
        float BottomLeft = 0;

        constexpr CornerRadius() = default;
        constexpr CornerRadius(float uniform) : TopLeft(uniform), TopRight(uniform), BottomRight(uniform), BottomLeft(uniform) {}
        constexpr CornerRadius(float topLeft, float topRight, float bottomRight, float bottomLeft)
            : TopLeft(topLeft), TopRight(topRight), BottomRight(bottomRight), BottomLeft(bottomLeft) {}
    };

    // sRGB color with straight (non-premultiplied) alpha.
    struct Color
    {
        uint8_t R = 0;
        uint8_t G = 0;
        uint8_t B = 0;
        uint8_t A = 0;
    };

    constexpr bool operator==(Color a, Color b) { return a.R == b.R && a.G == b.G && a.B == b.B && a.A == b.A; }

    // Rgb(0xRRGGBB), fully opaque.
    constexpr Color Rgb(uint32_t rgb)
    {
        return { uint8_t(rgb >> 16), uint8_t(rgb >> 8), uint8_t(rgb), 255 };
    }

    // Rgba(0xRRGGBBAA).
    constexpr Color Rgba(uint32_t rgba)
    {
        return { uint8_t(rgba >> 24), uint8_t(rgba >> 16), uint8_t(rgba >> 8), uint8_t(rgba) };
    }

    // Same color with alpha multiplied by opacity (0..1).
    constexpr Color WithOpacity(Color c, float opacity)
    {
        float a = float(c.A) * (opacity < 0 ? 0 : opacity > 1 ? 1 : opacity);
        return { c.R, c.G, c.B, uint8_t(a + 0.5f) };
    }

    namespace Colors
    {
        inline constexpr Color Transparent = { 0, 0, 0, 0 };
        inline constexpr Color Black = Rgb(0x000000);
        inline constexpr Color White = Rgb(0xFFFFFF);
    }

    // Solid color or linear gradient. A default-constructed Brush is "none" (nothing is drawn,
    // or — for control props documented as such — "derive automatically").
    struct Brush
    {
        enum class Kind : uint8_t
        {
            None,
            Solid,
            LinearGradient,
        };

        Kind Type = Kind::None;
        Color From;
        Color To;
        float Angle = 90; // gradient direction in degrees: 0 = left→right, 90 = top→bottom

        constexpr Brush() = default;
        constexpr Brush(Color color) : Type(Kind::Solid), From(color), To(color) {}

        static constexpr Brush Linear(Color from, Color to, float angle = 90)
        {
            Brush b;
            b.Type = Kind::LinearGradient;
            b.From = from;
            b.To = to;
            b.Angle = angle;
            return b;
        }

        constexpr bool IsNone() const { return Type == Kind::None; }
    };

    // Outer glow around a shape or text. Radius 0 = no glow.
    struct Glow
    {
        Funky::Color Color;
        float Radius = 0;
    };

    // Drop shadow. Blur 0 and zero offset = no shadow.
    struct Shadow
    {
        Vec2 Offset;
        float Blur = 0;
        Funky::Color Color;
    };

    // Text outline (rendered from the distance-field channel; stage 2).
    struct Outline
    {
        Funky::Color Color;
        float Thickness = 0;
    };

    enum class HorizontalAlignment : uint8_t
    {
        Stretch,
        Left,
        Center,
        Right,
    };

    enum class VerticalAlignment : uint8_t
    {
        Stretch,
        Top,
        Center,
        Bottom,
    };

    enum class Orientation : uint8_t
    {
        Vertical,
        Horizontal,
    };

    enum class Anchor : uint8_t
    {
        TopLeft,
        Top,
        TopRight,
        Left,
        Center,
        Right,
        BottomLeft,
        Bottom,
        BottomRight,
    };

    enum class FontWeight : uint16_t
    {
        Thin = 100,
        Light = 300,
        Normal = 400,
        Medium = 500,
        SemiBold = 600,
        Bold = 700,
        Black = 900,
    };

    // Grid column width: Auto (fits content), Px(n) (fixed), Star(n) (share of remaining space).
    struct GridLength
    {
        enum class Kind : uint8_t
        {
            Unused,
            Auto,
            Pixel,
            Star,
        };

        Kind Type = Kind::Unused;
        float Value = 0;
    };

    inline constexpr GridLength Auto = { GridLength::Kind::Auto, 0 };
    constexpr GridLength Px(float value) { return { GridLength::Kind::Pixel, value }; }
    constexpr GridLength Star(float weight = 1) { return { GridLength::Kind::Star, weight }; }

    // Animation duration. Use the literal: 120_ms.
    struct Duration
    {
        float Seconds = 0;
    };

    namespace Literals
    {
        constexpr Duration operator""_ms(unsigned long long ms) { return { float(ms) * 0.001f }; }
        constexpr Duration operator""_ms(long double ms) { return { float(ms) * 0.001f }; }
    }

    // ------------------------------------------------------------------------------------
    // Keys and IDs
    // ------------------------------------------------------------------------------------

    constexpr uint64_t HashBytes(const char* data, size_t size, uint64_t seed = 0xcbf29ce484222325ull)
    {
        uint64_t h = seed;
        for (size_t i = 0; i < size; ++i)
        {
            h ^= uint8_t(data[i]);
            h *= 0x100000001b3ull;
        }
        return h;
    }

    // Identifies a panel, container or control among its siblings.
    // A string literal is hashed at compile time and does NOT end up in the binary:
    //     ui.Panel("stats", ...)          // Key("stats") is consteval
    // Runtime strings: Key::FromString(text). Integers: Key(42).
    struct Key
    {
        uint64_t Value = 0;

        constexpr Key() = default;
        explicit constexpr Key(uint64_t value) : Value(value) {}

        template <size_t N>
        consteval Key(const char (&text)[N]) : Value(HashBytes(text, N - 1))
        {
        }

        static constexpr Key FromString(std::string_view text) { return Key(HashBytes(text.data(), text.size())); }
        static Key FromPointer(const void* pointer) { return Key(uint64_t(reinterpret_cast<uintptr_t>(pointer)) * 0x9E3779B97F4A7C15ull); }

        constexpr bool IsNone() const { return Value == 0; }
    };

    // Unique id of a widget in the current frame (parent id combined with its key).
    struct Id
    {
        uint64_t Value = 0;
    };

    constexpr bool operator==(Id a, Id b) { return a.Value == b.Value; }

    // ------------------------------------------------------------------------------------
    // Fonts
    // ------------------------------------------------------------------------------------

    // Handle to a font family registered with Ui::LoadFont. Default (Index 0) = the default
    // family from OverlayDesc.
    struct Font
    {
        uint16_t Index = 0;
    };

    struct TextStyle
    {
        Funky::Font Font;
        float FontSize = 14;
        Funky::FontWeight FontWeight = Funky::FontWeight::Normal;
        bool Italic = false;
        Brush Foreground = Rgb(0xE6E8EE);
        Funky::Glow Glow;       // stage 2 (distance field)
        Funky::Shadow Shadow;   // stage 2 (distance field)
        Funky::Outline Outline; // stage 2 (distance field)
    };

    // ------------------------------------------------------------------------------------
    // Primitive styles
    // ------------------------------------------------------------------------------------

    // Rectangles and circles.
    struct ShapeStyle
    {
        Brush Fill;
        Brush Stroke;
        float StrokeThickness = 0; // inner stroke (drawn inside the shape edge)
        Funky::CornerRadius CornerRadius; // rectangles only
        Funky::Glow Glow;
        Funky::Shadow Shadow;
    };

    // Lines, polylines, Bézier curves and arcs. Caps are round.
    struct LineStyle
    {
        Brush Stroke = Colors::White;
        float Thickness = 1;
        Funky::Glow Glow;
    };

    // Triangles and convex polygons.
    struct FillStyle
    {
        Brush Fill = Colors::White;
        Funky::Glow Glow;
    };

    // ------------------------------------------------------------------------------------
    // Layout / container props
    //
    // Every props struct repeats the common layout fields (designated initializers do not
    // work with base classes):
    //     Margin, HorizontalAlignment, VerticalAlignment, Width, Height,
    //     MinWidth, MaxWidth, MinHeight, MaxHeight          (0 = automatic / unlimited)
    // ------------------------------------------------------------------------------------

    struct PanelProps
    {
        std::string_view Title;        // empty = no title bar
        Funky::Anchor Anchor = Funky::Anchor::TopLeft;
        Thickness Margin = 16;         // distance from the viewport edges used by Anchor
        Vec2* Position = nullptr;      // optional client-owned position (read and written on drag)
        bool Movable = false;          // draggable in interactive mode
        float Width = 0;               // 0 = size to content
        float Height = 0;              // 0 = size to content
        float MinWidth = 0;
        float MinHeight = 0;
        Thickness Padding = 12;
        float Spacing = 8;             // vertical gap between children
        Brush Background = Rgba(0x12141AE6);
        Brush BorderBrush = Rgba(0xFFFFFF14);
        float BorderThickness = 1;
        Funky::CornerRadius CornerRadius = 10;
        Funky::Shadow Shadow = { { 0, 6 }, 18, Rgba(0x00000080) };
        Funky::Glow Glow;
        float Opacity = 1;
    };

    struct StackProps
    {
        Funky::Key Key;                // 0 = positional
        Funky::Orientation Orientation = Funky::Orientation::Vertical;
        float Spacing = 6;
        Thickness Margin;
        Funky::HorizontalAlignment HorizontalAlignment = Funky::HorizontalAlignment::Stretch;
        Funky::VerticalAlignment VerticalAlignment = Funky::VerticalAlignment::Stretch;
        float Width = 0;
        float Height = 0;
        float MinWidth = 0;
        float MaxWidth = 0;
        float MinHeight = 0;
        float MaxHeight = 0;
    };

    // Children fill cells left→right, top→bottom (row-major auto flow).
    struct GridProps
    {
        Funky::Key Key;                // 0 = positional
        GridLength Columns[8] = {};    // unused entries stay GridLength::Kind::Unused
        float ColumnSpacing = 10;
        float RowSpacing = 6;
        Thickness Margin;
        Funky::HorizontalAlignment HorizontalAlignment = Funky::HorizontalAlignment::Stretch;
        Funky::VerticalAlignment VerticalAlignment = Funky::VerticalAlignment::Stretch;
        float Width = 0;
        float Height = 0;
        float MinWidth = 0;
        float MaxWidth = 0;
        float MinHeight = 0;
        float MaxHeight = 0;
    };

    // ------------------------------------------------------------------------------------
    // Control props. Defaults are the hand-tuned dark gaming style.
    // ------------------------------------------------------------------------------------

    struct LabelProps
    {
        Brush Foreground = Rgb(0xE6E8EE);
        float FontSize = 14;
        Funky::FontWeight FontWeight = Funky::FontWeight::Normal;
        bool Italic = false;
        Funky::Font Font;
        Funky::Glow Glow;              // stage 2
        Funky::Shadow Shadow;          // stage 2
        Funky::Outline Outline;        // stage 2
        float Opacity = 1;
        Thickness Margin;
        Funky::HorizontalAlignment HorizontalAlignment = Funky::HorizontalAlignment::Left;
        Funky::VerticalAlignment VerticalAlignment = Funky::VerticalAlignment::Center;
        float Width = 0;
        float Height = 0;
        float MinWidth = 0;
        float MaxWidth = 0;
        float MinHeight = 0;
        float MaxHeight = 0;
    };

    struct ButtonProps
    {
        Funky::Key Key;                    // 0 = derived from the text
        Brush Background = Rgb(0x262B38);
        Brush HoverBackground;             // none = derived from Background
        Brush PressedBackground;           // none = derived from Background
        Brush Foreground = Rgb(0xE6E8EE);
        Brush BorderBrush = Rgba(0xFFFFFF1A);
        float BorderThickness = 1;
        Funky::CornerRadius CornerRadius = 6;
        Thickness Padding = { 14, 7 };
        float FontSize = 14;
        Funky::FontWeight FontWeight = Funky::FontWeight::Medium;
        Funky::Font Font;
        Funky::Glow Glow;                  // shown on hover when Radius > 0
        Funky::Shadow Shadow;
        bool IsEnabled = true;
        float Opacity = 1;
        Thickness Margin;
        Funky::HorizontalAlignment HorizontalAlignment = Funky::HorizontalAlignment::Stretch;
        Funky::VerticalAlignment VerticalAlignment = Funky::VerticalAlignment::Center;
        float Width = 0;
        float Height = 0;
        float MinWidth = 0;
        float MaxWidth = 0;
        float MinHeight = 0;
        float MaxHeight = 0;
    };

    struct CheckBoxProps
    {
        Funky::Key Key;                    // 0 = derived from the address of the bound value
        Brush Foreground = Rgb(0xE6E8EE);
        Brush BoxBackground = Rgb(0x1C202A);
        Brush BoxBorderBrush = Rgba(0xFFFFFF33);
        Brush CheckedBackground = Rgb(0x3AA8FF);
        Brush CheckMark = Rgb(0x0B0D12);
        float BoxSize = 18;
        Funky::CornerRadius CornerRadius = 5;
        float FontSize = 14;
        Funky::FontWeight FontWeight = Funky::FontWeight::Normal;
        Funky::Font Font;
        Funky::Glow Glow = { Rgba(0x3AA8FF80), 8 }; // shown when checked
        bool IsEnabled = true;
        float Opacity = 1;
        Thickness Margin;
        Funky::HorizontalAlignment HorizontalAlignment = Funky::HorizontalAlignment::Left;
        Funky::VerticalAlignment VerticalAlignment = Funky::VerticalAlignment::Center;
        float Width = 0;
        float Height = 0;
        float MinWidth = 0;
        float MaxWidth = 0;
        float MinHeight = 0;
        float MaxHeight = 0;
    };

    struct SliderProps
    {
        Funky::Key Key;                    // 0 = derived from the address of the bound value
        float Step = 0;                    // 0 = continuous
        Brush Track = Rgb(0x1C202A);
        Brush Fill = Brush::Linear(Rgb(0x2F7BFF), Rgb(0x3AA8FF), 0);
        Brush Thumb = Rgb(0xF2F4F8);
        float TrackThickness = 4;
        float ThumbRadius = 7;
        Funky::Glow Glow = { Rgba(0x3AA8FF99), 10 }; // thumb glow on hover / drag
        bool IsEnabled = true;
        float Opacity = 1;
        Thickness Margin;
        Funky::HorizontalAlignment HorizontalAlignment = Funky::HorizontalAlignment::Stretch;
        Funky::VerticalAlignment VerticalAlignment = Funky::VerticalAlignment::Center;
        float Width = 0;                   // 0 = stretch / 160 DIPs when not stretched
        float Height = 0;                  // 0 = 22 DIPs
        float MinWidth = 0;
        float MaxWidth = 0;
        float MinHeight = 0;
        float MaxHeight = 0;
    };

    // Props for custom controls built with Ui::Widget.
    struct WidgetProps
    {
        float Width = 0;                   // desired size
        float Height = 0;
        bool IsEnabled = true;
        Thickness Margin;
        Funky::HorizontalAlignment HorizontalAlignment = Funky::HorizontalAlignment::Left;
        Funky::VerticalAlignment VerticalAlignment = Funky::VerticalAlignment::Center;
        float MinWidth = 0;
        float MaxWidth = 0;
        float MinHeight = 0;
        float MaxHeight = 0;
    };

    // Result of Ui::Widget: placement and interaction state for this frame.
    struct Widget
    {
        Funky::Id Id;
        Funky::Rect Rect;          // final rectangle (DIPs)
        bool Hovered = false;      // pointer is over the widget (and nothing covers it)
        bool Pressed = false;      // left button went down on the widget and is still held
        bool Clicked = false;      // left button released over the widget after pressing it
        bool IsEnabled = true;
        Vec2 DragDelta;            // pointer movement since last frame while Pressed
    };

    // ------------------------------------------------------------------------------------
    // Overlay
    // ------------------------------------------------------------------------------------

    struct Allocator
    {
        void* (*Allocate)(size_t size, void* user) = nullptr;
        void (*Free)(void* pointer, void* user) = nullptr;
        void* User = nullptr;
    };

    struct OverlayDesc
    {
        void* TargetWindow = nullptr;          // HWND of the (borderless) target window
        std::string_view WindowClassName;      // UTF-8, required, unique per process
        std::string_view DefaultFontFamily;    // UTF-8, required, e.g. the client's UI font
        Funky::Allocator Allocator;            // optional; default = process heap. Process-wide: give every overlay the same one
    };

    class Ui;

    // RAII scope returned by containers: if (auto panel = ui.Panel("stats")) { ... }
    class [[nodiscard]] Scope
    {
    public:
        Scope(Scope&& other) noexcept : Owner(other.Owner), IsOpen(other.IsOpen) { other.Owner = nullptr; }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        Scope& operator=(Scope&&) = delete;
        ~Scope();

        explicit operator bool() const { return IsOpen; }

    private:
        friend class Ui;
        Scope(Ui* owner, bool isOpen) : Owner(owner), IsOpen(isOpen) {}

        Ui* Owner = nullptr;
        bool IsOpen = false;
    };

    // The overlay window and UI context. Create one per target window; use from one thread.
    class Ui
    {
    public:
        static Ui* Create(const OverlayDesc& desc); // nullptr on failure
        void Destroy();

        // --- Frame -------------------------------------------------------------------------
        // BeginFrame waits for the next frame slot, pumps window messages, follows the target
        // window and reads input. Returns false when the target window is gone.
        bool BeginFrame();
        void EndFrame();

        // --- Input ---------------------------------------------------------------------------
        void SetInteractive(bool interactive); // menu mode: panels receive the mouse
        bool IsInteractive() const;
        bool IsPointerOver() const;            // pointer over UI or a drag is in progress
        bool IsKeyboardFocused() const;        // a text box owns the keyboard (stage 2)

        // --- Frame info ----------------------------------------------------------------------
        Vec2 ViewportSize() const;             // DIPs
        Vec2 PointerPosition() const;          // DIPs, relative to the viewport
        float DeltaTime() const;               // seconds since the previous frame
        float DpiScale() const;                // physical pixels per DIP

        // --- Fonts & text --------------------------------------------------------------------
        Font LoadFont(std::string_view family); // UTF-8 family name
        Vec2 MeasureText(std::string_view text, const TextStyle& style = {});

        // --- Primitives (DIPs, viewport space) ---------------------------------------------
        // Outside any panel they are drawn below all panels; inside a panel they belong to it
        // (clipped to the panel and following its z-order).
        void DrawRect(Rect rect, const ShapeStyle& style);
        void DrawCircle(Vec2 center, float radius, const ShapeStyle& style);
        void DrawArc(Vec2 center, float radius, float startAngle, float sweepAngle, const LineStyle& style); // degrees, clockwise from +X
        void DrawLine(Vec2 from, Vec2 to, const LineStyle& style);
        void DrawPolyline(std::span<const Vec2> points, const LineStyle& style);
        void DrawBezier(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, const LineStyle& style); // cubic
        void DrawTriangle(Vec2 a, Vec2 b, Vec2 c, const FillStyle& style);
        void DrawPolygon(std::span<const Vec2> convexPoints, const FillStyle& style);
        void DrawString(Vec2 position, std::string_view text, const TextStyle& style = {}); // position = top-left

        // --- Containers ----------------------------------------------------------------------
        Scope Panel(Key key, const PanelProps& props = {});
        Scope Stack(const StackProps& props = {});
        Scope Grid(const GridProps& props);

        // --- Controls ------------------------------------------------------------------------
        void Label(std::string_view text, const LabelProps& props = {});
        bool Button(std::string_view text, const ButtonProps& props = {});                      // true when clicked
        bool CheckBox(std::string_view text, bool& value, const CheckBoxProps& props = {});     // true when toggled
        bool Slider(float& value, float minimum, float maximum, const SliderProps& props = {}); // true when changed

        // --- Custom controls -----------------------------------------------------------------
        // Reserves a slot in the current layout and resolves hover / press / click for it.
        Funky::Widget Widget(Key key, const WidgetProps& props);

        // Smoothly moves a value towards target (ease-out) over the given duration and returns
        // the current value. The first call returns target. Colors blend in linear space.
        float Transition(const Funky::Widget& widget, Key slot, float target, Duration duration);
        Vec2 Transition(const Funky::Widget& widget, Key slot, Vec2 target, Duration duration);
        Color Transition(const Funky::Widget& widget, Key slot, Color target, Duration duration);
        float Transition(Key key, float target, Duration duration); // not tied to a widget
        Vec2 Transition(Key key, Vec2 target, Duration duration);
        Color Transition(Key key, Color target, Duration duration);

    protected:
        Ui() = default;
        ~Ui() = default;

    private:
        friend class Scope;
        void EndScope();
    };

    inline Scope::~Scope()
    {
        if (Owner)
            Owner->EndScope();
    }

    // ------------------------------------------------------------------------------------
    // Helpers
    // ------------------------------------------------------------------------------------

    constexpr float Lerp(float a, float b, float t) { return a + (b - a) * t; }
    constexpr Vec2 Lerp(Vec2 a, Vec2 b, float t) { return { Lerp(a.X, b.X, t), Lerp(a.Y, b.Y, t) }; }
    Color Lerp(Color a, Color b, float t); // blends in linear space
}
