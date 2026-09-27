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

        // Moves the transition stored under key towards value[0..count) and writes its current value back.
        void Animate(UiImpl& ui, uint64_t key, float* value, uint32_t count, float duration)
        {
            bool isNew = false;
            TransitionState* s = ui.Transitions.FindOrAdd(key, &isNew);
            if (!s)
                return; // out of memory: the target is returned as is
            size_t bytes = sizeof(float) * count;

            if (isNew || duration <= 0)
            {
                MemCopy(s->From, value, bytes);
                MemCopy(s->To, value, bytes);
                MemCopy(s->Current, value, bytes);
                s->Elapsed = 0;
            }
            else
            {
                if (!MemEqual(s->To, value, bytes))
                {
                    MemCopy(s->From, s->Current, bytes);
                    MemCopy(s->To, value, bytes);
                    s->Elapsed = 0;
                }
                if (s->LastFrame != ui.FrameIndex)
                {
                    s->Elapsed = Min(s->Elapsed + ui.Dt, duration);
                    float u = 1 - s->Elapsed / duration;
                    float eased = 1 - u * u * u;
                    for (uint32_t i = 0; i < count; ++i)
                        s->Current[i] = s->From[i] * (1 - eased) + s->To[i] * eased; // exactly To when done
                }
            }
            s->Duration = duration;
            s->LastFrame = ui.FrameIndex;
            MemCopy(value, s->Current, bytes);
        }

        float AnimateValue(UiImpl& ui, uint64_t key, float target, Duration duration)
        {
            Animate(ui, key, &target, 1, duration.Seconds);
            return target;
        }

        Vec2 AnimateValue(UiImpl& ui, uint64_t key, Vec2 target, Duration duration)
        {
            float value[2] = { target.X, target.Y };
            Animate(ui, key, value, 2, duration.Seconds);
            return { value[0], value[1] };
        }

        // Colors move as premultiplied linear RGBA, like Lerp(Color, Color, float).
        Color AnimateValue(UiImpl& ui, uint64_t key, Color target, Duration duration)
        {
            LinearColor c = ToLinear(target);
            float value[4] = { c.R * c.A, c.G * c.A, c.B * c.A, c.A };
            Animate(ui, key, value, 4, duration.Seconds);
            if (value[3] <= 0)
                return Colors::Transparent;
            float inverse = 1 / value[3];
            return FromLinear({ value[0] * inverse, value[1] * inverse, value[2] * inverse, value[3] });
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

        uint32_t ScaleAlpha(uint32_t color, float opacity)
        {
            return (color & 0x00FFFFFFu) | (uint32_t(float(color >> 24) * opacity + 0.5f) << 24);
        }

        // Appends the runs of one layer to FinalShapes (reserved by BuildFrame).
        void AppendLayer(UiImpl& ui, uint64_t layer, float opacity)
        {
            if (opacity <= 0)
                return; // e.g. a panel's sizing pass
            for (const ShapeRun& run : ui.Runs)
            {
                if (run.Layer != layer || run.Count == 0)
                    continue;
                GpuShape* out = ui.FinalShapes.Append(run.Count);
                MemCopy(out, ui.Shapes.Data + run.First, sizeof(GpuShape) * run.Count);
                if (opacity >= 1)
                    continue;
                for (uint32_t i = 0; i < run.Count; ++i)
                {
                    GpuShape& s = out[i];
                    s.Fill0 = ScaleAlpha(s.Fill0, opacity);
                    s.Fill1 = ScaleAlpha(s.Fill1, opacity);
                    s.Stroke = ScaleAlpha(s.Stroke, opacity);
                    s.Glow = ScaleAlpha(s.Glow, opacity);
                }
            }
        }

        // Presents the frame unless it is identical to the last presented one. An embedded target is
        // redrawn by the client every frame, so it always gets the UI.
        void Present(UiImpl& ui)
        {
            uint64_t hash = 0;
            if (!ui.Embedded)
            {
                float view[3] = { ui.Viewport.X, ui.Viewport.Y, ui.Scale };
                hash = HashMemory(ui.FinalShapes.Data, sizeof(GpuShape) * ui.FinalShapes.Count);
                hash = HashMemory(ui.Clips.Data, sizeof(ClipRect) * ui.Clips.Count, hash);
                hash = HashMemory(ui.Points.Data, sizeof(Vec2) * ui.Points.Count, hash);
                hash = HashMemory(ui.Batches.Data, sizeof(DrawBatch) * ui.Batches.Count, hash);
                hash = HashMemory(view, sizeof(view), hash);

                ui.PresentedLastFrame = false;
                if (!ui.Window.IsVisible() || (hash == ui.LastFrameHash && !ui.ForceRedraw))
                    return;
            }
            ui.PresentedLastFrame = ui.Gpu.Render(ui.FinalShapes.Data, ui.FinalShapes.Count, ui.Batches.Data, ui.Batches.Count,
                                                  ui.Clips.Data, ui.Clips.Count, ui.Points.Data, ui.Points.Count, ui.Scale);
            if (ui.PresentedLastFrame)
            {
                ui.LastFrameHash = hash;
                ui.ForceRedraw = false;
            }
        }

        void CollectGarbage(UiImpl& ui)
        {
            uint32_t frame = ui.FrameIndex;
            auto stale = [frame](uint32_t lastFrame) { return frame - lastFrame > MaxUnseenFrames; };
            ui.ContainerStates.RemoveIf([&](uint64_t, const ContainerState& s) { return stale(s.LastFrame); });
            ui.Transitions.RemoveIf([&](uint64_t, const TransitionState& s) { return stale(s.LastFrame); });
            // A dragged panel keeps its position for the whole session.
            ui.Panels.RemoveIf([&](uint64_t, const PanelState& s) { return stale(s.LastFrame) && !s.Dragged; });
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
        Runs.Free();
        Clips.Free();
        Points.Free();
        FinalShapes.Free();
        Batches.Free();
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

        ui.BuildFrame();
        // A recreated device starts without the atlas.
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
        ui.Text.EndFrame(ui.FrameIndex);
    }

    void UiImpl::BuildFrame()
    {
        FinalShapes.Clear();
        Batches.Clear();
        if (!FinalShapes.Reserve(Shapes.Count))
            return;
        AppendLayer(*this, 0, 1);
        for (uint64_t id : PanelOrder)
            if (const PanelState* panel = Panels.Find(id))
                AppendLayer(*this, id, panel->Opacity);
        AppendLayer(*this, ForegroundLayer, 1);
        if (FinalShapes.Count)
            Batches.Push({ 0, FinalShapes.Count, 0 });
    }

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
        return ui.Text.Layout(text, style, ui.Scale, ui.FrameIndex, ui.Frame)->Size;
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
        if ((Runs.IsEmpty() || Runs.Back().Layer != CurrentLayer) && !Runs.Push({ CurrentLayer, Shapes.Count, 0 }))
            return nullptr;
        GpuShape* shapes = Shapes.Append(count);
        if (!shapes)
            return nullptr;
        Runs.Back().Count += count;
        MemZero(shapes, sizeof(GpuShape) * count);
        for (uint32_t i = 0; i < count; ++i)
            shapes[i].TypeFlags = CurrentClip << ShapeClipShift;
        return shapes;
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

        Vec2 title = props.Title.empty() ? Vec2{} : MeasureText(props.Title, PanelTitleStyle);
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
            DrawString(panel.Content.Position(), props.Title, PanelTitleStyle);
            panel.Content.Y += titleBar;
            panel.Content.Height -= titleBar;
            panel.Measured.X = title.X;
        }
        if (!ui.OpenContainers.Push(panel))
        {
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
