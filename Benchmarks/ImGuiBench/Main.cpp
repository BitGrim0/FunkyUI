// Dear ImGui side of the benchmark: translates the neutral scene model (BenchCommon.h) into Dear ImGui calls.
//
//   Display     own device + flip-model swap chain on the bench window, paced exactly like FunkyUI:
//               max frame latency 1, wait on the latency waitable object, Present(1, 0).
//   Throughput  the shared offscreen target, no platform backend.
//
// Effects Dear ImGui has no primitive for are emulated the usual way: shadows and shape glows as
// --layers layered shapes, line glows as a few wide faint strokes, gradients by shading the vertices.

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_internal.h" // ShadeVertsLinearColorGradientKeepAlpha
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "BenchCommon.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
    using Bench::Prim;
    using Bench::PrimKind;

    constexpr int GlowStrokes = 6;          // line / curve glow: from wide and faint to narrow and bright
    constexpr float ToggleRate = 12;        // knob animation: exponential approach, 1/s
    constexpr float DegreesToRadians = float(Bench::Pi / 180);

    // Same color with alpha multiplied by factor.
    ImU32 ScaleAlpha(ImU32 color, float factor)
    {
        uint32_t alpha = std::min(uint32_t(float(color >> 24) * factor + 0.5f), 255u);
        return (color & 0x00FFFFFFu) | (alpha << 24);
    }

    bool ForwardToImGui(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        return ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam) != 0;
    }

    // Model font sizes are em sizes, as in DirectWrite (FunkyUI); a Dear ImGui size is the hhea ascent - descent
    // (stbtt_ScaleForPixelHeight). Returns ImGui size per em from the font's 'head' and 'hhea' tables, 0 if they are missing.
    float ImGuiSizePerEm(const ImFontConfig& source)
    {
        const auto* data = static_cast<const uint8_t*>(source.FontData);
        size_t size = size_t(source.FontDataSize);
        auto u16 = [&](size_t at) { return at + 2 <= size ? uint32_t(data[at] << 8 | data[at + 1]) : 0u; };
        auto table = [&](const char* tag) -> size_t {
            for (size_t record = 12, end = 12 + 16 * size_t(u16(4)); record < end && record + 16 <= size; record += 16)
                if (std::memcmp(data + record, tag, 4) == 0)
                    return size_t(u16(record + 8)) << 16 | u16(record + 10);
            return 0;
        };
        size_t head = table("head");
        size_t hhea = table("hhea");
        uint32_t unitsPerEm = head ? u16(head + 18) : 0;
        if (!hhea || !unitsPerEm)
            return 0;
        return float(int16_t(u16(hhea + 4)) - int16_t(u16(hhea + 6))) / float(unitsPerEm);
    }

    class ImGuiBackend final : public Bench::Backend
    {
    public:
        explicit ImGuiBackend(int layers) : Layers(layers) {}

        const char* Name() const override { return "Dear ImGui"; }
        std::string Version() const override { return IMGUI_VERSION; }

        bool Open(Bench::Mode mode, const Bench::BenchWindow& window, const Bench::OffscreenTarget& target) override
        {
            IsDisplay = mode == Bench::Mode::Display;
            Window = window.Handle;
            Scale = window.DpiScale;
            if (IsDisplay)
            {
                if (!CreateSwapChain(window))
                    return false;
            }
            else
            {
                Device = target.Device;
                Context = target.Context;
                Target = target.View;
            }

            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.LogFilename = nullptr;
            ImGui::StyleColorsDark();
            ImGuiStyle& style = ImGui::GetStyle();
            style.ScaleAllSizes(Scale);
            // Font sizes are logical and FontScaleDpi scales widget text once; draw-list text gets pixel sizes.
            style.FontScaleDpi = Scale;
            if (!LoadFonts(io))
                return false;
            style.FontSizeBase = Bench::DefaultFontSize * SizePerEm[0];

            if (IsDisplay)
            {
                ImGui_ImplWin32_Init(Window);
                Win32Ready = true;
                Bench::WindowHook = ForwardToImGui;
            }
            else
                io.DisplaySize = ImVec2(float(target.Width), float(target.Height));
            Dx11Ready = ImGui_ImplDX11_Init(Device.Get(), Context.Get());
            LastFrame = Bench::Now();
            return Dx11Ready;
        }

        void Close() override
        {
            Bench::WindowHook = nullptr;
            if (ImGui::GetCurrentContext())
            {
                if (Dx11Ready)
                    ImGui_ImplDX11_Shutdown();
                if (Win32Ready)
                    ImGui_ImplWin32_Shutdown();
                ImGui::DestroyContext();
            }
            Dx11Ready = Win32Ready = false;
            if (FrameWaitable)
                CloseHandle(FrameWaitable);
            FrameWaitable = nullptr;
            Target.Reset();
            SwapChain.Reset();
            Context.Reset();
            Device.Reset();
        }

        void BeginScene(const Bench::Scene&) override { Knobs.clear(); }

        bool BeginFrame() override
        {
            ImGuiIO& io = ImGui::GetIO();
            if (IsDisplay)
            {
                WaitForSingleObjectEx(FrameWaitable, 1000, TRUE);
                Bench::PumpMessages();
                ImGui_ImplDX11_NewFrame();
                ImGui_ImplWin32_NewFrame();
                // Poll the cursor every frame like FunkyUI's host; the backend does it only while the window has the focus.
                POINT cursor;
                if (GetCursorPos(&cursor) && ScreenToClient(Window, &cursor))
                    io.AddMousePosEvent(float(cursor.x), float(cursor.y));
            }
            else
            {
                int64_t now = Bench::Now();
                io.DeltaTime = std::max(float(Bench::TicksToMs(now - LastFrame) / 1000), 1e-6f);
                LastFrame = now;
                io.AddMousePosEvent(Bench::PointerPosition.X * Scale, Bench::PointerPosition.Y * Scale);
                ImGui_ImplDX11_NewFrame();
            }
            ImGui::NewFrame();
            return true;
        }

        void Build(const Bench::FrameModel& model) override
        {
            ImDrawList* background = ImGui::GetBackgroundDrawList();
            for (const Prim& prim : model.Prims)
                DrawPrim(background, prim);
            if (Knobs.size() < model.Panels.size())
                Knobs.resize(model.Panels.size(), -1.0f);
            for (size_t i = 0; i < model.Panels.size(); ++i)
                DrawPanel(model.Panels[i], Knobs[i]);
        }

        void EndFrame() override
        {
            ImGui::Render();
            ID3D11RenderTargetView* target = Target.Get();
            Context->OMSetRenderTargets(1, &target, nullptr);
            if (IsDisplay)
                Context->ClearRenderTargetView(target, Bench::BackgroundColor);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            if (IsDisplay)
                SwapChain->Present(1, 0);
        }

        Bench::DrawCounts LastDrawCounts() const override
        {
            const ImDrawData* data = ImGui::GetDrawData();
            return data ? Bench::DrawCounts{ uint64_t(data->TotalVtxCount), uint64_t(data->TotalIdxCount) } : Bench::DrawCounts{};
        }

    private:
        bool CreateSwapChain(const Bench::BenchWindow& window)
        {
            const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
            if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                         UINT(std::size(levels)), D3D11_SDK_VERSION, &Device, nullptr, &Context)))
                return false;
            Bench::ComPtr<IDXGIDevice> dxgiDevice;
            Bench::ComPtr<IDXGIAdapter> adapter;
            Bench::ComPtr<IDXGIFactory2> factory;
            if (FAILED(Device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
                return false;

            DXGI_SWAP_CHAIN_DESC1 desc = {};
            desc.Width = window.Width;
            desc.Height = window.Height;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2;
            desc.Scaling = DXGI_SCALING_NONE;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
            Bench::ComPtr<IDXGISwapChain1> swapChain;
            if (FAILED(factory->CreateSwapChainForHwnd(Device.Get(), window.Handle, &desc, nullptr, nullptr, &swapChain)) ||
                FAILED(swapChain.As(&SwapChain)))
                return false;
            factory->MakeWindowAssociation(window.Handle, DXGI_MWA_NO_ALT_ENTER);
            SwapChain->SetMaximumFrameLatency(1);
            FrameWaitable = SwapChain->GetFrameLatencyWaitableObject();

            Bench::ComPtr<ID3D11Texture2D> backBuffer;
            return SUCCEEDED(SwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) &&
                   SUCCEEDED(Device->CreateRenderTargetView(backBuffer.Get(), nullptr, &Target));
        }

        bool LoadFonts(ImGuiIO& io)
        {
            wchar_t windows[MAX_PATH] = {};
            GetWindowsDirectoryW(windows, MAX_PATH);
            for (int i = 0; i < Bench::FontCount; ++i)
            {
                std::string path = Bench::ToUtf8(std::wstring(windows) + L"\\Fonts\\" + Bench::FontFiles[i]);
                // The first one is the default font. The size here is only a legacy default: FontSizeBase and AddText set the sizes.
                Fonts[i] = io.Fonts->AddFontFromFileTTF(path.c_str(), Bench::DefaultFontSize);
                SizePerEm[i] = Fonts[i] ? ImGuiSizePerEm(*Fonts[i]->Sources[0]) : 0;
                if (SizePerEm[i] <= 0)
                {
                    std::printf("Cannot load %s\n", path.c_str());
                    return false;
                }
            }
            return true;
        }

        ImVec2 ToPixels(Bench::Point p) const { return ImVec2(p.X * Scale, p.Y * Scale); }

        // Alpha of layer k (0 = innermost) of an emulated shadow or glow: fades out linearly, all layers add up to the full alpha.
        ImU32 LayerColor(ImU32 color, int k) const
        {
            return ScaleAlpha(color, 2.0f * float(Layers - k) / float(Layers * (Layers + 1)));
        }

        void DrawPrim(ImDrawList* dl, const Prim& p)
        {
            switch (p.Kind)
            {
                case PrimKind::Rect:
                case PrimKind::Circle:
                    DrawShape(dl, p);
                    break;
                case PrimKind::Arc:
                {
                    float start = p.StartAngle * DegreesToRadians;
                    dl->PathArcTo(ToPixels(p.Points[0]), p.Radius * Scale, start, start + p.SweepAngle * DegreesToRadians);
                    StrokePath(dl, p);
                    break;
                }
                case PrimKind::Line:
                    if (p.GlowRadius > 0)
                    {
                        dl->PathLineTo(ToPixels(p.Points[0]));
                        dl->PathLineTo(ToPixels(p.Points[1]));
                        StrokePath(dl, p);
                    }
                    else
                        dl->AddLine(ToPixels(p.Points[0]), ToPixels(p.Points[1]), p.Color, p.Thickness * Scale);
                    break;
                case PrimKind::Polyline:
                    for (int i = 0; i < p.PointCount; ++i)
                        dl->PathLineTo(ToPixels(p.Points[i]));
                    StrokePath(dl, p);
                    break;
                case PrimKind::Bezier:
                    if (p.GlowRadius > 0)
                    {
                        dl->PathLineTo(ToPixels(p.Points[0]));
                        dl->PathBezierCubicCurveTo(ToPixels(p.Points[1]), ToPixels(p.Points[2]), ToPixels(p.Points[3]));
                        StrokePath(dl, p);
                    }
                    else
                        dl->AddBezierCubic(ToPixels(p.Points[0]), ToPixels(p.Points[1]), ToPixels(p.Points[2]), ToPixels(p.Points[3]),
                                           p.Color, p.Thickness * Scale);
                    break;
                case PrimKind::Triangle:
                case PrimKind::Polygon:
                {
                    ImVec2 points[Bench::MaxPrimPoints];
                    int count = p.Kind == PrimKind::Triangle ? 3 : p.PointCount;
                    for (int i = 0; i < count; ++i)
                        points[i] = ToPixels(p.Points[i]);
                    if (p.GlowRadius > 0)
                        for (int k = 0; k < Layers; ++k)
                            dl->AddPolyline(points, count, LayerColor(p.GlowColor, k), p.GlowRadius * Scale * float(k + 1) / float(Layers),
                                            ImDrawFlags_Closed);
                    dl->AddConvexPolyFilled(points, count, p.Color);
                    break;
                }
                case PrimKind::Text:
                    dl->AddText(Fonts[p.Font], p.FontSize * SizePerEm[p.Font] * Scale, ToPixels(p.Points[0]), p.Color, p.Text,
                                p.Text + p.TextLength);
                    break;
            }
        }

        // Rects and circles: shadow layers, glow layers, the fill (+ gradient shading of its vertices) and the border.
        void DrawShape(ImDrawList* dl, const Prim& p)
        {
            bool circle = p.Kind == PrimKind::Circle;
            float radius = p.Radius * Scale;
            ImVec2 a = circle ? ToPixels(p.Points[0]) - ImVec2(radius, radius) : ToPixels(p.Points[0]);
            ImVec2 b = circle ? ToPixels(p.Points[0]) + ImVec2(radius, radius) : a + ImVec2(p.Size.X, p.Size.Y) * Scale;
            ImVec2 center = (a + b) * 0.5f;

            // The shape grown by `grow` pixels, filled or outlined.
            auto fill = [&](ImVec2 offset, float grow, ImU32 color) {
                if (circle)
                    dl->AddCircleFilled(center + offset, radius + grow, color);
                else
                    dl->AddRectFilled(a + offset - ImVec2(grow, grow), b + offset + ImVec2(grow, grow), color, std::max(radius + grow, 0.0f));
            };
            auto outline = [&](float grow, float thickness, ImU32 color) {
                if (circle)
                    dl->AddCircle(center, radius + grow, color, 0, thickness);
                else
                    dl->AddRect(a - ImVec2(grow, grow), b + ImVec2(grow, grow), color, radius + grow, thickness);
            };

            if (p.ShadowBlur > 0) // layers from shrunk by blur/2 to grown by blur/2
                for (int k = 0; k < Layers; ++k)
                    fill(ToPixels(p.ShadowOffset), p.ShadowBlur * Scale * (float(k + 1) / float(Layers) - 0.5f), LayerColor(p.ShadowColor, k));
            if (p.GlowRadius > 0) // outlines from the edge out to 1/Layers .. all of the glow radius
                for (int k = 0; k < Layers; ++k)
                {
                    float width = p.GlowRadius * Scale * float(k + 1) / float(Layers);
                    outline(width * 0.5f, width, LayerColor(p.GlowColor, k));
                }

            int firstVertex = dl->VtxBuffer.Size;
            fill({}, 0, p.Color);
            if (Bench::HasAlpha(p.GradientColor))
            {
                float angle = p.GradientAngle * DegreesToRadians;
                ImVec2 direction(std::cos(angle), std::sin(angle));
                float extent = std::abs((b.x - a.x) * 0.5f * direction.x) + std::abs((b.y - a.y) * 0.5f * direction.y);
                ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, firstVertex, dl->VtxBuffer.Size, center - direction * extent,
                                                              center + direction * extent, p.Color, p.GradientColor);
            }
            if (Bench::HasAlpha(p.BorderColor))
                outline(0, Scale, p.BorderColor);
        }

        // Strokes the current path; a glow goes underneath as wider, fainter strokes of the same path.
        void StrokePath(ImDrawList* dl, const Prim& p)
        {
            if (p.GlowRadius > 0)
                for (int k = 0; k < GlowStrokes; ++k)
                {
                    float spread = 1 - float(k) / GlowStrokes; // 1 .. 1/6
                    dl->AddPolyline(dl->_Path.Data, dl->_Path.Size, ScaleAlpha(p.GlowColor, float(k + 1) / 21), // 1 + 2 + .. + 6 = 21
                                    (p.Thickness + 2 * p.GlowRadius * spread) * Scale);
                }
            dl->PathStroke(p.Color, p.Thickness * Scale);
        }

        void DrawPanel(const Bench::PanelModel& panel, float& knob)
        {
            ImGui::SetNextWindowPos(ToPixels(panel.Position));
            ImGui::SetNextWindowSize(ImVec2(Bench::PanelWidth * Scale, 0));
            if (ImGui::Begin(panel.Title, nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize))
            {
                if (ImGui::BeginTable("grid", 2))
                {
                    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed);
                    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
                    for (int row = 0; row < Bench::RowCount; ++row)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(Bench::RowLabels[row]);
                        ImGui::TableNextColumn();
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        float value = panel.Values[row];
                        ImGui::PushID(row);
                        ImGui::SliderFloat("##", &value, 0, 1, ""); // no value text, like FunkyUI's slider
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }

                for (int i = 0; i < Bench::CheckCount; ++i)
                {
                    bool value = panel.Checks[i];
                    ImGui::Checkbox(Bench::CheckLabels[i], &value);
                }

                for (int i = 0; i < Bench::ButtonCount; ++i)
                {
                    if (i > 0)
                        ImGui::SameLine();
                    ImGui::Button(Bench::ButtonLabels[i]);
                }

                ToggleSwitch(panel.Toggle, knob);
            }
            ImGui::End();
        }

        // Custom control: InvisibleButton + draw list, the knob animated on the client side.
        void ToggleSwitch(bool on, float& knob)
        {
            float target = on ? 1.0f : 0.0f;
            knob = knob < 0 ? target : knob + (target - knob) * std::min(1.0f, ImGui::GetIO().DeltaTime * ToggleRate);

            ImVec2 text = ImGui::CalcTextSize(Bench::ToggleLabel);
            float trackWidth = Bench::TrackWidth * Scale;
            float trackHeight = Bench::TrackHeight * Scale;
            float gap = Bench::ToggleGap * Scale;
            ImVec2 origin = ImGui::GetCursorScreenPos();
            ImVec2 size(trackWidth + gap + text.x, std::max(trackHeight, text.y));
            ImGui::InvisibleButton(Bench::ToggleLabel, size);
            bool hovered = ImGui::IsItemHovered();

            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 a(origin.x, origin.y + (size.y - trackHeight) * 0.5f);
            ImVec2 b = a + ImVec2(trackWidth, trackHeight);
            ImVec4 off = ImGui::ColorConvertU32ToFloat4(Bench::ToggleOffColor);
            ImVec4 on4 = ImGui::ColorConvertU32ToFloat4(Bench::ToggleOnColor);
            dl->AddRectFilled(a, b, ImGui::ColorConvertFloat4ToU32(ImLerp(off, on4, knob)), trackHeight * 0.5f);
            dl->AddRect(a, b, Bench::ToggleBorderColor, trackHeight * 0.5f, Scale);
            float inset = trackHeight * 0.5f;
            dl->AddCircleFilled(ImVec2(ImLerp(a.x + inset, b.x - inset, knob), (a.y + b.y) * 0.5f), (hovered ? 7.0f : 6.0f) * Scale,
                                Bench::ToggleKnobColor);
            dl->AddText(ImVec2(b.x + gap, origin.y + (size.y - text.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), Bench::ToggleLabel);
        }

        int Layers;
        bool IsDisplay = true;
        bool Win32Ready = false;
        bool Dx11Ready = false;
        HWND Window = nullptr;
        float Scale = 1;
        int64_t LastFrame = 0;
        Bench::ComPtr<ID3D11Device> Device;
        Bench::ComPtr<ID3D11DeviceContext> Context;
        Bench::ComPtr<IDXGISwapChain2> SwapChain;
        Bench::ComPtr<ID3D11RenderTargetView> Target;
        HANDLE FrameWaitable = nullptr;
        ImFont* Fonts[Bench::FontCount] = {};
        float SizePerEm[Bench::FontCount] = {};     // Dear ImGui font size per em size
        std::vector<float> Knobs;                   // toggle knob per panel, -1 = not shown yet
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
    ImGuiBackend backend(options.Layers);
    return Bench::Run(backend, options);
}
