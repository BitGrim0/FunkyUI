// Internal module interfaces. Windows headers are NOT included here: Win32/COM types are
// forward-declared so that Ui.cpp / Layout.cpp / Draw.cpp stay platform-light.
// Host.cpp, Renderer.cpp and Text.cpp include <windows.h> & co themselves.

#pragma once

#include "Base.h"

struct HWND__;
struct ID3D11Device;
struct ID3D11RenderTargetView;

namespace Funky
{
    // ====================================================================================
    // GPU instances. Every primitive is one screen-aligned quad; the kinds are batched separately
    // (like Zed's GPUI), each with its own compact instance buffer and shaders:
    //   GpuShape — src/Shaders/Shape.hlsl: the pixel shader evaluates the shape's signed distance function;
    //   GpuGlyph — src/Shaders/Glyph.hlsl: a textured quad from the glyph atlas.
    // All geometry is in DIPs; the shaders convert to pixels with DpiScale.
    // ====================================================================================

    enum ShapeType : uint32_t
    {
        ShapeRoundRect = 0, // P0 = rect (minX, minY, maxX, maxY), P1 = radii (TL, TR, BR, BL)
        ShapePolyline = 1,  // P0 = (first point, point count, half thickness): round-joined, round-capped stroke through UiImpl::Points
        ShapeArc = 2,       // P0 = (cx, cy, radius, half thickness), P1 = (start angle, sweep angle) radians, clockwise from +X (y down), round caps
        ShapeTriangle = 3,  // P0 = (ax, ay, bx, by), P1 = (cx, cy); edges AA'd only if their Flag*Edge bit is set
        ShapeImage = 4,     // stage 3: Bounds = image rect, P0 = UV, P1 = corner radii
    };

    enum ShapeFlags : uint32_t
    {
        FlagGradient = 1u << 8,  // Fill0 → Fill1 along the segment P2.xy → P2.zw (blended in linear space)
        FlagEdgeAB = 1u << 9,    // triangle edge a→b is an outer edge (antialiased)
        FlagEdgeBC = 1u << 10,
        FlagEdgeCA = 1u << 11,
        FlagShadow = 1u << 12,   // round rect without a gradient: a shadow under it, offset P2.xy, sigma P2.z (0 = hard), color Fill1
    };

    constexpr uint32_t ShapeTypeMask = 0xFFu;
    constexpr uint32_t ShapeClipShift = 16; // bits 16..31 = index into the clip rect buffer (0 = none)

    struct GpuShape
    {
        float Bounds[4];     // quad to rasterize (minX, minY, maxX, maxY), already expanded for AA / glow / blur
        float P0[4];         // geometry, see ShapeType
        float P1[4];
        float P2[4];         // gradient start (xy) and end (zw) points; FlagShadow: shadow offset (xy), sigma (z)
        uint32_t Fill0;      // packed Color (R in the lowest byte), sRGB, straight alpha
        uint32_t Fill1;      // gradient end color; FlagShadow: shadow color
        uint32_t Stroke;     // inner stroke color
        uint32_t Glow;       // glow color
        float StrokeWidth;   // DIPs, inner stroke (inside the edge)
        float GlowRadius;    // DIPs, glow falls off outside the edge
        float Softness;      // DIPs, > 0 = blurred silhouette (shadows): coverage uses a Gaussian of this sigma; stroke & glow ignored
        uint32_t TypeFlags;  // ShapeType | ShapeFlags | clipIndex << ShapeClipShift
    };

    static_assert(sizeof(GpuShape) == 96);

    // Glyph instance — shared layout with src/Shaders/Glyph.hlsl (struct Glyph). One color per glyph:
    // a gradient brush is sampled at the glyph's center on the CPU.
    struct GpuGlyph
    {
        float Rect[4];       // quad (minX, minY, maxX, maxY), DIPs
        uint32_t Texel0;     // atlas texel of the top-left corner: x | y << 16
        uint32_t Texel1;     // bottom-right corner (exclusive)
        uint32_t Color;      // packed Color, sRGB, straight alpha
        uint32_t Clip;       // index into the clip rect buffer (0 = none)
    };

    static_assert(sizeof(GpuGlyph) == 32);

    struct ClipRect
    {
        float MinX, MinY, MaxX, MaxY; // DIPs
    };

    constexpr ClipRect EmptyBounds = { 3.0e38f, 3.0e38f, -3.0e38f, -3.0e38f }; // start of a union of bounds

    enum BatchKind : uint32_t
    {
        BatchShapes = 0,
        BatchGlyphs = 1,
    };

    // One draw call: a range of one instance buffer, in paint order.
    struct DrawBatch
    {
        uint32_t Kind;       // BatchKind: which buffer and shaders
        uint32_t First;      // first instance in that buffer
        uint32_t Count;
        float Opacity;       // the layer's opacity (panel fade / Opacity), multiplies the shaders' output
    };

    constexpr uint32_t PackColor(Color c)
    {
        return uint32_t(c.R) | (uint32_t(c.G) << 8) | (uint32_t(c.B) << 16) | (uint32_t(c.A) << 24);
    }

    // Compile-time profiler, off by default (no code at all without the define): QueryPerformanceCounter
    // ticks spent in EndFrame's phases during the last frame. Build with FUNKY_PROFILE (library and reader)
    // and read it with GetFrameProfile after EndFrame; a skipped frame has only Build.
#if defined(FUNKY_PROFILE)
    #define FK_PROFILE(...) __VA_ARGS__

    struct FrameProfile
    {
        int64_t Build;      // BuildFrame (batches) and the frame hash
        int64_t Upload;     // atlas and instance buffers
        int64_t Draw;       // pipeline state and draw calls
        int64_t Present;    // Present (overlay only)
    };

    const FrameProfile& GetFrameProfile(const Ui* ui);
    int64_t ProfileTicks();
#else
    #define FK_PROFILE(...)
#endif

    // ====================================================================================
    // Host — the overlay window (Host.cpp)
    // ====================================================================================

    struct InputEvents
    {
        Vec2 PointerPx;         // physical pixels relative to the overlay's client area (GetCursorPos every frame)
        bool ButtonDown[3];     // left, right, middle — tracked from window messages
        uint8_t Pressed[3];     // down transitions since ClearInputEdges()
        uint8_t Released[3];    // up transitions since ClearInputEdges()
        float Wheel;            // notches since ClearInputEdges(), positive = away from the user
    };

    class Host
    {
    public:
        // Creates the overlay window over the target's client area (styles, DPI awareness, class
        // registration). Does not create any D3D objects.
        bool Init(HWND__* target, std::string_view className, Arena& scratch);
        void Shutdown();

        // Pumps window messages and follows the target (position, size, z-order, visibility).
        // Returns false when the target window no longer exists.
        bool Update();

        HWND__* Window() const;
        uint32_t Width() const;             // physical pixels
        uint32_t Height() const;
        bool IsVisible() const;             // false while the target is minimized / hidden
        float DpiScale() const;

        const InputEvents& Input() const;
        void ClearInputEdges();

        void SetClickThrough(bool clickThrough);      // toggles WS_EX_TRANSPARENT (only on change)
        void SetPointerCapture(bool capture);         // SetCapture / ReleaseCapture (only on change)

        // Frame pacing: waits on the swap chain's frame-latency waitable object after a Present,
        // otherwise (frame skipped) waits for the next composition cycle with DwmFlush().
        void WaitForFrame(void* frameWaitable, bool presentedLastFrame);

    private:
        struct Impl;
        Impl* State = nullptr;              // defined in Host.cpp
    };

    // ====================================================================================
    // Renderer — D3D11 + DirectComposition (Renderer.cpp, Shaders/).
    // Embedded mode: the client's device, no swap chain; draws into the view given by SetTarget.
    // ====================================================================================

    struct AtlasUpdate
    {
        const uint8_t* Pixels;  // R8G8 texels (coverage, distance), row pitch = Size * 2
        uint32_t Size;          // atlas width = height, texels
        uint32_t X, Y, Width, Height; // dirty region to upload
        bool Recreate;          // size changed or atlas reset: recreate the texture and upload everything
    };

    // Everything one Render draws. Instances, points and clips are in DIPs.
    struct FrameData
    {
        const GpuShape* Shapes;
        uint32_t ShapeCount;
        const GpuGlyph* Glyphs;
        uint32_t GlyphCount;
        const Vec2* Points;     // polyline vertices
        uint32_t PointCount;
        const ClipRect* Clips;  // [0] = no clipping
        uint32_t ClipCount;
        const DrawBatch* Batches; // paint order
        uint32_t BatchCount;
        float DpiScale;
    };

    class Renderer
    {
    public:
        bool Init(HWND__* window, uint32_t width, uint32_t height); // overlay: own device and swap chain, physical pixels
        bool Init(ID3D11Device* device);                           // embedded: the client's device (referenced)
        void Shutdown();
        void Resize(uint32_t width, uint32_t height);              // overlay
        void UpdateAtlas(const AtlasUpdate& update);

        // Embedded: the view the next Render draws into and its size in physical pixels. The view is
        // referenced only until that Render, so the client can resize its swap chain between frames.
        void SetTarget(ID3D11RenderTargetView* view, uint32_t width, uint32_t height);

        // Overlay: after a device loss (driver update, GPU reset or switch) recreates the device and swap
        // chain, retrying at most every few frames. Returns true when it did: the atlas must be uploaded again.
        bool RestoreDevice();

        // Overlay: clears to transparent, draws the batches and presents. Embedded: draws on top of the
        // target and restores the device context state it changed.
        bool Render(const FrameData& frame);

        void* FrameWaitable() const;        // HANDLE of the swap chain's frame-latency waitable object

        FK_PROFILE(FrameProfile& Profile() const;)

    private:
        struct Impl;
        Impl* State = nullptr;              // defined in Renderer.cpp
    };

    // ====================================================================================
    // Text — DirectWrite text layout + glyph atlas (Text.cpp)
    // ====================================================================================

    // The first fields of a GpuGlyph, relative to the layout: drawing adds the origin, color and clip.
    struct GlyphQuad
    {
        float Rect[4];              // DIPs (minX, minY, maxX, maxY) from the layout's top-left (pixel-snapped at DpiScale)
        uint32_t Texel0;            // atlas texels as in GpuGlyph (stay valid when the atlas grows)
        uint32_t Texel1;
    };

    struct TextLayout
    {
        GlyphQuad* Glyphs;
        uint32_t GlyphCount;
        Vec2 Size;                  // DIPs: widest line (with trailing spaces) × line heights
        float Baseline;             // DIPs from the top of the first line
        ClipRect Ink;               // DIPs: union of the quads (meaningless without glyphs)
    };

    class TextSystem
    {
    public:
        bool Init(std::string_view defaultFamily, Arena& scratch); // fails when the family is not installed
        void Shutdown();

        Font LoadFont(std::string_view family, Arena& scratch); // returns the default font on failure

        // Layout of the text (explicit line breaks, no wrapping). Never returns null: an empty layout is
        // returned on failure. Valid until the next EndFrame(). Simple text (Latin, Greek, Cyrillic,
        // punctuation) is laid out on every call without a cache; other text is shaped and cached while
        // used every frame. scratch holds temporaries.
        const TextLayout* Layout(std::string_view text, const TextStyle& style, float dpiScale, Arena& scratch);

        // Text drawn once: simple text goes straight into glyph instances (Out has room for a glyph per byte),
        // the same instances as Layout's quads placed at the origin. Returns null when it did (count glyphs,
        // covering ink, DIPs); otherwise the text needs shaping and its layout is returned, to draw as any other.
        struct GlyphTarget
        {
            GpuGlyph* Out;
            Vec2 Origin;            // DIPs, pixel-snapped
            uint32_t Color;         // packed
            uint32_t Clip;

            void operator()(uint32_t i, const GlyphQuad& q) const
            {
                GpuGlyph& g = Out[i];
                g.Rect[0] = Origin.X + q.Rect[0];
                g.Rect[1] = Origin.Y + q.Rect[1];
                g.Rect[2] = Origin.X + q.Rect[2];
                g.Rect[3] = Origin.Y + q.Rect[3];
                g.Texel0 = q.Texel0;
                g.Texel1 = q.Texel1;
                g.Color = Color;
                g.Clip = Clip;
            }
        };
        const TextLayout* LayoutInto(std::string_view text, const TextStyle& style, float dpiScale, Arena& scratch, const GlyphTarget& target,
                                     uint32_t& count, ClipRect& ink);

        // Layout(...)->Size; simple text is measured without building quads or rasterizing glyphs.
        Vec2 Measure(std::string_view text, const TextStyle& style, float dpiScale, Arena& scratch);

        // Pending atlas upload, if any. The pixels stay valid until the next Layout() call.
        bool TakeAtlasUpdate(AtlasUpdate& update);
        void InvalidateAtlas();     // the GPU copy is gone: the next update uploads the whole atlas

        // Ends the layouts' lifetime and frees shaped text not used this frame. A full atlas is reset
        // here, after the frame, so glyphs emitted earlier in a frame always stay valid.
        void EndFrame();

    private:
        struct Impl;
        Impl* State = nullptr;      // DirectWrite objects, fonts, glyph cache, shaped layout cache, atlas
    };

    // ====================================================================================
    // Ui — frame, ids, persistent widget state, layout, input routing (Ui.cpp, Layout.cpp, Draw.cpp)
    // ====================================================================================

    // Common layout fields collected from any props struct (see ToLayoutSpec).
    struct LayoutSpec
    {
        Thickness Margin;
        HorizontalAlignment HAlign = HorizontalAlignment::Stretch;
        VerticalAlignment VAlign = VerticalAlignment::Stretch;
        float Width = 0, Height = 0;
        float MinWidth = 0, MaxWidth = 0, MinHeight = 0, MaxHeight = 0;
    };

    template <class P>
    LayoutSpec ToLayoutSpec(const P& p)
    {
        LayoutSpec s;
        s.Margin = p.Margin;
        if constexpr (requires { p.HorizontalAlignment; })
            s.HAlign = p.HorizontalAlignment;
        if constexpr (requires { p.VerticalAlignment; })
            s.VAlign = p.VerticalAlignment;
        s.Width = p.Width;
        s.Height = p.Height;
        s.MinWidth = p.MinWidth;
        s.MinHeight = p.MinHeight;
        if constexpr (requires { p.MaxWidth; })
            s.MaxWidth = p.MaxWidth;
        if constexpr (requires { p.MaxHeight; })
            s.MaxHeight = p.MaxHeight;
        return s;
    }

    enum class ContainerKind : uint8_t
    {
        Root,
        Panel,
        Stack,
        Grid,
    };

    constexpr uint32_t MaxGridColumns = 8;
    constexpr uint32_t MaxGridRows = 64; // rows beyond this reuse the last row's remembered height

    // Persistent per-container data (keyed by container id). Never hold a pointer to it across
    // calls that may insert into the same map — look it up again by id.
    struct ContainerState
    {
        Vec2 ContentSize;                   // children extent measured last frame
        float ColumnWidths[MaxGridColumns]; // grid: widest Auto cell per column last frame
        float RowHeights[MaxGridRows];      // grid: tallest cell per row last frame
        uint32_t LastFrame;
    };

    // Persistent per-panel data (keyed by panel id).
    struct PanelState
    {
        Vec2 Position;          // top-left, DIPs
        Vec2 Size;              // outer size measured last frame
        Rect LastRect;          // rect drawn last frame (hit testing at BeginFrame)
        Vec2 DragOffset;        // pointer - position when the drag started
        float Opacity;          // props.Opacity × fade-in: the opacity of the panel's batches (BuildFrame)
        uint32_t LastFrame;     // frame the panel was last submitted
        uint32_t FirstFrame;    // frame it (re)appeared: that frame is an invisible sizing pass
        uint32_t FirstRun;      // its draw runs this frame lie in UiImpl::Runs[FirstRun, EndRun)
        uint32_t EndRun;
        bool Dragged;           // position was set by dragging (Anchor no longer applies; kept by GC)
    };

    // Open container during the frame.
    struct Container
    {
        ContainerKind Kind;
        Orientation Direction;
        uint64_t Id;
        uint32_t PositionalIndex;   // counter for children without an explicit key
        Rect Content;               // area for children
        float Spacing;
        float Cursor;               // next child offset along the main axis (from Content origin)
        Vec2 Measured;              // children extent this frame (from Content origin, desired sizes)
        LayoutSpec Spec;            // the container's own layout spec (used at EndScope)
        Rect Outer;                 // the container's own rect as placed at Begin
        uint32_t ClipIndex;         // clip rect active inside this container
        // Grid
        GridLength Columns[MaxGridColumns];
        uint32_t ColumnCount;
        uint32_t Column;                      // column of the next cell
        uint32_t Row;
        float ColumnX[MaxGridColumns];        // from Content.X
        float ColumnWidth[MaxGridColumns];
        float ColumnMeasured[MaxGridColumns]; // widest cell per column this frame
        float RowY;                           // top of the current row (from Content.Y)
        float RowHeight;                      // current row height: max(last frame's, tallest cell so far)
        float RowMeasured;                    // tallest cell of the current row this frame
        float RowSpacing;
        // Panel
        bool Movable;
    };

    // A contiguous range of shapes or glyphs emitted into one layer.
    struct DrawRun
    {
        uint64_t Layer;             // 0 = background, otherwise panel id; ForegroundLayer = on top of everything
        uint32_t Kind;              // BatchKind
        uint32_t First;             // into UiImpl::Shapes or UiImpl::Glyphs
        uint32_t Count;
        ClipRect Bounds;            // glyph runs: union of the glyph quads (BuildFrame's overlap test)
    };

    constexpr uint64_t ForegroundLayer = ~0ull;

    // A Transition. At rest (Progress >= 1) the value is exactly the target, recognized by its bits,
    // so a finished transition costs one lookup and one comparison. While moving, From and To are the
    // values as blended (colors: premultiplied linear RGBA).
    struct TransitionState
    {
        float From[4];
        float To[4];
        float Progress;         // 0..1 of the duration
        uint32_t LastFrame;     // frame it last advanced (at most once per frame)
        uint64_t Target;        // bits of the target value
    };

    static_assert(sizeof(TransitionState) == 48);

    struct UiImpl : Ui
    {
        bool Embedded = false;              // CreateEmbedded: no Host, the client's device and render target
        Host Window;
        Renderer Gpu;
        TextSystem Text;
        Arena Frame;                        // reset every BeginFrame
        Arena Scratch;                      // Init / LoadFont scratch, reset after each use

        // Frame
        uint32_t FrameIndex = 0;
        int64_t LastTicks = 0;
        int64_t TickFrequency = 0;
        float Dt = 0;
        Vec2 Viewport;                      // DIPs
        float Scale = 1;
        uint32_t SurfaceWidth = 0;          // physical pixels the renderer is sized for
        uint32_t SurfaceHeight = 0;
        bool PresentedLastFrame = false;
        bool ForceRedraw = true;            // present the next frame even if it equals the last one

        // Input (DIPs)
        InputEvents EmbeddedInput = {};     // embedded mode: fed by SetPointer (physical pixels, left button only)
        bool Interactive = false;
        Vec2 Pointer;
        Vec2 PreviousPointer;
        bool ButtonDown = false;            // left button
        bool ButtonPressed = false;         // went down this frame
        bool ButtonReleased = false;        // went up this frame
        uint64_t HoveredPanel = 0;          // topmost panel under the pointer (from last frame's rects), 0 = none
        uint64_t ActiveId = 0;              // widget or panel holding the left button
        bool ActiveIsPanelDrag = false;
        bool PointerOverUi = false;         // result for IsPointerOver() / click-through
        bool WidgetHoveredThisFrame = false;

        // Persistent state
        HashMap<PanelState> Panels;
        HashMap<ContainerState> ContainerStates;
        HashMap<TransitionState> Transitions;
        Array<uint64_t> PanelOrder;         // z-order, back = topmost

        // Per-frame draw data, in emission order. The GPU reads the instance arrays as they are: the
        // batches pick ranges of them in paint order.
        Array<Container> OpenContainers;    // [0] = Root (the viewport)
        Array<GpuShape> Shapes;
        Array<GpuGlyph> Glyphs;
        Array<DrawRun> Runs;
        Array<ClipRect> Clips;              // [0] = no clipping
        Array<Vec2> Points;                 // polyline vertices (ShapePolyline)
        uint64_t CurrentLayer = 0;
        uint32_t CurrentClip = 0;
        Array<DrawBatch> Batches;           // paint order, built by BuildFrame

        // Overlay: the last frame's arrays (swapped with the current ones at BeginFrame, nothing is copied).
        // A frame equal to the last one byte for byte is not presented.
        Array<GpuShape> PreviousShapes;
        Array<GpuGlyph> PreviousGlyphs;
        Array<ClipRect> PreviousClips;
        Array<Vec2> PreviousPoints;
        Array<DrawBatch> PreviousBatches;
        Vec2 PreviousViewport;
        float PreviousScale = 0;

        // --- Ui.cpp ----------------------------------------------------------------------
        bool Init(const OverlayDesc& desc);
        bool Init(const EmbeddedDesc& desc);
        bool InitCommon(std::string_view defaultFontFamily); // after the renderer: text, reserves, clock
        void Shutdown();
        Container& Top() { return OpenContainers.Back(); }
        uint64_t MakeId(Key key);           // HashCombine(parent id, key) or positional when key is none
        GpuShape* EmitShapes(uint32_t count); // zeroed shapes in the current layer run, TypeFlags = clip index (OR the type in); null on OOM
        // Glyphs: room for up to count at the end of Glyphs (null on OOM), then CommitGlyphs adds the
        // ones written (covering bounds, DIPs) to the current layer run.
        GpuGlyph* ReserveGlyphs(uint32_t count);
        void CommitGlyphs(uint32_t count, const ClipRect& bounds);
        bool IsSameFrame() const;           // overlay: this frame's data equals the last frame's
        void SetLayer(uint64_t layer);
        uint32_t PushClip(Rect rect);       // intersected with the current clip; returns its index
        Funky::Widget Interact(uint64_t id, Rect rect, bool enabled); // hover / press / click
        void BuildFrame();                  // runs → Batches in paint order
        void EndPanel();                    // EndScope of a panel: measure, drag, z-order

        // --- Layout.cpp ------------------------------------------------------------------
        Rect Place(Vec2 desired, const LayoutSpec& spec); // places a leaf in the current container
        bool BeginContainer(ContainerKind kind, uint64_t id, const LayoutSpec& spec); // false on OOM (nothing opened)
        void EndContainer();

        // --- Draw.cpp --------------------------------------------------------------------
        // Text laid out once by a control: measured by its Size, then drawn at the placed position.
        const TextLayout& LayoutText(std::string_view text, const TextStyle& style) { return *Text.Layout(text, style, Scale, Frame); }
        void DrawLayout(Vec2 position, const TextLayout& layout, const TextStyle& style);
    };

    inline UiImpl* Impl(Ui* ui) { return static_cast<UiImpl*>(ui); }
    inline const UiImpl* Impl(const Ui* ui) { return static_cast<const UiImpl*>(ui); }
}
