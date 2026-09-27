// Ui — lifetime, frame loop, ids, input routing, panels, transitions and frame assembly.

#include "Internal.h"

#include <new>

// The Win32 clock without <windows.h>: the core stays platform-light (see Internal.h).
extern "C" __declspec(dllimport) int QueryPerformanceCounter(union _LARGE_INTEGER* count);
extern "C" __declspec(dllimport) int QueryPerformanceFrequency(union _LARGE_INTEGER* frequency);

namespace Funky
{
    namespace
    {
        constexpr uint64_t RootId = 1;
        constexpr uint64_t PositionalSalt = 0xA0761D6478BD642Full; // keeps positional ids apart from Key(1), Key(2), ...
        constexpr uint32_t MaxUnseenFrames = 8;
        constexpr float MaxDeltaTime = 0.1f;
        constexpr float NoClip = 1e9f;
        constexpr float PanelFadeSeconds = 0.16f;
        constexpr uint64_t PanelFadeSlot = Key("PanelFade").Value;
        constexpr TextStyle PanelTitleStyle = { .FontWeight = FontWeight::SemiBold, .Foreground = Rgb(0xF5F7FA) };

        int64_t Ticks()
        {
            int64_t ticks = 0;
            QueryPerformanceCounter(reinterpret_cast<_LARGE_INTEGER*>(&ticks));
            return ticks;
        }

        // Drawn in the given frame and not as its invisible sizing pass.
        bool IsShown(const PanelState& panel, uint32_t frame)
        {
            return panel.LastFrame == frame && panel.FirstFrame != frame;
        }

        // Topmost panel shown in the given frame that contains the point, 0 = none.
        uint64_t PanelAt(UiImpl& ui, Vec2 point, uint32_t frame)
        {
            for (uint32_t i = ui.PanelOrder.Count; i-- > 0;)
            {
                const PanelState* panel = ui.Panels.Find(ui.PanelOrder[i]);
                if (panel && IsShown(*panel, frame) && panel->LastRect.Contains(point))
                    return ui.PanelOrder[i];
            }
            return 0;
        }

        void BringToTop(UiImpl& ui, uint64_t id)
        {
            for (uint32_t i = 0; i < ui.PanelOrder.Count; ++i)
            {
                if (ui.PanelOrder[i] == id)
                {
                    ui.PanelOrder.RemoveAt(i);
                    break;
                }
            }
            ui.PanelOrder.Push(id);
        }

        // Anchor aligns the panel inside the viewport deflated by the margin. The Anchor enum is
        // row-major (TopLeft, Top, TopRight, Left, ...): column and row pick 0, 1/2 or all of the free space.
        Vec2 AnchoredPosition(Anchor anchor, Thickness margin, Vec2 size, Vec2 viewport)
        {
            Rect area = Rect{ 0, 0, viewport.X, viewport.Y }.Deflate(margin);
            float column = float(uint32_t(anchor) % 3) * 0.5f;
            float row = float(uint32_t(anchor) / 3) * 0.5f;
            return { area.X + (area.Width - size.X) * column, area.Y + (area.Height - size.Y) * row };
        }

        // How each Transition type is stored: the target's bits (exact at rest) and the floats it blends as.
        template <class T>
        struct Blended;

        template <>
        struct Blended<float>
        {
            static constexpr uint32_t Count = 1;
            static uint64_t Bits(float v) { return std::bit_cast<uint32_t>(v); }
            static float FromBits(uint64_t bits) { return std::bit_cast<float>(uint32_t(bits)); }
            static void ToFloats(float v, float* out) { out[0] = v; }
            static float FromFloats(const float* v) { return v[0]; }
        };

        template <>
        struct Blended<Vec2>
        {
            static constexpr uint32_t Count = 2;
            static uint64_t Bits(Vec2 v) { return std::bit_cast<uint32_t>(v.X) | uint64_t(std::bit_cast<uint32_t>(v.Y)) << 32; }
            static Vec2 FromBits(uint64_t bits) { return { std::bit_cast<float>(uint32_t(bits)), std::bit_cast<float>(uint32_t(bits >> 32)) }; }
            static void ToFloats(Vec2 v, float* out) { out[0] = v.X; out[1] = v.Y; }
            static Vec2 FromFloats(const float* v) { return { v[0], v[1] }; }
        };

        // Colors move as premultiplied linear RGBA, like Lerp(Color, Color, float).
        template <>
        struct Blended<Color>
        {
            static constexpr uint32_t Count = 4;
            static uint64_t Bits(Color c) { return c.A ? PackColor(c) : 0; } // every transparent color is one target
            static Color FromBits(uint64_t bits) { return { uint8_t(bits), uint8_t(bits >> 8), uint8_t(bits >> 16), uint8_t(bits >> 24) }; }

            static void ToFloats(Color c, float* out) { ToPremultipliedLinear(c, out); }
            static Color FromFloats(const float* v) { return FromPremultipliedLinear(v); }
        };

        // Ease-out cubic from From to To at s.Progress.
        void Blend(const TransitionState& s, uint32_t count, float* out)
        {
            float u = 1 - s.Progress;
            float eased = 1 - u * u * u;
            for (uint32_t i = 0; i < count; ++i)
                out[i] = s.From[i] * (1 - eased) + s.To[i] * eased;
        }

        // Moves the transition stored under key towards target (advancing at most once per frame) and returns
        // its current value. A new transition, or one with no duration, is at its target right away.
        template <class T>
        T AnimateValue(UiImpl& ui, uint64_t key, T target, Duration duration)
        {
            using B = Blended<T>;
            bool isNew = false;
            TransitionState* s = ui.Transitions.FindOrAdd(key, &isNew);
            if (!s)
                return target; // out of memory: no animation
            uint64_t bits = B::Bits(target);
            if (isNew || duration.Seconds <= 0)
            {
                s->Target = bits;
                s->Progress = 1;
                s->LastFrame = ui.FrameIndex;
                return target;
            }
            if (bits != s->Target)
            {
                // A new target: move on from where the value is now.
                float current[4];
                if (s->Progress >= 1)
                    B::ToFloats(B::FromBits(s->Target), current);
                else
                    Blend(*s, B::Count, current);
                MemCopy(s->From, current, sizeof(float) * B::Count);
                B::ToFloats(target, s->To);
                s->Target = bits;
                s->Progress = 0;
            }
            if (s->LastFrame != ui.FrameIndex)
            {
                s->LastFrame = ui.FrameIndex;
                if (s->Progress < 1)
                    s->Progress = Min(s->Progress + ui.Dt / duration.Seconds, 1.0f);
            }
            if (s->Progress >= 1)
                return target; // at rest: exactly the target
            float value[4];
            Blend(*s, B::Count, value);
            return B::FromFloats(value);
        }

        void ReadInput(UiImpl& ui)
        {
            const InputEvents& input = ui.Embedded ? ui.EmbeddedInput : ui.Window.Input();
            ui.PreviousPointer = ui.Pointer;
            ui.Pointer = input.PointerPx / ui.Scale;
            ui.ButtonDown = ui.Interactive && input.ButtonDown[0];
            ui.ButtonPressed = ui.Interactive && input.Pressed[0] > 0;
            ui.ButtonReleased = ui.Interactive && input.Released[0] > 0;
            if (ui.Embedded)
                ui.EmbeddedInput.Pressed[0] = ui.EmbeddedInput.Released[0] = 0;
            else
                ui.Window.ClearInputEdges();

            if (!ui.Interactive)
            {
                ui.ActiveId = 0;
                ui.ActiveIsPanelDrag = false;
            }
            ui.HoveredPanel = ui.Interactive ? PanelAt(ui, ui.Pointer, ui.FrameIndex - 1) : 0;
        }

        // The run the next instances of this kind go to: the last one if it has the same layer and kind.
        DrawRun* CurrentRun(UiImpl& ui, uint32_t kind, uint32_t first)
        {
            if (!ui.Runs.IsEmpty() && ui.Runs.Back().Layer == ui.CurrentLayer && ui.Runs.Back().Kind == kind)
                return &ui.Runs.Back();
            return ui.Runs.Push({ ui.CurrentLayer, kind, first, 0, EmptyBounds });
        }

        // Appends a batch to a list, merging it into the last one when it continues that one.
        void AddBatch(DrawBatch* batches, uint32_t& count, const DrawBatch& batch)
        {
            if (batch.Count == 0)
                return;
            DrawBatch* last = count ? &batches[count - 1] : nullptr;
            if (last && last->Kind == batch.Kind && last->Opacity == batch.Opacity && last->First + last->Count == batch.First)
                last->Count += batch.Count;
            else
                batches[count++] = batch;
        }

        // Turns runs, fed in paint order, into batches by kind, like Zed's GPUI. The runs are cut into
        // levels; a level draws all its shapes, then all its glyphs. So text moves after the shapes fed
        // later in its level, which is fine as long as they do not overlap it: a shape overlapping text of
        // the current level (e.g. a highlight over a label, or a panel over another panel's text) starts
        // the next level. A panel of labels, sliders and buttons is usually one level: two draw calls.
        class Batcher
        {
        public:
            explicit Batcher(Array<DrawBatch>& batches) : Batches(batches) {}

            void AddShapes(const GpuShape* shapes, uint32_t first, uint32_t count, float opacity)
            {
                uint32_t start = first;
                for (uint32_t i = first; i < first + count && TextCount > 0; ++i)
                {
                    if (Overlaps(shapes[i].Bounds, TextUnion) && OverlapsText(shapes[i].Bounds))
                    {
                        Emit({ BatchShapes, start, i - start, opacity });
                        EndLevel();
                        start = i;
                    }
                }
                Emit({ BatchShapes, start, first + count - start, opacity });
            }

            void AddGlyphs(uint32_t first, uint32_t count, const ClipRect& bounds, float opacity)
            {
                if (TextCount == MaxLevelTexts)
                    EndLevel(); // bounds the overlap tests per shape
                Texts[TextCount++] = bounds;
                TextUnion = { Min(TextUnion.MinX, bounds.MinX), Min(TextUnion.MinY, bounds.MinY),
                              Max(TextUnion.MaxX, bounds.MaxX), Max(TextUnion.MaxY, bounds.MaxY) };
                AddBatch(Glyphs, GlyphCount, { BatchGlyphs, first, count, opacity });
            }

            void EndLevel()
            {
                for (uint32_t i = 0; i < GlyphCount; ++i)
                    Emit(Glyphs[i]);
                TextCount = GlyphCount = 0;
                TextUnion = EmptyBounds;
            }

        private:
            static constexpr uint32_t MaxLevelTexts = 32;

            // Touching counts as overlapping: a pixel on the shared edge must keep its order.
            static bool Overlaps(const float* bounds, const ClipRect& t)
            {
                return bounds[0] <= t.MaxX && t.MinX <= bounds[2] && bounds[1] <= t.MaxY && t.MinY <= bounds[3];
            }

            bool OverlapsText(const float* bounds) const
            {
                for (uint32_t i = 0; i < TextCount; ++i)
                    if (Overlaps(bounds, Texts[i]))
                        return true;
                return false;
            }

            void Emit(const DrawBatch& batch)
            {
                // Room for one more first: AddBatch then merges or appends.
                if (batch.Count > 0 && Batches.Reserve(Batches.Count + 1))
                    AddBatch(Batches.Data, Batches.Count, batch);
            }

            Array<DrawBatch>& Batches;
            ClipRect Texts[MaxLevelTexts];      // bounds of the level's glyph runs
            DrawBatch Glyphs[MaxLevelTexts];    // the level's glyph batches, drawn after its shapes
            ClipRect TextUnion = EmptyBounds;   // of Texts: most shapes are rejected by this one test
            uint32_t TextCount = 0;
            uint32_t GlyphCount = 0;
        };

        // The counts are compared first (see IsSameFrame).
        template <class T>
        bool SameData(const Array<T>& a, const Array<T>& b)
        {
            return MemEqual(a.Data, b.Data, sizeof(T) * a.Count);
        }

        template <class T>
        void Swap(T& a, T& b)
        {
            T t = a;
            a = b;
            b = t;
        }

        // Presents the frame unless it equals the last one (overlay). Any frame that is not presented for
        // another reason forces the next one, so "equal to the last frame" always means "already on screen".
        // An embedded target is redrawn by the client every frame, so it always gets the UI.
        void Present(UiImpl& ui)
        {
            if (!ui.Embedded)
            {
                ui.PresentedLastFrame = false;
                if (!ui.Window.IsVisible())
                {
                    ui.ForceRedraw = true;
                    return;
                }
                if (!ui.ForceRedraw && ui.IsSameFrame())
                    return;
            }
            FrameData frame = { ui.Shapes.Data, ui.Shapes.Count, ui.Glyphs.Data, ui.Glyphs.Count, ui.Points.Data, ui.Points.Count,
                                ui.Clips.Data, ui.Clips.Count, ui.Batches.Data, ui.Batches.Count, ui.Scale };
            ui.PresentedLastFrame = ui.Gpu.Render(frame);
            ui.ForceRedraw = !ui.PresentedLastFrame;
        }

        // Every MaxUnseenFrames frames: state unseen for that long is dropped (so it lives 8 to 16 frames).
        void CollectGarbage(UiImpl& ui)
        {
            uint32_t frame = ui.FrameIndex;
            if (frame % MaxUnseenFrames != 0)
                return;
            auto stale = [frame](uint32_t lastFrame) { return frame - lastFrame > MaxUnseenFrames; };
            ui.ContainerStates.RemoveIf([&](uint64_t, const ContainerState& s) { return stale(s.LastFrame); });
            ui.Transitions.RemoveIf([&](uint64_t, const TransitionState& s) { return stale(s.LastFrame); });
            // A dragged panel keeps its position for the whole session.
            ui.Panels.RemoveIf([&](uint64_t, const PanelState& s) { return stale(s.LastFrame) && !s.Dragged; });
            ui.ContainerStates.ShrinkIfSparse();
            ui.Transitions.ShrinkIfSparse();
            ui.Panels.ShrinkIfSparse();
            for (uint32_t i = 0; i < ui.PanelOrder.Count;)
            {
                if (ui.Panels.Find(ui.PanelOrder[i]))
                    ++i;
                else
                    ui.PanelOrder.RemoveAt(i);
            }
        }

        template <class Desc>
        Ui* CreateUi(const Desc& desc)
        {
            SetAllocator(desc.Allocator);
            void* memory = MemAlloc(sizeof(UiImpl));
            if (!memory)
                return nullptr;
            UiImpl* ui = new (memory) UiImpl();
            if (!ui->Init(desc))
            {
                ui->Destroy();
                return nullptr;
            }
            return ui;
        }
    }

    // ------------------------------------------------------------------------------------
    // Lifetime
    // ------------------------------------------------------------------------------------

    Ui* Ui::Create(const OverlayDesc& desc) { return CreateUi(desc); }
    Ui* Ui::CreateEmbedded(const EmbeddedDesc& desc) { return CreateUi(desc); }

    void Ui::Destroy()
    {
        UiImpl* ui = Impl(this);
        ui->Shutdown();
        ui->~UiImpl();
        MemFree(ui);
    }

    bool UiImpl::Init(const OverlayDesc& desc)
    {
        if (!Window.Init(static_cast<HWND__*>(desc.TargetWindow), desc.WindowClassName, Scratch))
            return false;
        SurfaceWidth = Max(Window.Width(), 1u);
        SurfaceHeight = Max(Window.Height(), 1u);
        Scale = Window.DpiScale();
        return Gpu.Init(Window.Window(), SurfaceWidth, SurfaceHeight) && InitCommon(desc.DefaultFontFamily);
    }

    // The size and scale come with SetRenderTarget.
    bool UiImpl::Init(const EmbeddedDesc& desc)
    {
        Embedded = true;
        return desc.Device && Gpu.Init(static_cast<ID3D11Device*>(desc.Device)) && InitCommon(desc.DefaultFontFamily);
    }

    bool UiImpl::InitCommon(std::string_view defaultFontFamily)
    {
        // The first push of every frame then never allocates (see BeginFrame).
        bool ok = Text.Init(defaultFontFamily, Scratch) && OpenContainers.Reserve(16) && Clips.Reserve(16);
        Scratch.Reset();
        Viewport = { float(SurfaceWidth) / Scale, float(SurfaceHeight) / Scale };
        QueryPerformanceFrequency(reinterpret_cast<_LARGE_INTEGER*>(&TickFrequency));
        LastTicks = Ticks();
        return ok;
    }

    void UiImpl::Shutdown()
    {
        Text.Shutdown();
        Gpu.Shutdown();
        Window.Shutdown();
        Panels.Free();
        ContainerStates.Free();
        Transitions.Free();
        PanelOrder.Free();
        OpenContainers.Free();
        Shapes.Free();
        Glyphs.Free();
        Runs.Free();
        Clips.Free();
        Points.Free();
        Batches.Free();
        PreviousShapes.Free();
        PreviousGlyphs.Free();
        PreviousClips.Free();
        PreviousPoints.Free();
        PreviousBatches.Free();
        Frame.Free();
        Scratch.Free();
    }

    // ------------------------------------------------------------------------------------
    // Frame
    // ------------------------------------------------------------------------------------

    bool Ui::BeginFrame()
    {
        UiImpl& ui = *Impl(this);
        // Embedded: the client paces the frames and gives the size and scale with SetRenderTarget.
        if (!ui.Embedded)
        {
            ui.Window.WaitForFrame(ui.Gpu.FrameWaitable(), ui.PresentedLastFrame);
            if (!ui.Window.Update())
                return false;

            // The last frame becomes the previous one; its buffers are reused for this one.
            Swap(ui.Shapes, ui.PreviousShapes);
            Swap(ui.Glyphs, ui.PreviousGlyphs);
            Swap(ui.Clips, ui.PreviousClips);
            Swap(ui.Points, ui.PreviousPoints);
            Swap(ui.Batches, ui.PreviousBatches);
            ui.PreviousViewport = ui.Viewport;
            ui.PreviousScale = ui.Scale;

            uint32_t width = Max(ui.Window.Width(), 1u);
            uint32_t height = Max(ui.Window.Height(), 1u);
            if (width != ui.SurfaceWidth || height != ui.SurfaceHeight)
            {
                ui.Gpu.Resize(width, height);
                ui.SurfaceWidth = width;
                ui.SurfaceHeight = height;
                ui.ForceRedraw = true;
            }
            ui.Scale = ui.Window.DpiScale();
        }

        ++ui.FrameIndex;
        int64_t now = Ticks();
        ui.Dt = Clamp(float(double(now - ui.LastTicks) / double(ui.TickFrequency)), 0.0f, MaxDeltaTime);
        ui.LastTicks = now;
        ui.Viewport = { float(ui.SurfaceWidth) / ui.Scale, float(ui.SurfaceHeight) / ui.Scale };

        ReadInput(ui);

        ui.Frame.Reset();
        ui.Shapes.Clear();
        ui.Glyphs.Clear();
        ui.Runs.Clear();
        ui.Clips.Clear();
        ui.Clips.Push({ -NoClip, -NoClip, NoClip, NoClip });
        ui.Points.Clear();
        ui.CurrentClip = 0;
        ui.CurrentLayer = 0;
        ui.WidgetHoveredThisFrame = false;

        Container root = {};
        root.Kind = ContainerKind::Root;
        root.Direction = Orientation::Vertical;
        root.Id = RootId;
        root.Content = root.Outer = { 0, 0, ui.Viewport.X, ui.Viewport.Y };
        ui.OpenContainers.Clear();
        ui.OpenContainers.Push(root);
        return true;
    }

    void Ui::EndFrame()
    {
        UiImpl& ui = *Impl(this);
        FK_ASSERT(ui.OpenContainers.Count == 1); // every scope is closed

        // Released buttons end the capture only now, after the widgets have seen the release.
        if (!ui.ButtonDown)
        {
            ui.ActiveId = 0;
            ui.ActiveIsPanelDrag = false;
        }
        ui.PointerOverUi = ui.Interactive && (ui.ActiveId != 0 || ui.WidgetHoveredThisFrame || PanelAt(ui, ui.Pointer, ui.FrameIndex) != 0);
        if (!ui.Embedded) // embedded: the client routes the input by IsPointerOver()
        {
            ui.Window.SetClickThrough(!ui.PointerOverUi);
            ui.Window.SetPointerCapture(ui.ActiveId != 0);
        }

        FK_PROFILE(FrameProfile& profile = ui.Gpu.Profile(); profile = {}; int64_t buildStart = ProfileTicks();)
        ui.BuildFrame();
        FK_PROFILE(profile.Build = ProfileTicks() - buildStart;)

        // A recreated device starts without the atlas; a changed atlas changes the pixels.
        if (ui.Gpu.RestoreDevice())
        {
            ui.Text.InvalidateAtlas();
            ui.ForceRedraw = true;
        }
        AtlasUpdate atlas = {};
        if (ui.Text.TakeAtlasUpdate(atlas))
        {
            ui.Gpu.UpdateAtlas(atlas);
            ui.ForceRedraw = true;
        }
        Present(ui);
        CollectGarbage(ui);
        ui.Text.EndFrame();
    }

    // The batches in paint order: background, panels by z-order, foreground. The GPU reads Shapes and
    // Glyphs as emitted, so nothing is copied; a panel's opacity goes with its batches.
    void UiImpl::BuildFrame()
    {
        Batches.Clear();
        Batcher batcher(Batches);
        auto layer = [&](uint64_t id, float opacity, uint32_t firstRun, uint32_t endRun)
        {
            for (uint32_t i = firstRun; i < endRun; ++i)
            {
                const DrawRun& run = Runs[i];
                if (run.Layer != id)
                    continue;
                if (run.Kind == BatchShapes)
                    batcher.AddShapes(Shapes.Data, run.First, run.Count, opacity);
                else
                    batcher.AddGlyphs(run.First, run.Count, run.Bounds, opacity);
            }
        };

        layer(0, 1, 0, Runs.Count);
        for (uint64_t id : PanelOrder)
        {
            // Skipped: panels not submitted this frame, and a panel's invisible sizing pass (opacity 0).
            const PanelState* panel = Panels.Find(id);
            if (panel && panel->LastFrame == FrameIndex && panel->Opacity > 0)
                layer(id, panel->Opacity, panel->FirstRun, panel->EndRun);
        }
        layer(ForegroundLayer, 1, 0, Runs.Count);
        batcher.EndLevel();
    }

    // Everything that decides the pixels: the viewport, the paint order with the panels' opacity (both in
    // the batches) and the instance data. A changed frame usually differs within the first bytes.
    bool UiImpl::IsSameFrame() const
    {
        return Viewport == PreviousViewport && Scale == PreviousScale && Batches.Count == PreviousBatches.Count &&
               Clips.Count == PreviousClips.Count && Points.Count == PreviousPoints.Count && Shapes.Count == PreviousShapes.Count &&
               Glyphs.Count == PreviousGlyphs.Count && SameData(Batches, PreviousBatches) && SameData(Clips, PreviousClips) &&
               SameData(Points, PreviousPoints) && SameData(Shapes, PreviousShapes) && SameData(Glyphs, PreviousGlyphs);
    }

#if defined(FUNKY_PROFILE)
    const FrameProfile& GetFrameProfile(const Ui* ui)
    {
        return Impl(ui)->Gpu.Profile();
    }
#endif

    // ------------------------------------------------------------------------------------
    // Input & frame info
    // ------------------------------------------------------------------------------------

    void Ui::SetRenderTarget(void* renderTargetView, uint32_t widthPx, uint32_t heightPx, float dpiScale)
    {
        UiImpl& ui = *Impl(this);
        if (!ui.Embedded)
            return;
        ui.SurfaceWidth = Max(widthPx, 1u);
        ui.SurfaceHeight = Max(heightPx, 1u);
        ui.Scale = dpiScale > 0 ? dpiScale : 1;
        ui.Gpu.SetTarget(static_cast<ID3D11RenderTargetView*>(renderTargetView), ui.SurfaceWidth, ui.SurfaceHeight);
    }

    // Edges are counted here: a client that calls it for every mouse event also keeps clicks shorter than a frame.
    void Ui::SetPointer(Vec2 positionPx, bool leftButtonDown)
    {
        InputEvents& input = Impl(this)->EmbeddedInput;
        input.PointerPx = positionPx;
        if (leftButtonDown != input.ButtonDown[0])
            ++(leftButtonDown ? input.Pressed[0] : input.Released[0]);
        input.ButtonDown[0] = leftButtonDown;
    }

    void Ui::SetInteractive(bool interactive) { Impl(this)->Interactive = interactive; }
    bool Ui::IsInteractive() const { return Impl(this)->Interactive; }
    bool Ui::IsPointerOver() const { return Impl(this)->PointerOverUi; }
    bool Ui::IsKeyboardFocused() const { return false; } // stage 2: TextBox
    Vec2 Ui::ViewportSize() const { return Impl(this)->Viewport; }
    Vec2 Ui::PointerPosition() const { return Impl(this)->Pointer; }
    float Ui::DeltaTime() const { return Impl(this)->Dt; }
    float Ui::DpiScale() const { return Impl(this)->Scale; }

    Font Ui::LoadFont(std::string_view family)
    {
        UiImpl& ui = *Impl(this);
        Font font = ui.Text.LoadFont(family, ui.Scratch);
        ui.Scratch.Reset();
        return font;
    }

    Vec2 Ui::MeasureText(std::string_view text, const TextStyle& style)
    {
        UiImpl& ui = *Impl(this);
        return ui.Text.Measure(text, style, ui.Scale, ui.Frame);
    }

    // ------------------------------------------------------------------------------------
    // Ids, layers, clips, interaction
    // ------------------------------------------------------------------------------------

    uint64_t UiImpl::MakeId(Key key)
    {
        Container& parent = Top();
        return HashCombine(parent.Id, key.IsNone() ? (++parent.PositionalIndex ^ PositionalSalt) : key.Value);
    }

    GpuShape* UiImpl::EmitShapes(uint32_t count)
    {
        DrawRun* run = CurrentRun(*this, BatchShapes, Shapes.Count);
        GpuShape* shapes = run ? Shapes.Append(count) : nullptr;
        if (!shapes)
            return nullptr;
        run->Count += count;
        // Zeroed per element: fixed-size stores, no memset call.
        for (uint32_t i = 0; i < count; ++i)
        {
            shapes[i] = GpuShape{};
            shapes[i].TypeFlags = CurrentClip << ShapeClipShift;
        }
        return shapes;
    }

    GpuGlyph* UiImpl::ReserveGlyphs(uint32_t count)
    {
        return Glyphs.Reserve(Glyphs.Count + count) ? Glyphs.Data + Glyphs.Count : nullptr;
    }

    void UiImpl::CommitGlyphs(uint32_t count, const ClipRect& bounds)
    {
        FK_ASSERT(Glyphs.Count + count <= Glyphs.Capacity);
        DrawRun* run = count ? CurrentRun(*this, BatchGlyphs, Glyphs.Count) : nullptr;
        if (!run)
            return;
        Glyphs.Count += count;
        run->Count += count;
        run->Bounds = { Min(run->Bounds.MinX, bounds.MinX), Min(run->Bounds.MinY, bounds.MinY),
                        Max(run->Bounds.MaxX, bounds.MaxX), Max(run->Bounds.MaxY, bounds.MaxY) };
    }

    void UiImpl::SetLayer(uint64_t layer)
    {
        CurrentLayer = layer;
    }

    uint32_t UiImpl::PushClip(Rect rect)
    {
        const ClipRect& parent = Clips[CurrentClip];
        ClipRect clip = { Max(rect.Left(), parent.MinX), Max(rect.Top(), parent.MinY),
                          Min(rect.Right(), parent.MaxX), Min(rect.Bottom(), parent.MaxY) };
        // The clip index has 16 bits in GpuShape::TypeFlags.
        if (Clips.Count <= 0xFFFF && Clips.Push(clip))
            CurrentClip = Clips.Count - 1;
        return CurrentClip;
    }

    Funky::Widget UiImpl::Interact(uint64_t id, Rect rect, bool enabled)
    {
        Funky::Widget w;
        w.Id = { id };
        w.Rect = rect;
        w.IsEnabled = enabled;

        const ClipRect& clip = Clips[CurrentClip];
        bool inClip = Pointer.X >= clip.MinX && Pointer.Y >= clip.MinY && Pointer.X < clip.MaxX && Pointer.Y < clip.MaxY;
        // The widget's layer must be the topmost panel under the pointer (background: no panel there).
        w.Hovered = Interactive && enabled && inClip && rect.Contains(Pointer) && CurrentLayer == HoveredPanel &&
                    (ActiveId == 0 || ActiveId == id);
        bool activated = w.Hovered && ButtonPressed && ActiveId == 0;
        if (activated)
        {
            ActiveId = id;
            ActiveIsPanelDrag = false;
        }
        w.Pressed = ActiveId == id && ButtonDown;
        w.Clicked = ButtonReleased && ActiveId == id && w.Hovered;
        if (w.Pressed && !activated) // movement before the press is not a drag
            w.DragDelta = Pointer - PreviousPointer;
        WidgetHoveredThisFrame |= w.Hovered;
        return w;
    }

    Funky::Widget Ui::Widget(Key key, const WidgetProps& props)
    {
        UiImpl& ui = *Impl(this);
        uint64_t id = ui.MakeId(key);
        // WidgetProps::Width/Height are the desired size, not an override: Stretch still fills the slot.
        LayoutSpec spec = ToLayoutSpec(props);
        spec.Width = spec.Height = 0;
        return ui.Interact(id, ui.Place({ props.Width, props.Height }, spec), props.IsEnabled);
    }

    // ------------------------------------------------------------------------------------
    // Transitions
    // ------------------------------------------------------------------------------------

    float Ui::Transition(const Funky::Widget& widget, Key slot, float target, Duration duration)
    {
        return AnimateValue(*Impl(this), HashCombine(widget.Id.Value, slot.Value), target, duration);
    }

    Vec2 Ui::Transition(const Funky::Widget& widget, Key slot, Vec2 target, Duration duration)
    {
        return AnimateValue(*Impl(this), HashCombine(widget.Id.Value, slot.Value), target, duration);
    }

    Color Ui::Transition(const Funky::Widget& widget, Key slot, Color target, Duration duration)
    {
        return AnimateValue(*Impl(this), HashCombine(widget.Id.Value, slot.Value), target, duration);
    }

    float Ui::Transition(Key key, float target, Duration duration)
    {
        UiImpl& ui = *Impl(this);
        return AnimateValue(ui, HashCombine(ui.Top().Id, key.Value), target, duration);
    }

    Vec2 Ui::Transition(Key key, Vec2 target, Duration duration)
    {
        UiImpl& ui = *Impl(this);
        return AnimateValue(ui, HashCombine(ui.Top().Id, key.Value), target, duration);
    }

    Color Ui::Transition(Key key, Color target, Duration duration)
    {
        UiImpl& ui = *Impl(this);
        return AnimateValue(ui, HashCombine(ui.Top().Id, key.Value), target, duration);
    }

    // ------------------------------------------------------------------------------------
    // Panels
    // ------------------------------------------------------------------------------------

    Scope Ui::Panel(Key key, const PanelProps& props)
    {
        UiImpl& ui = *Impl(this);
        FK_ASSERT(ui.CurrentLayer == 0); // panels cannot nest
        if (ui.CurrentLayer != 0)
            return Scope(nullptr, false);

        uint64_t id = HashCombine(RootId, key.Value);
        bool isNew = false;
        PanelState* state = ui.Panels.FindOrAdd(id, &isNew);
        if (!state)
            return Scope(nullptr, false);
        FK_ASSERT(isNew || state->LastFrame != ui.FrameIndex); // panel keys are unique per frame
        if (isNew || state->LastFrame + 1 != ui.FrameIndex)
        {
            // (Re)appearing: this frame is the invisible sizing pass, and the panel opens on top.
            state->FirstFrame = ui.FrameIndex;
            BringToTop(ui, id);
        }
        state->LastFrame = ui.FrameIndex;
        bool sizing = state->FirstFrame == ui.FrameIndex;

        const TextLayout* titleLayout = props.Title.empty() ? nullptr : &ui.LayoutText(props.Title, PanelTitleStyle);
        Vec2 title = titleLayout ? titleLayout->Size : Vec2{};
        float titleBar = props.Title.empty() ? 0 : title.Y + props.Spacing;

        // Explicit or last frame's measured size, rounded out to whole pixels.
        float scale = ui.Scale;
        Vec2 size = { props.Width > 0 ? props.Width : state->Size.X, props.Height > 0 ? props.Height : state->Size.Y };
        size = { Ceil(Max(size.X, props.MinWidth) * scale) / scale, Ceil(Max(size.Y, props.MinHeight) * scale) / scale };

        // Client-owned, remembered after a drag, or anchored; keeps the grab point under the pointer while dragged.
        bool dragging = ui.ActiveIsPanelDrag && ui.ActiveId == id;
        Vec2 position = dragging ? ui.Pointer - state->DragOffset
                        : props.Position ? *props.Position
                        : state->Dragged ? state->Position
                                         : AnchoredPosition(props.Anchor, props.Margin, size, ui.Viewport);
        position.X = Round(Clamp(position.X, 0.0f, Max(ui.Viewport.X - size.X, 0.0f)) * scale) / scale;
        position.Y = Round(Clamp(position.Y, 0.0f, Max(ui.Viewport.Y - size.Y, 0.0f)) * scale) / scale;
        if (dragging)
        {
            if (props.Position)
                *props.Position = position;
            else
                state->Dragged = true;
        }
        state->Position = position;

        float fade = AnimateValue(ui, HashCombine(id, PanelFadeSlot), sizing ? 0.0f : 1.0f, { sizing ? 0 : PanelFadeSeconds });
        state->Opacity = Saturate(props.Opacity) * fade;

        Rect rect = { position.X, position.Y, size.X, size.Y };
        state->FirstRun = state->EndRun = ui.Runs.Count; // the panel's runs start here (see BuildFrame)
        ui.SetLayer(id);
        DrawRect(rect, { .Fill = props.Background, .Stroke = props.BorderBrush, .StrokeThickness = props.BorderThickness,
                         .CornerRadius = props.CornerRadius, .Glow = props.Glow, .Shadow = props.Shadow });

        Container panel = {};
        panel.Kind = ContainerKind::Panel;
        panel.Direction = Orientation::Vertical;
        panel.Id = id;
        panel.Outer = rect;
        panel.Content = rect.Deflate(props.Padding);
        panel.Spacing = props.Spacing;
        panel.ClipIndex = ui.PushClip(rect);
        panel.Movable = props.Movable;
        if (!props.Title.empty())
        {
            ui.DrawLayout(panel.Content.Position(), *titleLayout, PanelTitleStyle);
            panel.Content.Y += titleBar;
            panel.Content.Height -= titleBar;
            panel.Measured.X = title.X;
        }
        if (!ui.OpenContainers.Push(panel))
        {
            state->EndRun = ui.Runs.Count;
            ui.SetLayer(0);
            ui.CurrentClip = ui.Top().ClipIndex;
            return Scope(nullptr, false);
        }
        return Scope(this, true);
    }

    void UiImpl::EndPanel()
    {
        Container panel = Top();
        OpenContainers.Pop();
        SetLayer(0); // panels do not nest: back to the background
        CurrentClip = Top().ClipIndex;

        PanelState* state = Panels.Find(panel.Id);
        if (!state)
            return;
        state->EndRun = Runs.Count;
        state->Size = panel.Measured + (panel.Outer.Size() - panel.Content.Size()); // content + padding + title bar
        state->LastRect = panel.Outer;

        // A press on the panel that no child took is captured by it (and drags it if movable), so the
        // target never sees half of a click; any press raises the panel.
        if (!ButtonPressed || HoveredPanel != panel.Id)
            return;
        if (ActiveId == 0)
        {
            ActiveId = panel.Id;
            ActiveIsPanelDrag = panel.Movable;
            state->DragOffset = Pointer - panel.Outer.Position();
        }
        BringToTop(*this, panel.Id);
    }
}
