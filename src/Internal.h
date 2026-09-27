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
    // GPU shape instance — shared layout with src/Shaders/Shape.hlsl (struct Shape).
    // One instance = one screen-aligned quad; the pixel shader evaluates the shape's signed
    // distance function. All geometry is in DIPs; the shader converts to pixels with DpiScale.
    // ====================================================================================

    enum ShapeType : uint32_t
    {
        ShapeRoundRect = 0, // P0 = rect (minX, minY, maxX, maxY), P1 = radii (TL, TR, BR, BL)
        ShapePolyline = 1,  // P0 = (first point, point count, half thickness): round-joined, round-capped stroke through UiImpl::Points
        ShapeArc = 2,       // P0 = (cx, cy, radius, half thickness), P1 = (start angle, sweep angle) radians, clockwise from +X (y down), round caps
        ShapeTriangle = 3,  // P0 = (ax, ay, bx, by), P1 = (cx, cy); edges AA'd only if their Flag*Edge bit is set
        ShapeGlyph = 4,     // Bounds = glyph quad, P0 = atlas texel rect (x0, y0, x1, y1); coverage in atlas .r, distance field in .g
        ShapeImage = 5,     // stage 3: Bounds = image rect, P0 = UV, P1 = corner radii
    };

    enum ShapeFlags : uint32_t
    {
        FlagGradient = 1u << 8,  // Fill0 → Fill1 along the segment P2.xy → P2.zw (blended in linear space)
        FlagEdgeAB = 1u << 9,    // triangle edge a→b is an outer edge (antialiased)
        FlagEdgeBC = 1u << 10,
        FlagEdgeCA = 1u << 11,
    };

    constexpr uint32_t ShapeTypeMask = 0xFFu;
    constexpr uint32_t ShapeClipShift = 16; // bits 16..31 = index into the clip rect buffer (0 = none)

    struct GpuShape
    {
        float Bounds[4];     // quad to rasterize (minX, minY, maxX, maxY), already expanded for AA / glow / blur
        float P0[4];         // geometry, see ShapeType
        float P1[4];
        float P2[4];         // gradient start (xy) and end (zw) points
        uint32_t Fill0;      // packed Color (R in the lowest byte), sRGB, straight alpha
        uint32_t Fill1;      // gradient end color
        uint32_t Stroke;     // inner stroke color
        uint32_t Glow;       // glow color
        float StrokeWidth;   // DIPs, inner stroke (inside the edge)
        float GlowRadius;    // DIPs, glow falls off outside the edge
        float Softness;      // DIPs, > 0 = blurred silhouette (shadows): coverage uses a Gaussian of this sigma; stroke & glow ignored
        uint32_t TypeFlags;  // ShapeType | ShapeFlags | clipIndex << ShapeClipShift
    };

    static_assert(sizeof(GpuShape) == 96);

    struct ClipRect
    {
        float MinX, MinY, MaxX, MaxY; // DIPs
    };

    struct DrawBatch
    {
        uint32_t FirstShape;
        uint32_t ShapeCount;
        uint32_t Texture; // 0 = glyph atlas (images: stage 3)
    };

    constexpr uint32_t PackColor(Color c)
    {
        return uint32_t(c.R) | (uint32_t(c.G) << 8) | (uint32_t(c.B) << 16) | (uint32_t(c.A) << 24);
    }

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
    // Renderer — D3D11 + DirectComposition (Renderer.cpp, Shaders/Shape.hlsl).
    // Embedded mode: the client's device, no swap chain; draws into the view given by SetTarget.
    // ====================================================================================

    struct AtlasUpdate
    {
        const uint8_t* Pixels;  // R8G8 texels (coverage, distance), row pitch = Size * 2
        uint32_t Size;          // atlas width = height, texels
        uint32_t X, Y, Width, Height; // dirty region to upload
        bool Recreate;          // size changed or atlas reset: recreate the texture and upload everything
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
        // target and restores the device context state it changed. Shapes and points are in DIPs.
        // clips[0] must be the "no clipping" rect.
        bool Render(const GpuShape* shapes, uint32_t shapeCount,
                    const DrawBatch* batches, uint32_t batchCount,
                    const ClipRect* clips, uint32_t clipCount,
                    const Vec2* points, uint32_t pointCount, float dpiScale);

        void* FrameWaitable() const;        // HANDLE of the swap chain's frame-latency waitable object

    private:
        struct Impl;
        Impl* State = nullptr;              // defined in Renderer.cpp
    };

    // ====================================================================================
    // Text — DirectWrite shaping + glyph atlas (Text.cpp)
    // ====================================================================================

    struct GlyphQuad
    {
        float X, Y, Width, Height;  // DIPs, relative to the layout's top-left corner (pixel-snapped at DpiScale)
        float U0, V0, U1, V1;       // atlas texels (stay valid when the atlas grows)
    };

    struct TextLayout
    {
        GlyphQuad* Glyphs;
        uint32_t GlyphCount;
        Vec2 Size;                  // DIPs (DirectWrite layout width / height)
        float Baseline;             // DIPs from the top of the first line
    };

    class TextSystem
    {
    public:
        bool Init(std::string_view defaultFamily, Arena& scratch);
        void Shutdown();

        Font LoadFont(std::string_view family, Arena& scratch); // returns the default font on failure

        // Cached layout of the text (explicit '\n' line breaks, no wrapping). Never returns null:
        // an empty layout is returned on failure. Valid until the next EndFrame().
        const TextLayout* Layout(std::string_view text, const TextStyle& style, float dpiScale, uint32_t frame, Arena& scratch);

        // Pending atlas upload, if any. The pixels stay valid until the next Layout() call.
        bool TakeAtlasUpdate(AtlasUpdate& update);
        void InvalidateAtlas();     // the GPU copy is gone: the next update uploads the whole atlas

        // Evicts layouts and formats not used for a while. A full atlas is reset here, after the
        // frame, so glyphs emitted earlier in a frame always stay valid.
        void EndFrame(uint32_t frame);

    private:
        struct Impl;
        Impl* State = nullptr;      // DirectWrite objects, font families, glyph cache, layout cache, atlas
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
        float Opacity;          // props.Opacity × fade-in, applied to the panel's shapes by BuildFrame
        uint32_t LastFrame;     // frame the panel was last submitted
        uint32_t FirstFrame;    // frame it (re)appeared: that frame is an invisible sizing pass
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

    // A contiguous range of shapes emitted into one layer.
    struct ShapeRun
    {
        uint64_t Layer;             // 0 = background, otherwise panel id; ForegroundLayer = on top of everything
        uint32_t First;
        uint32_t Count;
    };

    constexpr uint64_t ForegroundLayer = ~0ull;

    struct TransitionState
    {
        float From[4];
        float To[4];
        float Current[4];
        float Elapsed;
        float Duration;
        uint32_t LastFrame;
    };

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
        uint64_t LastFrameHash = 0;
        bool ForceRedraw = true;

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

        // Per-frame draw data
        Array<Container> OpenContainers;    // [0] = Root (the viewport)
        Array<GpuShape> Shapes;             // in emission order
        Array<ShapeRun> Runs;
        Array<ClipRect> Clips;              // [0] = no clipping
        Array<Vec2> Points;                 // polyline vertices (ShapePolyline)
        uint64_t CurrentLayer = 0;
        uint32_t CurrentClip = 0;
        Array<GpuShape> FinalShapes;        // Shapes reordered by layer for rendering
        Array<DrawBatch> Batches;

        // --- Ui.cpp ----------------------------------------------------------------------
        bool Init(const OverlayDesc& desc);
        bool Init(const EmbeddedDesc& desc);
        bool InitCommon(std::string_view defaultFontFamily); // after the renderer: text, reserves, clock
        void Shutdown();
        Container& Top() { return OpenContainers.Back(); }
        uint64_t MakeId(Key key);           // HashCombine(parent id, key) or positional when key is none
        GpuShape* EmitShapes(uint32_t count); // zeroed shapes in the current layer run, TypeFlags = clip index (OR the type in); null on OOM
        void SetLayer(uint64_t layer);
        uint32_t PushClip(Rect rect);       // intersected with the current clip; returns its index
        Funky::Widget Interact(uint64_t id, Rect rect, bool enabled); // hover / press / click
        void BuildFrame();                  // runs → FinalShapes/Batches (applies panel opacity)
        void EndPanel();                    // EndScope of a panel: measure, drag, z-order

        // --- Layout.cpp ------------------------------------------------------------------
        Rect Place(Vec2 desired, const LayoutSpec& spec); // places a leaf in the current container
        bool BeginContainer(ContainerKind kind, uint64_t id, const LayoutSpec& spec); // false on OOM (nothing opened)
        void EndContainer();
    };

    inline UiImpl* Impl(Ui* ui) { return static_cast<UiImpl*>(ui); }
    inline const UiImpl* Impl(const Ui* ui) { return static_cast<const UiImpl*>(ui); }
}
