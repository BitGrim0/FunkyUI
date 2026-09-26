// FunkyUI demo: a stand-in "game" window (painted with GDI) and an overlay on top of it.
//
//   Insert       toggles menu mode (the Settings panel appears, panels become interactive).
//   Esc          closes the demo.
//
// The demo builds and links without any CRT: no printf / sinf / rand, no global objects with
// constructors. Everything it needs beyond Windows comes from FunkyUI.h.

#include <FunkyUI.h>

#include <windows.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

using namespace Funky;
using namespace Funky::Literals;

namespace
{
    constexpr Color Accent = Rgb(0x3AA8FF);
    constexpr Color AccentDeep = Rgb(0x2F7BFF);
    constexpr Color Dim = Rgb(0x8A90A2);

    struct Settings
    {
        float Speed = 1;
        float GlowRadius = 12;
        float LineWidth = 3;
        float RingRadius = 34;
        bool ShowRoute = true;
        bool ShowRing = true;
        bool Paused = false;
        bool ShowStats = true;
        Vec2 StatsPosition = { 16, 16 }; // client-owned panel position: dragging writes it, Reset restores it
    };

    // --------------------------------------------------------------------------------------
    // Small no-CRT helpers
    // --------------------------------------------------------------------------------------

    // Smooth periodic wave in [-1, 1] with period 1: a parabolic sine approximation (no sinf without the CRT).
    float Wave(float t)
    {
        float x = t - float(int(t));
        if (x < 0)
            x += 1;
        return x < 0.5f ? 16 * x * (0.5f - x) : -16 * (x - 0.5f) * (1 - x);
    }

    // Fixed-size text builder instead of snprintf.
    struct TextBuffer
    {
        char Data[48];
        uint32_t Length = 0;

        TextBuffer& Add(std::string_view text)
        {
            for (char c : text)
                if (Length < sizeof(Data))
                    Data[Length++] = c;
            return *this;
        }

        TextBuffer& Add(int value)
        {
            char digits[12];
            int count = 0;
            unsigned v = value < 0 ? 0u - unsigned(value) : unsigned(value);
            do
            {
                digits[count++] = char('0' + v % 10);
                v /= 10;
            } while (v);
            if (value < 0)
                Add("-");
            while (count)
                Add({ &digits[--count], 1 });
            return *this;
        }

        std::string_view View() const { return { Data, Length }; }
    };

    Vec2 Bezier(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, float t)
    {
        float u = 1 - t;
        return p0 * (u * u * u) + p1 * (3 * u * u * t) + p2 * (3 * u * t * t) + p3 * (t * t * t);
    }

    // --------------------------------------------------------------------------------------
    // A custom control: a toggle switch built only from Widget + Transition + Draw*.
    // --------------------------------------------------------------------------------------

    bool ToggleSwitch(Ui& ui, std::string_view text, bool& value)
    {
        constexpr float TrackWidth = 34;
        constexpr float TrackHeight = 18;
        constexpr float Gap = 8;

        TextStyle textStyle = {};
        Vec2 textSize = ui.MeasureText(text, textStyle);
        Widget w = ui.Widget(Key::FromPointer(&value), {
            .Width = TrackWidth + Gap + textSize.X,
            .Height = TrackHeight > textSize.Y ? TrackHeight : textSize.Y,
        });
        if (w.Clicked)
            value = !value;

        // Transition remembers the animated values per widget; the control itself stores nothing.
        float on = ui.Transition(w, "on", value ? 1.0f : 0.0f, 180_ms);
        Color track = ui.Transition(w, "track", value ? Accent : Rgb(0x2A2F3C), 180_ms);
        float knobRadius = ui.Transition(w, "knob", w.Hovered ? 7.0f : 6.0f, 100_ms);

        Rect trackRect = { w.Rect.X, w.Rect.CenterY() - TrackHeight / 2, TrackWidth, TrackHeight };
        ui.DrawRect(trackRect, {
            .Fill = track,
            .Stroke = Rgba(0xFFFFFF1A),
            .StrokeThickness = 1,
            .CornerRadius = TrackHeight / 2,
            .Glow = { WithOpacity(Accent, 0.5f * on), 8 * on },
        });

        float inset = TrackHeight / 2;
        Vec2 knob = { Lerp(trackRect.X + inset, trackRect.Right() - inset, on), trackRect.CenterY() };
        ui.DrawCircle(knob, knobRadius, { .Fill = Rgb(0xF2F4F8), .Shadow = { { 0, 1 }, 3, Rgba(0x00000080) } });

        ui.DrawString({ trackRect.Right() + Gap, w.Rect.CenterY() - textSize.Y / 2 }, text, textStyle);
        return w.Clicked;
    }

    // --------------------------------------------------------------------------------------
    // Overlay content
    // --------------------------------------------------------------------------------------

    // Primitives drawn outside any panel: they sit below all panels and never take the mouse.
    void DrawScene(Ui& ui, const Settings& s, float time)
    {
        Vec2 view = ui.ViewportSize();

        // A route with glow, a marker moving along it and an arrow at its end.
        if (s.ShowRoute)
        {
            Vec2 p0 = { view.X * 0.34f, view.Y * 0.78f };
            Vec2 p1 = { view.X * 0.46f, view.Y * 0.30f };
            Vec2 p2 = { view.X * 0.60f, view.Y * 0.62f };
            Vec2 p3 = { view.X * 0.76f, view.Y * 0.62f }; // p2.Y == p3.Y: the route ends heading right

            ui.DrawBezier(p0, p1, p2, p3, {
                .Stroke = Brush::Linear(Rgb(0x5CE08A), Accent, 0),
                .Thickness = s.LineWidth,
                .Glow = { Rgba(0x3AA8FF90), s.GlowRadius },
            });

            float arrow = 6 + s.LineWidth * 2;
            ui.DrawTriangle({ p3.X + arrow * 1.4f, p3.Y }, { p3.X, p3.Y - arrow }, { p3.X, p3.Y + arrow },
                            { .Fill = Accent, .Glow = { Rgba(0x3AA8FF90), s.GlowRadius } });

            // Start marker: a circle with a soft drop shadow.
            ui.DrawCircle(p0, 12, {
                .Fill = Brush::Linear(Rgb(0x7BF0A6), Rgb(0x2DB86A)),
                .Stroke = Rgba(0xFFFFFF40),
                .StrokeThickness = 2,
                .Shadow = { { 0, 6 }, 10, Rgba(0x000000A0) },
            });

            float progress = time * 0.15f - float(int(time * 0.15f));
            ui.DrawCircle(Bezier(p0, p1, p2, p3, progress), 5, { .Fill = Colors::White, .Glow = { Rgba(0xFFFFFFA0), 10 } });
        }

        // An animated progress ring with its percentage in the middle.
        if (s.ShowRing)
        {
            Vec2 center = { view.X - 110, view.Y * 0.52f };
            float progress = 0.5f + 0.5f * Wave(time * 0.1f);
            ui.DrawArc(center, s.RingRadius, 0, 360, { .Stroke = Rgba(0xFFFFFF1A), .Thickness = 6 });
            ui.DrawArc(center, s.RingRadius, -90, 360 * progress, {
                .Stroke = Brush::Linear(AccentDeep, Rgb(0x5CE08A), 0),
                .Thickness = 6,
                .Glow = { Rgba(0x3AA8FF80), s.GlowRadius * 0.75f },
            });

            TextBuffer percent;
            percent.Add(int(progress * 100 + 0.5f)).Add("%");
            TextStyle style = { .FontSize = 16, .FontWeight = FontWeight::SemiBold };
            Vec2 size = ui.MeasureText(percent.View(), style);
            ui.DrawString(center - size / 2, percent.View(), style);
        }

        // A pulsing hexagon "objective" (convex polygon).
        {
            Vec2 center = { view.X * 0.56f, view.Y * 0.18f };
            float r = 22 + 3 * Wave(time * 0.5f);
            float h = r * 0.866f; // sin 60°
            Vec2 hexagon[6] = {
                { center.X + r, center.Y },
                { center.X + r / 2, center.Y + h },
                { center.X - r / 2, center.Y + h },
                { center.X - r, center.Y },
                { center.X - r / 2, center.Y - h },
                { center.X + r / 2, center.Y - h },
            };
            ui.DrawPolygon(hexagon, { .Fill = Brush::Linear(Rgb(0xFFB547), Rgb(0xFF6A3D)), .Glow = { Rgba(0xFF8A3D80), s.GlowRadius } });
        }

        // A fake telemetry graph (polyline).
        {
            constexpr int PointCount = 32;
            Vec2 points[PointCount];
            float left = view.X * 0.60f;
            float width = view.X * 0.32f;
            float baseline = view.Y * 0.88f;
            for (int i = 0; i < PointCount; ++i)
            {
                float x = float(i) / (PointCount - 1);
                float y = 0.6f * Wave(x * 1.5f + time * 0.2f) + 0.4f * Wave(x * 4.0f - time * 0.35f);
                points[i] = { left + x * width, baseline - 24 * y };
            }
            ui.DrawLine({ left, baseline }, { left + width, baseline }, { .Stroke = Rgba(0xFFFFFF1A) });
            ui.DrawPolyline(points, { .Stroke = Rgba(0x5CE08AE0), .Thickness = 2, .Glow = { Rgba(0x5CE08A60), 6 } });
        }

        // Plain text straight on the viewport.
        ui.DrawString({ 24, view.Y - 72 }, "FunkyUI", { .FontSize = 32, .FontWeight = FontWeight::Bold, .Foreground = Rgba(0xFFFFFFD0) });
        ui.DrawString({ 26, view.Y - 32 }, "immediate-mode overlay UI", { .FontSize = 13, .Foreground = Dim });
    }

    // Always visible; can be dragged around in menu mode.
    void StatsPanel(Ui& ui, Settings& s, float time, float fps)
    {
        if (auto panel = ui.Panel("stats", { .Position = &s.StatsPosition, .Movable = true, .Padding = { 12, 8 }, .Spacing = 4 }))
        {
            float load = 42 + 18 * Wave(time * 0.07f) + 6 * Wave(time * 0.9f);
            int cpu = int(load + 0.5f);
            Color cpuColor = ui.Transition(Key("stats.cpu"), cpu > 55 ? Rgb(0xFFB547) : Rgb(0x5CE08A), 300_ms);

            if (auto grid = ui.Grid({ .Columns = { Auto, Auto }, .ColumnSpacing = 14, .RowSpacing = 2 }))
            {
                TextBuffer cpuText;
                ui.Label("CPU", { .Foreground = Dim, .FontSize = 12, .FontWeight = FontWeight::SemiBold });
                ui.Label(cpuText.Add(cpu).Add("%").View(), { .Foreground = cpuColor, .FontWeight = FontWeight::SemiBold });

                TextBuffer fpsText;
                ui.Label("FPS", { .Foreground = Dim, .FontSize = 12, .FontWeight = FontWeight::SemiBold });
                ui.Label(fpsText.Add(int(fps + 0.5f)).View(), { .FontWeight = FontWeight::SemiBold });
            }
        }
    }

    // The "menu": a panel the client draws only while menu mode is on.
    void SettingsPanel(Ui& ui, Settings& s)
    {
        if (auto panel = ui.Panel("settings", { .Title = "Settings", .Anchor = Anchor::Left, .Movable = true, .MinWidth = 320 }))
        {
            if (auto grid = ui.Grid({ .Columns = { Auto, Star() } }))
            {
                ui.Label("Speed");
                ui.Slider(s.Speed, 0, 3, { .Step = 0.1f });
                ui.Label("Glow");
                ui.Slider(s.GlowRadius, 0, 24, { .Step = 1 });
                ui.Label("Line width");
                ui.Slider(s.LineWidth, 1, 8);
                ui.Label("Ring size");
                ui.Slider(s.RingRadius, 20, 60);
            }

            ui.CheckBox("Show route", s.ShowRoute);
            ui.CheckBox("Show progress ring", s.ShowRing);
            ui.CheckBox("Pause animation", s.Paused);
            ToggleSwitch(ui, "Stats panel", s.ShowStats);

            if (auto buttons = ui.Stack({ .Orientation = Orientation::Horizontal, .Spacing = 8, .HorizontalAlignment = HorizontalAlignment::Right }))
            {
                if (ui.Button("Resume", { .IsEnabled = s.Paused }))
                    s.Paused = false;
                if (ui.Button("Reset", { .HoverBackground = Rgb(0x4A2630), .PressedBackground = Rgb(0x6A2A36) }))
                    s = Settings {};
                if (ui.Button("Done", {
                        .Background = Brush::Linear(AccentDeep, Accent, 0),
                        .Foreground = Colors::White,
                        .BorderBrush = Rgba(0xFFFFFF33),
                        .Glow = { Rgba(0x3AA8FF99), 12 },
                    }))
                    ui.SetInteractive(false);
            }
        }
    }

    void HintPanel(Ui& ui)
    {
        PanelProps props = { .Anchor = Anchor::Bottom, .Padding = { 12, 6 }, .Background = Rgba(0x12141AB0), .Shadow = {} };
        if (auto panel = ui.Panel("hint", props))
        {
            if (auto row = ui.Stack({ .Orientation = Orientation::Horizontal, .Spacing = 8 }))
            {
                ui.Label("Insert — menu mode", { .Foreground = Dim, .FontSize = 13 });
                bool on = ui.IsInteractive();
                ui.Label(on ? "ON" : "OFF", { .Foreground = on ? Accent : Dim, .FontSize = 13, .FontWeight = FontWeight::Bold });
            }
        }
    }

    // --------------------------------------------------------------------------------------
    // The stand-in target window
    // --------------------------------------------------------------------------------------

    void FillSolid(HDC dc, int left, int top, int right, int bottom, COLORREF color)
    {
        RECT rect = { left, top, right, bottom };
        HBRUSH brush = CreateSolidBrush(color);
        FillRect(dc, &rect, brush);
        DeleteObject(brush);
    }

    // A dusky sky gradient over a dark floor with a perspective grid and a few silhouettes.
    void PaintTarget(HWND window)
    {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        RECT client;
        GetClientRect(window, &client);
        int w = client.right;
        int h = client.bottom;
        int horizon = h * 62 / 100;

        constexpr int Bands = 64;
        for (int i = 0; i < Bands; ++i)
        {
            int t = i * 255 / (Bands - 1);
            COLORREF color = RGB(10 + t * 38 / 255, 12 + t * 14 / 255, 26 + t * 34 / 255); // #0A0C1A → #301A3C
            FillSolid(dc, 0, horizon * i / Bands, w, horizon * (i + 1) / Bands, color);
        }

        const int buildings[][2] = { { 4, 22 }, { 11, 34 }, { 17, 18 }, { 64, 28 }, { 71, 40 }, { 79, 24 }, { 88, 31 } };
        for (const auto& b : buildings)
            FillSolid(dc, w * b[0] / 100, horizon - h * b[1] / 100 / 2, w * (b[0] + 6) / 100, horizon, RGB(14, 12, 24));

        FillSolid(dc, 0, horizon, w, h, RGB(12, 10, 20));

        HPEN pen = CreatePen(PS_SOLID, 1, RGB(70, 34, 96));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        for (int i = -12; i <= 12; ++i)
        {
            MoveToEx(dc, w / 2 + i * w / 40, horizon, nullptr);
            LineTo(dc, w / 2 + i * w / 6, h);
        }
        for (int i = 1, y = 4; horizon + y < h; ++i, y += 4 * i)
        {
            MoveToEx(dc, 0, horizon + y, nullptr);
            LineTo(dc, w, horizon + y);
        }
        SelectObject(dc, oldPen);
        DeleteObject(pen);

        EndPaint(window, &paint);
    }

    LRESULT CALLBACK TargetProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT:
                PaintTarget(window);
                return 0;
            case WM_KEYDOWN:
                if (wParam == VK_ESCAPE)
                    DestroyWindow(window);
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    HWND CreateTargetWindow(HINSTANCE instance, int showCommand)
    {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = TargetProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"FunkyUI.Demo.Target";
        if (!RegisterClassExW(&wc))
            return nullptr;

        // Borderless like a game in borderless mode: 1280x720 at the system DPI, centered on the primary monitor.
        UINT dpi = GetDpiForSystem();
        int width = MulDiv(1280, int(dpi), 96);
        int height = MulDiv(720, int(dpi), 96);
        HWND window = CreateWindowExW(0, wc.lpszClassName, L"FunkyUI Demo — target", WS_POPUP,
                                      (GetSystemMetrics(SM_CXSCREEN) - width) / 2, (GetSystemMetrics(SM_CYSCREEN) - height) / 2,
                                      width, height, nullptr, nullptr, instance, nullptr);
        if (window)
            ShowWindow(window, showCommand);
        return window;
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HWND target = CreateTargetWindow(instance, showCommand);
    if (!target)
        return 1;

    Ui* ui = Ui::Create({ .TargetWindow = target, .WindowClassName = "FunkyUI.Demo.Overlay", .DefaultFontFamily = "Segoe UI" });
    if (!ui)
        return 1;

    Settings settings;
    float time = 0;
    float fps = 60;
    bool insertWasDown = false;

    // BeginFrame pumps the messages of this thread (the target window's too) and returns false
    // once the target window is destroyed.
    while (ui->BeginFrame())
    {
        bool insertDown = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
        if (insertDown && !insertWasDown)
            ui->SetInteractive(!ui->IsInteractive());
        insertWasDown = insertDown;

        float dt = ui->DeltaTime();
        if (!settings.Paused)
            time += dt * settings.Speed;
        if (dt > 0)
            fps = Lerp(fps, 1 / dt, 0.05f);

        DrawScene(*ui, settings, time);
        if (settings.ShowStats)
            StatsPanel(*ui, settings, time, fps);
        HintPanel(*ui);
        if (ui->IsInteractive())
            SettingsPanel(*ui, settings);

        ui->EndFrame();
    }

    ui->Destroy();
    return 0;
}
