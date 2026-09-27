// FunkyUI side of the benchmark: translates the neutral scene model (BenchCommon.h) into FunkyUI calls.
//
//   Display     the bench window is the overlay's target: Ui::Create, BeginFrame waits and pumps.
//   Throughput  embedded into the shared offscreen target: Ui::CreateEmbedded + SetRenderTarget.
//
// All effects are native: ShapeStyle Shadow / Glow, Brush::Linear gradients, LineStyle Glow.

#include <FunkyUI.h>

#include "BenchCommon.h"

namespace
{
    using namespace Funky::Literals;
    using Bench::PrimKind;

    Funky::Color ToColor(uint32_t rgba)
    {
        return { uint8_t(rgba), uint8_t(rgba >> 8), uint8_t(rgba >> 16), uint8_t(rgba >> 24) };
    }

    Funky::Vec2 ToVec2(Bench::Point p) { return { p.X, p.Y }; }

    Funky::ShapeStyle ShapeStyleOf(const Bench::Prim& p, float cornerRadius)
    {
        bool gradient = Bench::HasAlpha(p.GradientColor);
        bool border = Bench::HasAlpha(p.BorderColor);
        return {
            .Fill = gradient ? Funky::Brush::Linear(ToColor(p.Color), ToColor(p.GradientColor), p.GradientAngle) : Funky::Brush(ToColor(p.Color)),
            .Stroke = border ? Funky::Brush(ToColor(p.BorderColor)) : Funky::Brush(),
            .StrokeThickness = border ? 1.0f : 0.0f,
            .CornerRadius = cornerRadius,
            .Glow = { ToColor(p.GlowColor), p.GlowRadius },
            .Shadow = { ToVec2(p.ShadowOffset), p.ShadowBlur, ToColor(p.ShadowColor) },
        };
    }

    Funky::LineStyle LineStyleOf(const Bench::Prim& p)
    {
        return { .Stroke = ToColor(p.Color), .Thickness = p.Thickness, .Glow = { ToColor(p.GlowColor), p.GlowRadius } };
    }

    Funky::FillStyle FillStyleOf(const Bench::Prim& p)
    {
        return { .Fill = ToColor(p.Color), .Glow = { ToColor(p.GlowColor), p.GlowRadius } };
    }

    class FunkyBackend final : public Bench::Backend
    {
    public:
        const char* Name() const override { return "FunkyUI"; }
        std::string Version() const override { return "(this repository)"; }

        bool Open(Bench::Mode mode, const Bench::BenchWindow& window, const Bench::OffscreenTarget& target) override
        {
            IsDisplay = mode == Bench::Mode::Display;
            Target = &target;
            Scale = window.DpiScale;
            Ui = IsDisplay ? Funky::Ui::Create({
                                 .TargetWindow = window.Handle,
                                 .WindowClassName = "FunkyBench.Overlay",
                                 .DefaultFontFamily = Bench::FontFamilies[0],
                             })
                           : Funky::Ui::CreateEmbedded({ .Device = target.Device.Get(), .DefaultFontFamily = Bench::FontFamilies[0] });
            if (!Ui)
                return false;
            // Font 0 is the default family (as the first font is Dear ImGui's default); the others are loaded.
            for (int i = 1; i < Bench::FontCount; ++i)
                Fonts[i] = Ui->LoadFont(Bench::FontFamilies[i]);
            return true;
        }

        void Close() override
        {
            if (Ui)
                Ui->Destroy();
            Ui = nullptr;
        }

        void BeginScene(const Bench::Scene& scene) override { Ui->SetInteractive(scene.Interactive); }

        bool BeginFrame() override
        {
            if (!IsDisplay)
            {
                Ui->SetRenderTarget(Target->View.Get(), Target->Width, Target->Height, Scale);
                Ui->SetPointer({ Bench::PointerPosition.X * Scale, Bench::PointerPosition.Y * Scale }, false);
            }
            return Ui->BeginFrame();
        }

        void Build(const Bench::FrameModel& model) override
        {
            for (const Bench::Prim& prim : model.Prims)
                DrawPrim(prim);
            if (Positions.size() < model.Panels.size())
                Positions.resize(model.Panels.size());
            for (size_t i = 0; i < model.Panels.size(); ++i)
                DrawPanel(model.Panels[i], i);
        }

        void EndFrame() override { Ui->EndFrame(); }

    private:
        void DrawPrim(const Bench::Prim& p)
        {
            switch (p.Kind)
            {
                case PrimKind::Rect:
                    Ui->DrawRect({ p.Points[0].X, p.Points[0].Y, p.Size.X, p.Size.Y }, ShapeStyleOf(p, p.Radius));
                    break;
                case PrimKind::Circle:
                    Ui->DrawCircle(ToVec2(p.Points[0]), p.Radius, ShapeStyleOf(p, 0));
                    break;
                case PrimKind::Arc:
                    Ui->DrawArc(ToVec2(p.Points[0]), p.Radius, p.StartAngle, p.SweepAngle, LineStyleOf(p));
                    break;
                case PrimKind::Line:
                    Ui->DrawLine(ToVec2(p.Points[0]), ToVec2(p.Points[1]), LineStyleOf(p));
                    break;
                case PrimKind::Polyline:
                    Ui->DrawPolyline(Points(p), LineStyleOf(p));
                    break;
                case PrimKind::Bezier:
                    Ui->DrawBezier(ToVec2(p.Points[0]), ToVec2(p.Points[1]), ToVec2(p.Points[2]), ToVec2(p.Points[3]), LineStyleOf(p));
                    break;
                case PrimKind::Triangle:
                    Ui->DrawTriangle(ToVec2(p.Points[0]), ToVec2(p.Points[1]), ToVec2(p.Points[2]), FillStyleOf(p));
                    break;
                case PrimKind::Polygon:
                    Ui->DrawPolygon(Points(p), FillStyleOf(p));
                    break;
                case PrimKind::Text:
                    Ui->DrawString(ToVec2(p.Points[0]), { p.Text, p.TextLength },
                                   { .Font = Fonts[p.Font], .FontSize = p.FontSize, .Foreground = ToColor(p.Color) });
                    break;
            }
        }

        std::span<const Funky::Vec2> Points(const Bench::Prim& p)
        {
            for (int i = 0; i < p.PointCount; ++i)
                Scratch[i] = ToVec2(p.Points[i]);
            return { Scratch, p.PointCount };
        }

        void DrawPanel(const Bench::PanelModel& panel, size_t index)
        {
            Positions[index] = ToVec2(panel.Position);
            if (auto scope = Ui->Panel(Funky::Key(1000 + index), { .Title = panel.Title, .Position = &Positions[index], .Width = Bench::PanelWidth }))
            {
                if (auto grid = Ui->Grid({ .Columns = { Funky::Auto, Funky::Star() } }))
                    for (int row = 0; row < Bench::RowCount; ++row)
                    {
                        Ui->Label(Bench::RowLabels[row]);
                        float value = panel.Values[row];
                        Ui->Slider(value, 0, 1, { .Key = Funky::Key(1 + row) });
                    }

                for (int i = 0; i < Bench::CheckCount; ++i)
                {
                    bool value = panel.Checks[i];
                    Ui->CheckBox(Bench::CheckLabels[i], value, { .Key = Funky::Key(100 + i) });
                }

                if (auto buttons = Ui->Stack({ .Orientation = Funky::Orientation::Horizontal, .Spacing = 8 }))
                    for (const char* label : Bench::ButtonLabels)
                        Ui->Button(label);

                ToggleSwitch(panel.Toggle);
            }
        }

        // Custom control: Widget + Transition + Draw*, the same shapes as the Dear ImGui version.
        void ToggleSwitch(bool on)
        {
            Funky::Vec2 text = Ui->MeasureText(Bench::ToggleLabel);
            Funky::Widget w = Ui->Widget("toggle", {
                .Width = Bench::TrackWidth + Bench::ToggleGap + text.X,
                .Height = std::max(Bench::TrackHeight, text.Y),
            });
            float knob = Ui->Transition(w, "knob", on ? 1.0f : 0.0f, 150_ms);

            Funky::Rect track = { w.Rect.X, w.Rect.CenterY() - Bench::TrackHeight / 2, Bench::TrackWidth, Bench::TrackHeight };
            Ui->DrawRect(track, {
                .Fill = Funky::Lerp(ToColor(Bench::ToggleOffColor), ToColor(Bench::ToggleOnColor), knob),
                .Stroke = ToColor(Bench::ToggleBorderColor),
                .StrokeThickness = 1,
                .CornerRadius = Bench::TrackHeight / 2,
            });
            float inset = Bench::TrackHeight / 2;
            Funky::Vec2 center = { Funky::Lerp(track.X + inset, track.Right() - inset, knob), track.CenterY() };
            Ui->DrawCircle(center, w.Hovered ? 7.0f : 6.0f, { .Fill = ToColor(Bench::ToggleKnobColor) });
            Ui->DrawString({ track.Right() + Bench::ToggleGap, w.Rect.CenterY() - text.Y / 2 }, Bench::ToggleLabel);
        }

        Funky::Ui* Ui = nullptr;
        bool IsDisplay = true;
        const Bench::OffscreenTarget* Target = nullptr;
        float Scale = 1;
        Funky::Font Fonts[Bench::FontCount] = {};
        Funky::Vec2 Scratch[Bench::MaxPrimPoints] = {};
        std::vector<Funky::Vec2> Positions;         // client-owned panel positions (Panel reads them, and writes them on drag)
    };
}

int main(int argc, char** argv)
{
    Bench::Options options;
    if (!Bench::ParseOptions(argc, argv, options))
    {
        Bench::PrintUsage();
        return 1;
    }
    FunkyBackend backend;
    return Bench::Run(backend, options);
}
