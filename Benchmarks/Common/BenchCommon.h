// BenchCommon.h — everything the FunkyUI and Dear ImGui benchmarks share, so that both measure the
// same content at the same points: options, system info, the bench window, the offscreen target,
// counters, the neutral scene model, the frame loop, statistics and the report.
//
// A scene is a neutral model (background primitives + menu panels) generated from the frame index
// only, before the measured part of the frame. A benchmark implements Backend and only translates
// the model into its library's calls. Model coordinates are logical (1280x720 DIPs).
//
// One frame (Runner::RunFrame):
//     generate the model                                  not measured
//     [throughput] wait for a free timestamp slot         = GPU wait ms (only when the ring is full)
//     Backend::BeginFrame   vsync wait, messages, begin  -+
//     Backend::Build        UI code          = build ms   |  CPU cycles of this thread
//     [throughput] disjoint + begin timestamp + clear     |  (blocking waits cost no cycles)
//     Backend::EndFrame     render (+present) = submit ms |  throughput: CPU frame ms (wall)
//     [throughput] end timestamp + flush                 -+
// Throughput never waits for the frame it just submitted: up to GpuFramesInFlight frames are in flight and
// their GPU times (timestamp queries) are read a few frames later. Throughput FPS = 1000 / max(CPU, GPU).
// Display measures frame to frame instead (generation included, normally hidden in the vsync wait).

#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <intrin.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <timeapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <string_view>
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "winmm.lib")

// printf-style format checking where the compiler supports it.
#if defined(__clang__)
    #define BENCH_PRINTF_FORMAT(formatIndex, firstArgument) __attribute__((format(printf, formatIndex, firstArgument)))
#else
    #define BENCH_PRINTF_FORMAT(formatIndex, firstArgument)
#endif

namespace Bench
{
    using Microsoft::WRL::ComPtr;

    // ------------------------------------------------------------------------------------
    // Options
    // ------------------------------------------------------------------------------------

    constexpr int DefaultDisplayFrames = 600;
    constexpr int DefaultThroughputFrames = 1000;
    constexpr int DefaultRampFrames = 300;
    constexpr double MinWarmupMs = 500;     // warmup lasts at least this long: caches, animations and clocks settle

    struct Options
    {
        float Scale = 1;                    // multiplies all object counts
        int Frames = 0;                     // measured frames per scene, 0 = the mode's default
        int Warmup = 60;                    // warmup frames per scene (and at least MinWarmupMs)
        int Layers = 8;                     // Dear ImGui: layers of the emulated shadows and glows
        std::string Only;                   // run a single scene
        bool DisplayOnly = false;
        bool ThroughputOnly = false;
        bool Quick = false;                 // halves the measured frames

        int MeasuredFrames(int defaultFrames) const
        {
            int frames = Frames > 0 ? Frames : defaultFrames;
            return Quick ? std::max(frames / 2, 1) : frames;
        }

        int RampFrames() const { return Quick ? DefaultRampFrames / 2 : DefaultRampFrames; }
    };

    inline void PrintUsage()
    {
        std::printf(
            "Options:\n"
            "  --scale F          multiply all object counts by F (default 1)\n"
            "  --frames N         measured frames per scene (default: display %d, throughput %d)\n"
            "  --warmup N         warmup frames per scene (default 60, and at least %.1f s)\n"
            "  --layers N         layers of the emulated shadows/glows in Dear ImGui (default 8)\n"
            "  --only SCENE       run one scene: Idle Menu Background Mixed TextStatic TextDynamic\n"
            "                     TextFonts Fill Effects Ramp\n"
            "  --display-only     only the display mode (window + vsync)\n"
            "  --throughput-only  only the throughput mode (offscreen, no vsync) and the ramp\n"
            "  --quick            halve the measured frames\n"
            "Esc aborts the run; the results so far are still reported.\n",
            DefaultDisplayFrames, DefaultThroughputFrames, MinWarmupMs / 1000);
    }

    inline bool ParseOptions(int argc, char** argv, Options& options)
    {
        auto number = [&](int& i, double minimum, double& value) {
            if (i + 1 >= argc)
                return false;
            const char* text = argv[++i];
            char* end = nullptr;
            value = std::strtod(text, &end);
            return end != text && *end == 0 && value >= minimum;
        };

        for (int i = 1; i < argc; ++i)
        {
            std::string_view arg = argv[i];
            double value = 0;
            if (arg == "--scale" && number(i, 0.01, value))
                options.Scale = float(value);
            else if (arg == "--frames" && number(i, 1, value))
                options.Frames = int(value);
            else if (arg == "--warmup" && number(i, 0, value))
                options.Warmup = int(value);
            else if (arg == "--layers" && number(i, 1, value))
                options.Layers = int(value);
            else if (arg == "--only" && i + 1 < argc)
                options.Only = argv[++i];
            else if (arg == "--display-only")
                options.DisplayOnly = true;
            else if (arg == "--throughput-only")
                options.ThroughputOnly = true;
            else if (arg == "--quick")
                options.Quick = true;
            else
                return false;
        }
        return !(options.DisplayOnly && options.ThroughputOnly);
    }

    // ------------------------------------------------------------------------------------
    // Time, CPU and statistics
    // ------------------------------------------------------------------------------------

    inline int64_t Now()
    {
        LARGE_INTEGER ticks;
        QueryPerformanceCounter(&ticks);
        return ticks.QuadPart;
    }

    inline double TicksToMs(int64_t ticks)
    {
        static const double msPerTick = [] {
            LARGE_INTEGER frequency;
            QueryPerformanceFrequency(&frequency);
            return 1000.0 / double(frequency.QuadPart);
        }();
        return double(ticks) * msPerTick;
    }

    // CPU cycles this thread has run (user + kernel). Blocked waits do not count.
    inline uint64_t ThreadCycles()
    {
        ULONG64 cycles = 0;
        QueryThreadCycleTime(GetCurrentThread(), &cycles);
        return cycles;
    }

    // Thread cycles tick at a fixed reference clock: a busy spin against QPC gives cycles per ms.
    inline double MeasureCyclesPerMs()
    {
        double best = 0;
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            int64_t start = Now();
            uint64_t cycles = ThreadCycles();
            while (TicksToMs(Now() - start) < 100)
            {
            }
            best = std::max(best, double(ThreadCycles() - cycles) / TicksToMs(Now() - start));
        }
        return best;
    }

    inline uint64_t FileTimeTicks(FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; }

    // User + kernel time of all threads of the process, in 100 ns units (updated at the ~15.6 ms clock tick).
    inline uint64_t ProcessCpuTime()
    {
        FILETIME creation, exit, kernel, user;
        if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
            return 0;
        return FileTimeTicks(kernel) + FileTimeTicks(user);
    }

    // The same for this thread only.
    inline uint64_t ThreadCpuTime()
    {
        FILETIME creation, exit, kernel, user;
        if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user))
            return 0;
        return FileTimeTicks(kernel) + FileTimeTicks(user);
    }

    struct Distribution
    {
        double Avg = 0;
        double P50 = 0;
        double P95 = 0;
        double P99 = 0;
        double Max = 0;
    };

    inline Distribution Summarize(std::vector<double> values)
    {
        Distribution d;
        if (values.empty())
            return d;
        std::sort(values.begin(), values.end());
        double sum = 0;
        for (double v : values)
            sum += v;
        auto percentile = [&](double p) { // nearest rank
            size_t rank = size_t(std::ceil(p * double(values.size())));
            return values[std::clamp<size_t>(rank, 1, values.size()) - 1];
        };
        d.Avg = sum / double(values.size());
        d.P50 = percentile(0.50);
        d.P95 = percentile(0.95);
        d.P99 = percentile(0.99);
        d.Max = values.back();
        return d;
    }

    // ------------------------------------------------------------------------------------
    // Text helpers
    // ------------------------------------------------------------------------------------

    BENCH_PRINTF_FORMAT(1, 0) inline std::string FormatV(const char* format, va_list args)
    {
        va_list measure;
        va_copy(measure, args);
        int length = std::vsnprintf(nullptr, 0, format, measure);
        va_end(measure);
        if (length <= 0)
            return {};
        std::string text(size_t(length), '\0');
        std::vsnprintf(text.data(), text.size() + 1, format, args);
        return text;
    }

    BENCH_PRINTF_FORMAT(1, 2) inline std::string Format(const char* format, ...)
    {
        va_list args;
        va_start(args, format);
        std::string text = FormatV(format, args);
        va_end(args);
        return text;
    }

    inline std::string ToUtf8(std::wstring_view text)
    {
        int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
        std::string result(size_t(std::max(size, 0)), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), result.data(), size, nullptr, nullptr);
        return result;
    }

    inline std::string Trim(std::string text)
    {
        size_t first = text.find_first_not_of(' ');
        size_t last = text.find_last_not_of(' ');
        return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
    }

    inline bool EqualsIgnoreCase(std::string_view a, std::string_view b)
    {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
               });
    }

    // ------------------------------------------------------------------------------------
    // The bench window: a centered borderless window, 1280x720 DIPs at its monitor's DPI
    // ------------------------------------------------------------------------------------

    constexpr float ViewWidth = 1280;       // logical scene size, DIPs
    constexpr float ViewHeight = 720;
    constexpr float BackgroundColor[4] = { 0x14 / 255.0f, 0x16 / 255.0f, 0x1C / 255.0f, 1 };

    struct Point
    {
        float X = 0;
        float Y = 0;
    };

    constexpr Point PointerPosition = { 640, 360 }; // where the mouse pointer stays in every scene (logical)

    struct BenchWindow
    {
        HWND Handle = nullptr;
        float DpiScale = 1;                 // physical pixels per DIP
        uint32_t Width = 0;                 // client area, physical pixels
        uint32_t Height = 0;
    };

    // Extra message handler (Dear ImGui's Win32 backend); returns true when it handled the message.
    using MessageHook = bool (*)(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    inline MessageHook WindowHook = nullptr;
    inline bool EscapePressed = false;

    inline LRESULT CALLBACK BenchWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (WindowHook && WindowHook(window, message, wParam, lParam))
            return 1;
        switch (message)
        {
            case WM_KEYDOWN:
                if (wParam == VK_ESCAPE)
                    EscapePressed = true;
                return 0;
            case WM_CLOSE:
                EscapePressed = true;
                return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    // The window may not have the keyboard focus (a console app cannot always take it), so the key is polled too.
    inline bool AbortRequested()
    {
        return EscapePressed || (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    }

    inline bool CreateBenchWindow(BenchWindow& w, const wchar_t* title)
    {
        HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = BenchWindowProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(RGB(0x14, 0x16, 0x1C));
        wc.lpszClassName = L"UiBench.Window";
        if (!RegisterClassExW(&wc))
            return false;

        // Created hidden on the primary monitor, then sized at that monitor's DPI and centered on it.
        w.Handle = CreateWindowExW(0, wc.lpszClassName, title, WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, instance, nullptr);
        if (!w.Handle)
            return false;
        UINT dpi = GetDpiForWindow(w.Handle);
        w.DpiScale = float(dpi) / 96;
        w.Width = uint32_t(MulDiv(int(ViewWidth), int(dpi), 96));
        w.Height = uint32_t(MulDiv(int(ViewHeight), int(dpi), 96));

        MONITORINFO monitor = {};
        monitor.cbSize = sizeof(monitor);
        GetMonitorInfoW(MonitorFromWindow(w.Handle, MONITOR_DEFAULTTOPRIMARY), &monitor);
        const RECT& area = monitor.rcMonitor;
        SetWindowPos(w.Handle, nullptr, area.left + (area.right - area.left - int(w.Width)) / 2,
                     area.top + (area.bottom - area.top - int(w.Height)) / 2, int(w.Width), int(w.Height),
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return true;
    }

    inline void PumpMessages()
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    inline void ShowBenchWindow(const BenchWindow& w, bool show)
    {
        ShowWindow(w.Handle, show ? SW_SHOW : SW_HIDE);
        if (show)
        {
            SetForegroundWindow(w.Handle);
            UpdateWindow(w.Handle);
        }
        PumpMessages();
    }

    // Both libraries read the real cursor in display mode: put it on the same point for both.
    inline void PlaceCursor(const BenchWindow& w)
    {
        POINT p = { LONG(PointerPosition.X * w.DpiScale), LONG(PointerPosition.Y * w.DpiScale) };
        ClientToScreen(w.Handle, &p);
        SetCursorPos(p.x, p.y);
    }

    // ------------------------------------------------------------------------------------
    // Offscreen target (throughput mode): own device, no swap chain, no Present
    // ------------------------------------------------------------------------------------

    // Frames the CPU may run ahead of the GPU. Without Present a query's completion is only noticed at the next
    // Windows timer tick (~15.6 ms, 1 ms under the throughput mode's timeBeginPeriod(1)): the ring must hold a
    // whole tick of frames, or the GPU gets bursts, idles between them and drops its clocks. 64 cover a 1 ms tick
    // at any frame cost, and a default tick from ~0.25 ms per frame. A slot is 3 small queries.
    constexpr int GpuFramesInFlight = 64;   // the legend and the README name this number

    // The target plus a ring of timestamp queries: each frame's GPU time is read a few frames later.
    class OffscreenTarget
    {
    public:
        ComPtr<ID3D11Device> Device;
        ComPtr<ID3D11DeviceContext> Context;
        ComPtr<ID3D11Texture2D> Texture;
        ComPtr<ID3D11RenderTargetView> View;
        uint32_t Width = 0;                 // physical pixels
        uint32_t Height = 0;

        bool Create(uint32_t width, uint32_t height)
        {
            const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
            if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                         UINT(std::size(levels)), D3D11_SDK_VERSION, &Device, nullptr, &Context)))
                return false;

            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // same format as the swap chains of the display mode
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(Device->CreateTexture2D(&desc, nullptr, &Texture)) ||
                FAILED(Device->CreateRenderTargetView(Texture.Get(), nullptr, &View)))
                return false;

            D3D11_QUERY_DESC disjoint = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
            D3D11_QUERY_DESC timestamp = { D3D11_QUERY_TIMESTAMP, 0 };
            for (GpuFrame& f : Ring)
                if (FAILED(Device->CreateQuery(&disjoint, &f.Disjoint)) || FAILED(Device->CreateQuery(&timestamp, &f.Begin)) ||
                    FAILED(Device->CreateQuery(&timestamp, &f.End)))
                    return false;
            Width = width;
            Height = height;
            return true;
        }

        // Before a frame: collects the finished frames without blocking; when GpuFramesInFlight frames are
        // in flight, spins until the oldest one is done (never sleeps: a sleep costs a timer tick).
        void WaitForSlot()
        {
            while (Pending > 0 && CollectOldest(false))
            {
            }
            if (Pending == GpuFramesInFlight)
                CollectOldest(true);
        }

        // Right before the library renders: starts the frame's GPU interval and clears the target.
        // The GPU time lands in *gpuMs once the GPU finished the frame (stays < 0 when the interval is disjoint).
        void BeginFrame(double* gpuMs)
        {
            GpuFrame& f = Ring[Next];
            f.Result = gpuMs;
            Context->Begin(f.Disjoint.Get());
            Context->End(f.Begin.Get());
            Context->ClearRenderTargetView(View.Get(), BackgroundColor);
        }

        // Ends the interval and hands the recorded commands to the GPU.
        void EndFrame()
        {
            GpuFrame& f = Ring[Next];
            Context->End(f.End.Get());
            Context->End(f.Disjoint.Get());
            Context->Flush();
            Next = (Next + 1) % GpuFramesInFlight;
            ++Pending;
        }

        // Waits for every frame in flight: their results must land before the samples are read (or freed).
        void Drain()
        {
            while (Pending > 0)
                CollectOldest(true);
        }

    private:
        struct GpuFrame
        {
            ComPtr<ID3D11Query> Disjoint;   // the timestamps are only comparable if the GPU clock did not change
            ComPtr<ID3D11Query> Begin;
            ComPtr<ID3D11Query> End;
            double* Result = nullptr;
        };

        // S_OK = data read, S_FALSE = not ready (only without wait), anything else = failed (device lost).
        HRESULT Read(ID3D11Query* query, void* data, UINT size, bool wait)
        {
            HRESULT hr;
            while ((hr = Context->GetData(query, data, size, D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE && wait)
                YieldProcessor();
            return hr;
        }

        // Reads the oldest frame in flight. False = the GPU has not finished it yet (only without wait).
        bool CollectOldest(bool wait)
        {
            GpuFrame& f = Ring[(Next + GpuFramesInFlight - Pending) % GpuFramesInFlight];
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint = {};
            HRESULT hr = Read(f.Disjoint.Get(), &disjoint, sizeof(disjoint), wait);
            if (hr == S_FALSE)
                return false;
            // The disjoint query ends after the timestamps: once it is done, they are too.
            uint64_t begin = 0;
            uint64_t end = 0;
            bool valid = hr == S_OK && !disjoint.Disjoint && disjoint.Frequency > 0 && Read(f.Begin.Get(), &begin, sizeof(begin), true) == S_OK &&
                         Read(f.End.Get(), &end, sizeof(end), true) == S_OK && end >= begin;
            if (valid && f.Result)
                *f.Result = double(end - begin) * 1000.0 / double(disjoint.Frequency);
            f.Result = nullptr;
            --Pending;
            return true;
        }

        GpuFrame Ring[GpuFramesInFlight];
        int Next = 0;                       // slot of the next frame
        int Pending = 0;                    // frames in flight: the slots before Next
    };

    // ------------------------------------------------------------------------------------
    // Counters: GPU utilization (PDH) and memory
    // ------------------------------------------------------------------------------------

    // 3D-engine utilization of this process, summed over its "GPU Engine" instances and averaged
    // between Start and Stop. English counter names: the localized ones differ per Windows language.
    class GpuUsage
    {
    public:
        GpuUsage() = default;
        GpuUsage(const GpuUsage&) = delete;
        GpuUsage& operator=(const GpuUsage&) = delete;
        ~GpuUsage()
        {
            if (Query)
                PdhCloseQuery(Query);
        }

        bool Open()
        {
            if (PdhOpenQueryW(nullptr, 0, &Query) != ERROR_SUCCESS)
                return false;
            wchar_t path[128];
            std::swprintf(path, std::size(path), L"\\GPU Engine(pid_%lu_*engtype_3D)\\Utilization Percentage", GetCurrentProcessId());
            if (PdhAddEnglishCounterW(Query, path, 0, &Counter) != ERROR_SUCCESS)
            {
                PdhCloseQuery(Query);
                Query = nullptr;
                return false;
            }
            return true;
        }

        void Start()
        {
            if (Query)
                PdhCollectQueryData(Query);
        }

        // Percent of one 3D engine over the interval since Start; negative when unavailable.
        double Stop()
        {
            if (!Query || PdhCollectQueryData(Query) != ERROR_SUCCESS)
                return -1;
            constexpr DWORD FormatFlags = PDH_FMT_DOUBLE | PDH_FMT_NOCAP100;
            DWORD size = 0;
            DWORD count = 0;
            if (PdhGetFormattedCounterArrayW(Counter, FormatFlags, &size, &count, nullptr) != PDH_STATUS(PDH_MORE_DATA))
                return -1;
            std::vector<uint8_t> buffer(size);
            auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
            if (PdhGetFormattedCounterArrayW(Counter, FormatFlags, &size, &count, items) != ERROR_SUCCESS)
                return -1;
            double sum = 0;
            for (DWORD i = 0; i < count; ++i)
                if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA)
                    sum += items[i].FmtValue.doubleValue;
            return sum;
        }

    private:
        PDH_HQUERY Query = nullptr;
        PDH_HCOUNTER Counter = nullptr;
    };

    struct MemoryUsage
    {
        uint64_t WorkingSet = 0;
        uint64_t PeakWorkingSet = 0;
        uint64_t Private = 0;               // private bytes (commit)
        int64_t GpuLocal = -1;              // this process's video memory on the adapter; < 0 = unavailable
    };

    inline MemoryUsage QueryMemory(IDXGIAdapter3* adapter)
    {
        MemoryUsage m;
        PROCESS_MEMORY_COUNTERS_EX counters = {};
        counters.cb = sizeof(counters);
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
        {
            m.WorkingSet = counters.WorkingSetSize;
            m.PeakWorkingSet = counters.PeakWorkingSetSize;
            m.Private = counters.PrivateUsage;
        }
        DXGI_QUERY_VIDEO_MEMORY_INFO video = {};
        if (adapter && SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &video)))
            m.GpuLocal = int64_t(video.CurrentUsage);
        return m;
    }

    // ------------------------------------------------------------------------------------
    // System info
    // ------------------------------------------------------------------------------------

#ifdef _DEBUG
    inline constexpr bool IsDebugBuild = true;
#else
    inline constexpr bool IsDebugBuild = false;
#endif

#if defined(__clang__)
    inline constexpr const char* Compiler = "clang " __clang_version__;
#else
    inline constexpr const char* Compiler = "MSVC " _CRT_STRINGIZE(_MSC_FULL_VER);
#endif

    struct SystemInfo
    {
        std::string Cpu;
        std::string Gpu;
        uint64_t GpuMemory = 0;             // dedicated video memory, bytes
        uint32_t RefreshRate = 0;           // Hz, monitor of the bench window
        std::string Windows;
        uint64_t ExeSize = 0;
        bool OnBattery = false;
        double CyclesPerMs = 0;             // thread cycle clock
    };

    inline std::string CpuBrand()
    {
        int regs[4] = {};
        __cpuid(regs, int(0x80000000u));
        if (uint32_t(regs[0]) < 0x80000004u)
            return "unknown";
        char brand[49] = {};
        for (int i = 0; i < 3; ++i)
        {
            __cpuid(regs, int(0x80000002u) + i);
            std::memcpy(brand + 16 * i, regs, sizeof(regs));
        }
        return Trim(brand);
    }

    inline std::string WindowsVersion()
    {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        FARPROC proc = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
        OSVERSIONINFOW v = {};
        v.dwOSVersionInfoSize = sizeof(v);
        if (!proc || reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(proc))(&v) != 0)
            return "unknown";
        DWORD revision = 0;
        DWORD size = sizeof(revision);
        RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", RRF_RT_REG_DWORD, nullptr, &revision, &size);
        return Format("%lu.%lu.%lu.%lu%s", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber, revision,
                      v.dwBuildNumber >= 22000 ? " (Windows 11)" : "");
    }

    inline std::wstring ExePath()
    {
        std::wstring path(32768, L'\0');
        path.resize(GetModuleFileNameW(nullptr, path.data(), DWORD(path.size())));
        return path;
    }

    inline uint64_t FileSize(const std::wstring& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA data = {};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
            return 0;
        return (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    }

    inline uint32_t RefreshRate(HWND window)
    {
        MONITORINFOEXW monitor = {};
        monitor.cbSize = sizeof(monitor);
        DEVMODEW mode = {};
        mode.dmSize = sizeof(mode);
        if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &monitor) ||
            !EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode))
            return 0;
        return mode.dmDisplayFrequency;
    }

    inline SystemInfo QuerySystem(HWND window, IDXGIAdapter1* adapter)
    {
        SystemInfo s;
        s.Cpu = CpuBrand();
        DXGI_ADAPTER_DESC1 desc = {};
        if (adapter && SUCCEEDED(adapter->GetDesc1(&desc)))
        {
            s.Gpu = ToUtf8(desc.Description);
            s.GpuMemory = desc.DedicatedVideoMemory;
        }
        s.RefreshRate = RefreshRate(window);
        s.Windows = WindowsVersion();
        s.ExeSize = FileSize(ExePath());
        SYSTEM_POWER_STATUS power = {};
        s.OnBattery = GetSystemPowerStatus(&power) && power.ACLineStatus == 0;
        s.CyclesPerMs = MeasureCyclesPerMs();
        return s;
    }

    // ------------------------------------------------------------------------------------
    // Neutral scene model
    // ------------------------------------------------------------------------------------

    // Colors are packed RGBA with R in the lowest byte: the layout of Dear ImGui's IM_COL32 and of
    // Funky::Color's bytes. Alpha 0 means "absent" (no border, no gradient, ...).
    constexpr uint32_t PackColor(uint32_t rgb, uint32_t alpha)
    {
        return ((rgb >> 16) & 0xFF) | (rgb & 0xFF00) | ((rgb & 0xFF) << 16) | (alpha << 24);
    }

    constexpr bool HasAlpha(uint32_t color) { return (color >> 24) != 0; }

    enum class PrimKind : uint8_t
    {
        Rect,       // fill + optional 1-DIP border + corner radius
        Circle,
        Arc,
        Line,
        Polyline,
        Bezier,     // cubic
        Triangle,
        Polygon,    // convex
        Text,
    };

    constexpr int MaxPrimPoints = 16;
    constexpr int MaxTextLength = 31;

    // One background primitive, logical coordinates. Plain data: no heap per primitive.
    struct Prim
    {
        PrimKind Kind = PrimKind::Rect;
        uint8_t PointCount = 0;             // Polyline, Polygon
        uint8_t Font = 0;                   // Text: index into FontFamilies
        uint8_t TextLength = 0;
        uint32_t Color = 0;                 // fill (shapes), stroke (lines) or text color
        uint32_t BorderColor = 0;           // Rect, Circle: 1-DIP border
        uint32_t GradientColor = 0;         // Rect, Circle: linear gradient Color -> GradientColor
        float GradientAngle = 90;           // degrees: 0 = left to right, 90 = top to bottom
        float Radius = 0;                   // Rect: corner radius; Circle, Arc: radius
        float Thickness = 1;                // Arc, Line, Polyline, Bezier
        float StartAngle = 0;               // Arc: degrees, clockwise from +X
        float SweepAngle = 0;
        float FontSize = 14;
        Point Size;                         // Rect
        Point Points[MaxPrimPoints];        // Rect, Text: top-left; Circle, Arc: center; others: vertices / control points
        Point ShadowOffset;                 // shadow: blur 0 = none
        float ShadowBlur = 0;
        uint32_t ShadowColor = 0;
        float GlowRadius = 0;               // glow: radius 0 = none
        uint32_t GlowColor = 0;
        char Text[MaxTextLength + 1];
    };

    // Menu panel: a title, a 2-column grid (label | slider), check boxes, a row of buttons and a toggle switch.
    constexpr float PanelWidth = 280;
    constexpr int RowCount = 10;
    constexpr int CheckCount = 6;
    constexpr int ButtonCount = 3;
    constexpr int ControlsPerPanel = RowCount * 2 + CheckCount + ButtonCount + 1;

    inline constexpr const char* RowLabels[RowCount] = {
        "Parameter 01", "Parameter 02", "Parameter 03", "Parameter 04", "Parameter 05",
        "Parameter 06", "Parameter 07", "Parameter 08", "Parameter 09", "Parameter 10",
    };
    inline constexpr const char* CheckLabels[CheckCount] = { "Option 1", "Option 2", "Option 3", "Option 4", "Option 5", "Option 6" };
    inline constexpr const char* ButtonLabels[ButtonCount] = { "Apply", "Reset", "Close" };

    // The custom toggle switch, drawn the same way by both benchmarks.
    inline constexpr const char* ToggleLabel = "Enabled";
    constexpr float TrackWidth = 34;
    constexpr float TrackHeight = 18;
    constexpr float ToggleGap = 8;
    constexpr uint32_t ToggleOffColor = PackColor(0x2A2F3C, 0xFF);
    constexpr uint32_t ToggleOnColor = PackColor(0x3AA8FF, 0xFF);
    constexpr uint32_t ToggleBorderColor = PackColor(0xFFFFFF, 0x1A);
    constexpr uint32_t ToggleKnobColor = PackColor(0xF2F4F8, 0xFF);

    struct PanelModel
    {
        char Title[24];
        Point Position;                     // top-left, logical
        float Values[RowCount];             // slider values, 0..1
        bool Checks[CheckCount];
        bool Toggle;
    };

    struct FrameModel
    {
        std::vector<Prim> Prims;            // background, drawn below the panels
        std::vector<PanelModel> Panels;

        size_t Items() const { return Prims.size() + Panels.size() * ControlsPerPanel; }
    };

    constexpr int FontCount = 3;
    inline constexpr const char* FontFamilies[FontCount] = { "Segoe UI", "Consolas", "Arial" };
    inline constexpr const wchar_t* FontFiles[FontCount] = { L"segoeui.ttf", L"consola.ttf", L"arial.ttf" };
    constexpr float DefaultFontSize = 14;   // font 0 at this size is the default UI font of both libraries

    enum class SceneId : uint8_t
    {
        Idle,
        Menu,
        Background,
        Mixed,
        TextStatic,
        TextDynamic,
        TextFonts,
        Fill,
        Effects,
        Ramp,
    };

    struct Scene
    {
        SceneId Id;
        const char* Name;
        bool Interactive;                   // has panels: the UI runs in menu (interactive) mode, hit testing on
        bool Frozen;                        // content never changes (generated for frame 0)
        bool ThroughputOnly;
    };

    inline constexpr Scene Scenes[] = {
        //  Id                    Name           Interactive Frozen ThroughputOnly
        { SceneId::Idle,        "Idle",        true,  true,  false },
        { SceneId::Menu,        "Menu",        true,  false, false },
        { SceneId::Background,  "Background",  false, false, false },
        { SceneId::Mixed,       "Mixed",       true,  false, false },
        { SceneId::TextStatic,  "TextStatic",  false, false, false },
        { SceneId::TextDynamic, "TextDynamic", false, false, false },
        { SceneId::TextFonts,   "TextFonts",   false, false, false },
        { SceneId::Fill,        "Fill",        false, false, false },
        { SceneId::Effects,     "Effects",     false, false, false },
        { SceneId::Ramp,        "Ramp",        false, false, true },
    };

    // ------------------------------------------------------------------------------------
    // Scene generation: deterministic, from the frame index only
    // ------------------------------------------------------------------------------------

    inline constexpr uint32_t Palette[] = { 0x3AA8FF, 0x5CE08A, 0xFFB547, 0xFF6A3D, 0xB07CFF, 0x4DD8E0, 0xFF5C8A, 0xE6E8EE };
    constexpr uint32_t TextColor = PackColor(0xE6E8EE, 0xFF);
    constexpr double Pi = 3.14159265358979323846;

    // Object count at the given scale (never 0).
    inline uint32_t Count(double base, double scale)
    {
        return uint32_t(std::max(1.0, std::round(base * scale)));
    }

    inline uint32_t Hash(uint32_t x)
    {
        x ^= x >> 16;
        x *= 0x7FEB352Du;
        x ^= x >> 15;
        x *= 0x846CA68Bu;
        x ^= x >> 16;
        return x;
    }

    // A fixed random value in [0, 1) per object and purpose (salt): the same in every frame and in both benchmarks.
    inline float Random(uint32_t object, uint32_t salt)
    {
        return float(Hash(object * 0x9E3779B9u + Hash(salt)) >> 8) / 16777216.0f;
    }

    class SceneBuilder
    {
    public:
        SceneBuilder(FrameModel& model, uint32_t frame, uint32_t flipScale) : Model(model), Frame(frame), FlipScale(flipScale) {}

        // Panels cascade over the viewport at fixed positions; slider values, check boxes and the toggle change.
        void Panels(uint32_t count)
        {
            constexpr float TallestPanel = 580; // of either library: keeps every panel inside the viewport
            float stepX = count > 1 ? (ViewWidth - 32 - PanelWidth) / float(count - 1) : 0;
            float stepY = count > 1 ? (ViewHeight - 32 - TallestPanel) / float(count - 1) : 0;
            for (uint32_t i = 0; i < count; ++i)
            {
                PanelModel& p = Model.Panels.emplace_back();
                std::snprintf(p.Title, sizeof(p.Title), "Panel %02u", i + 1);
                p.Position = { 16 + stepX * float(i), 16 + stepY * float(i) };
                for (int row = 0; row < RowCount; ++row)
                    p.Values[row] = 0.5f + 0.45f * float(std::sin(Frame * 0.03 + i * 0.7 + row * 0.45));
                for (uint32_t c = 0; c < CheckCount; ++c)
                    p.Checks[c] = (Frame + i * 7 + c * 13) / (40 * FlipScale) % 2 == 1; // every 40 frames (x FlipScale), staggered
                p.Toggle = (Frame + i * 11) / (90 * FlipScale) % 2 == 1;
            }
        }

        void Background(double scale)
        {
            for (uint32_t i = 0, n = Count(2000, scale); i < n; ++i)
            {
                Prim& p = Add(PrimKind::Rect, PaletteColor(i, 1, 0xE0));
                p.Size = { 24 + 48 * Random(i, 2), 14 + 26 * Random(i, 3) };
                p.Points[0] = Place(i, 4, p.Size, 40);
                p.Radius = 4 + 4 * Random(i, 5);
                p.BorderColor = PackColor(0xFFFFFF, 0x40);
            }
            for (uint32_t i = 0, n = Count(1000, scale); i < n; ++i)
            {
                Prim& p = Add(PrimKind::Circle, PaletteColor(i, 10, 0xC0));
                p.Points[0] = Place(i, 11, {}, 40);
                p.Radius = 4 + 14 * Random(i, 12);
            }
            for (uint32_t i = 0, n = Count(300, scale); i < n; ++i)
            {
                Prim& p = Add(PrimKind::Arc, PaletteColor(i, 20, 0xFF));
                p.Points[0] = Place(i, 21, {}, 40);
                p.Radius = 10 + 18 * Random(i, 22);
                p.Thickness = 2;
                p.StartAngle = float(std::fmod(Frame * 3.0 + 360.0 * Random(i, 23), 360.0));
                p.SweepAngle = 90 + 210 * Random(i, 24);
            }
            for (uint32_t i = 0, n = Count(1000, scale); i < n; ++i)
            {
                Prim& p = Add(PrimKind::Line, PaletteColor(i, 30, 0xD0));
                Point center = Place(i, 31, {}, 40);
                double angle = Angle(i, 32);
                float half = 10 + 35 * Random(i, 33);
                p.Points[0] = { center.X - half * float(std::cos(angle)), center.Y - half * float(std::sin(angle)) };
                p.Points[1] = { center.X + half * float(std::cos(angle)), center.Y + half * float(std::sin(angle)) };
                p.Thickness = 1.5f;
            }
            for (uint32_t i = 0, n = Count(200, scale); i < n; ++i)
            {
                Prim& p = Add(PrimKind::Polyline, PaletteColor(i, 40, 0xE0));
                Point origin = Place(i, 41, { 110, 20 }, 30);
                double phase = Random(i, 42) * 2 * Pi;
                p.PointCount = 12;
                for (int k = 0; k < 12; ++k)
                    p.Points[k] = { origin.X + 10.0f * float(k), origin.Y + 10 * float(std::sin(k * 0.8 + Frame * 0.05 + phase)) };
                p.Thickness = 1.5f;
            }
            for (uint32_t i = 0, n = Count(400, scale); i < n; ++i)
                AddBezier(i, 50, PaletteColor(i, 51, 0xE0));
            for (uint32_t i = 0, n = Count(800, scale); i < n; ++i)
            {
                Prim& p = Add(PrimKind::Triangle, PaletteColor(i, 60, 0xD0));
                p.PointCount = 3;
                RegularPolygon(p, Place(i, 61, {}, 40), 8 + 12 * Random(i, 62), Angle(i, 63));
            }
            for (uint32_t i = 0, n = Count(400, scale); i < n; ++i)
            {
                Prim& p = Add(PrimKind::Polygon, PaletteColor(i, 70, 0xD0));
                p.PointCount = 6;
                RegularPolygon(p, Place(i, 71, {}, 40), 8 + 8 * Random(i, 72), Angle(i, 73));
            }
            for (uint32_t i = 0, n = Count(800, scale); i < n; ++i)
            {
                Prim& p = Add(PrimKind::Text, TextColor);
                p.Points[0] = Place(i, 80, { 80, 16 }, 40);
                p.FontSize = 13;
                if (i % 2)
                    SetText(p, "Value %05u", (Frame * 7 + i * 13) % 100000);
                else
                    SetText(p, "Label %04u", i);
            }
        }

        void Texts(uint32_t count, bool dynamic)
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                Prim& p = Add(PrimKind::Text, i % 4 ? TextColor : PaletteColor(i, 90, 0xFF));
                p.Points[0] = Place(i, 91, { 110, 18 }, 30);
                p.FontSize = 14;
                if (dynamic)
                    SetText(p, "HP %04u  %6.2f m", (Frame * 3 + i * 17) % 10000, std::fmod(Frame * 0.37 + i * 1.3, 1000.0));
                else
                    SetText(p, "Target %04u", i);
            }
        }

        // Each of the fonts at each size, perSize static strings.
        void FontTexts(uint32_t perSize)
        {
            constexpr float Sizes[] = { 12, 14, 18, 24, 32, 48 };
            uint32_t i = 0;
            for (uint8_t font = 0; font < FontCount; ++font)
                for (float size : Sizes)
                    for (uint32_t k = 0; k < perSize; ++k, ++i)
                    {
                        Prim& p = Add(PrimKind::Text, i % 3 ? TextColor : PaletteColor(i, 100, 0xFF));
                        p.Font = font;
                        p.FontSize = size;
                        p.Points[0] = Place(i, 101, { size * 5, size }, 30);
                        SetText(p, "Sample %03u", k);
                    }
        }

        // Large translucent shapes: heavy overdraw.
        void Fill(double scale)
        {
            for (uint32_t i = 0, n = Count(80, scale); i < n; ++i)
            {
                Prim& p = Add(PrimKind::Rect, PaletteColor(i, 110, 0x40)); // alpha 0.25
                p.Size = { 480, 320 };
                p.Points[0] = Place(i, 111, p.Size, 120);
                p.Radius = 16;
            }
            for (uint32_t i = 0, n = Count(40, scale); i < n; ++i)
            {
                Prim& p = Add(PrimKind::Circle, PaletteColor(i, 120, 0x33)); // alpha 0.2
                p.Points[0] = Place(i, 121, {}, 150);
                p.Radius = 220;
            }
        }

        void Effects(double scale)
        {
            for (uint32_t i = 0, n = Count(600, scale); i < n; ++i)
            {
                uint32_t color = Hash(i + 130);
                Prim& p = Add(PrimKind::Rect, PackColor(Palette[color % 8], 0xFF));
                p.GradientColor = PackColor(Palette[(color + 3) % 8], 0xFF);
                p.GradientAngle = 90;
                p.Size = { 56 + 24 * Random(i, 131), 32 + 16 * Random(i, 132) };
                p.Points[0] = Place(i, 133, p.Size, 40);
                p.Radius = 8;
                p.ShadowOffset = { 0, 4 };
                p.ShadowBlur = 12;
                p.ShadowColor = PackColor(0x000000, 0x80);
                p.GlowRadius = 10;
                p.GlowColor = PackColor(Palette[color % 8], 0x80);
            }
            for (uint32_t i = 0, n = Count(200, scale); i < n; ++i)
            {
                Prim& p = AddBezier(i, 140, PaletteColor(i, 141, 0xFF));
                p.GlowRadius = 8;
                p.GlowColor = (p.Color & 0x00FFFFFFu) | (0x90u << 24);
            }
            for (uint32_t i = 0, n = Count(300, scale); i < n; ++i)
            {
                Prim& p = Add(PrimKind::Circle, PaletteColor(i, 150, 0xFF));
                p.Points[0] = Place(i, 151, {}, 40);
                p.Radius = 6 + 10 * Random(i, 152);
                p.GlowRadius = 12;
                p.GlowColor = (p.Color & 0x00FFFFFFu) | (0x80u << 24);
            }
        }

        // Small moving objects, rects / circles / lines / texts in equal parts.
        void Ramp(uint32_t count)
        {
            uint32_t quarter = std::max(count / 4, 1u);
            for (uint32_t i = 0; i < quarter; ++i)
            {
                Prim& p = Add(PrimKind::Rect, PaletteColor(i, 160, 0xE0));
                p.Size = { 20, 14 };
                p.Points[0] = Place(i, 161, p.Size, 40);
                p.Radius = 3;
            }
            for (uint32_t i = 0; i < quarter; ++i)
            {
                Prim& p = Add(PrimKind::Circle, PaletteColor(i, 170, 0xE0));
                p.Points[0] = Place(i, 171, {}, 40);
                p.Radius = 6;
            }
            for (uint32_t i = 0; i < quarter; ++i)
            {
                Prim& p = Add(PrimKind::Line, PaletteColor(i, 180, 0xE0));
                Point center = Place(i, 181, {}, 40);
                double angle = Angle(i, 182);
                p.Points[0] = { center.X - 12 * float(std::cos(angle)), center.Y - 12 * float(std::sin(angle)) };
                p.Points[1] = { center.X + 12 * float(std::cos(angle)), center.Y + 12 * float(std::sin(angle)) };
                p.Thickness = 1.5f;
            }
            for (uint32_t i = 0; i < quarter; ++i)
            {
                Prim& p = Add(PrimKind::Text, TextColor);
                p.Points[0] = Place(i, 190, { 50, 16 }, 40);
                p.FontSize = 12;
                SetText(p, "Obj %03u", i % 1000);
            }
        }

    private:
        Prim& Add(PrimKind kind, uint32_t color)
        {
            Prim& p = Model.Prims.emplace_back();
            p.Kind = kind;
            p.Color = color;
            return p;
        }

        Prim& AddBezier(uint32_t i, uint32_t salt, uint32_t color)
        {
            Prim& p = Add(PrimKind::Bezier, color);
            Point start = Place(i, salt, { 120, 60 }, 30);
            double phase = Random(i, salt + 1) * 2 * Pi;
            float bend = 50 * float(std::sin(Frame * 0.04 + phase));
            p.PointCount = 4;
            p.Points[0] = { start.X, start.Y + 30 };
            p.Points[1] = { start.X + 40, start.Y + 30 - bend };
            p.Points[2] = { start.X + 80, start.Y + 30 + bend };
            p.Points[3] = { start.X + 120, start.Y + 30 };
            p.Thickness = 2;
            return p;
        }

        uint32_t PaletteColor(uint32_t object, uint32_t salt, uint32_t alpha) const
        {
            return PackColor(Palette[Hash(object * 31 + salt) % std::size(Palette)], alpha);
        }

        // A fixed random spot (keeping `extent` inside the viewport) plus a drift of `amplitude` that changes every frame.
        Point Place(uint32_t object, uint32_t salt, Point extent, float amplitude) const
        {
            float x = Random(object, salt) * (ViewWidth - extent.X);
            float y = Random(object, salt + 1000) * (ViewHeight - extent.Y);
            double phase = Random(object, salt + 2000) * 2 * Pi;
            double speed = 0.01 + 0.03 * Random(object, salt + 3000); // radians per frame
            double t = Frame * speed + phase;
            return { x + amplitude * float(std::sin(t)), y + amplitude * float(std::cos(t * 0.8)) };
        }

        // Rotation that changes every frame.
        double Angle(uint32_t object, uint32_t salt) const
        {
            return Random(object, salt) * 2 * Pi + Frame * (0.01 + 0.03 * Random(object, salt + 1000));
        }

        static void RegularPolygon(Prim& p, Point center, float radius, double angle)
        {
            for (int k = 0; k < p.PointCount; ++k)
            {
                double a = angle + 2 * Pi * k / p.PointCount;
                p.Points[k] = { center.X + radius * float(std::cos(a)), center.Y + radius * float(std::sin(a)) };
            }
        }

        BENCH_PRINTF_FORMAT(2, 3) static void SetText(Prim& p, const char* format, ...)
        {
            va_list args;
            va_start(args, format);
            int length = std::vsnprintf(p.Text, sizeof(p.Text), format, args);
            va_end(args);
            p.TextLength = uint8_t(std::clamp(length, 0, MaxTextLength));
        }

        FrameModel& Model;
        uint32_t Frame;
        uint32_t FlipScale;
    };

    // Throughput runs about 25 times as many frames per second as a 100 Hz display, so check boxes and toggles
    // flip 25 times less often per frame: about as often per second as in Display, like a user clicking. Flipped
    // every 40 frames at ~2600 FPS (15 ms) they would never finish a transition. Both libraries get the same frames.
    constexpr uint32_t ThroughputFlipScale = 25;

    // rampCount: number of objects of the Ramp scene. flipScale: see ThroughputFlipScale.
    inline void GenerateFrame(const Scene& scene, uint32_t frame, double scale, uint32_t rampCount, uint32_t flipScale, FrameModel& model)
    {
        model.Prims.clear();
        model.Panels.clear();
        SceneBuilder b(model, scene.Frozen ? 0 : frame, flipScale);
        switch (scene.Id)
        {
            case SceneId::Idle:
                b.Background(scale * 0.05);
                b.Panels(Count(24, scale));
                break;
            case SceneId::Menu:
                b.Panels(Count(24, scale));
                break;
            case SceneId::Background:
                b.Background(scale);
                break;
            case SceneId::Mixed:
                b.Background(scale * 0.5);
                b.Panels(Count(12, scale));
                break;
            case SceneId::TextStatic:
                b.Texts(Count(3000, scale), false);
                break;
            case SceneId::TextDynamic:
                b.Texts(Count(3000, scale), true);
                break;
            case SceneId::TextFonts:
                b.FontTexts(Count(120, scale));
                break;
            case SceneId::Fill:
                b.Fill(scale);
                break;
            case SceneId::Effects:
                b.Effects(scale);
                break;
            case SceneId::Ramp:
                b.Ramp(rampCount);
                break;
        }
    }

    // ------------------------------------------------------------------------------------
    // The library under test
    // ------------------------------------------------------------------------------------

    enum class Mode : uint8_t
    {
        Display,        // the bench window, vsync: the overlay situation
        Throughput,     // offscreen target, no Present, frames in flight, GPU time from timestamp queries
    };

    inline const char* ModeName(Mode mode) { return mode == Mode::Display ? "Display" : "Throughput"; }

    struct DrawCounts
    {
        uint64_t Vertices = 0;
        uint64_t Indices = 0;
    };

    class Backend
    {
    public:
        virtual ~Backend() = default;

        virtual const char* Name() const = 0;
        virtual std::string Version() const = 0;

        // Display: draws into `window` and presents with vsync (max frame latency 1, waitable object).
        // Throughput: draws into `target` (cleared by the runner right before EndFrame), no Present.
        virtual bool Open(Mode mode, const BenchWindow& window, const OffscreenTarget& target) = 0;
        virtual void Close() = 0;
        virtual void BeginScene(const Scene& scene) = 0;    // before the warmup of every scene

        virtual bool BeginFrame() = 0;                      // display: waits for the frame slot and pumps messages; false = stop
        virtual void Build(const FrameModel& model) = 0;    // translates the model into library calls
        virtual void EndFrame() = 0;                        // display: renders and presents; throughput: renders

        virtual DrawCounts LastDrawCounts() const { return {}; } // of the last frame, where the library exposes them
    };

    // ------------------------------------------------------------------------------------
    // Report
    // ------------------------------------------------------------------------------------

    // Console output, flushed as it goes, collected for the results file as well.
    class Report
    {
    public:
        void Write(std::string_view text)
        {
            std::fwrite(text.data(), 1, text.size(), stdout);
            std::fflush(stdout);
            Text += text;
        }

        BENCH_PRINTF_FORMAT(2, 3) void Print(const char* format, ...)
        {
            va_list args;
            va_start(args, format);
            Write(FormatV(format, args));
            va_end(args);
        }

        const std::string& Content() const { return Text; }

    private:
        std::string Text;
    };

    // Text table: the first column left-aligned, the others right-aligned.
    class Table
    {
    public:
        explicit Table(std::vector<std::string> header) { Rows.push_back(std::move(header)); }

        void Add(std::vector<std::string> row) { Rows.push_back(std::move(row)); }

        std::string Render() const
        {
            std::vector<size_t> widths;
            for (const auto& row : Rows)
                for (size_t c = 0; c < row.size(); ++c)
                {
                    widths.resize(std::max(widths.size(), c + 1));
                    widths[c] = std::max(widths[c], row[c].size());
                }
            std::string out;
            for (size_t r = 0; r < Rows.size(); ++r)
            {
                std::string line = " ";
                for (size_t c = 0; c < Rows[r].size(); ++c)
                {
                    std::string padding(widths[c] - Rows[r][c].size(), ' ');
                    line += "  " + (c == 0 ? Rows[r][c] + padding : padding + Rows[r][c]);
                }
                out += line.substr(0, line.find_last_not_of(' ') + 1) + "\n";
                if (r == 0)
                {
                    size_t length = 0;
                    for (size_t width : widths)
                        length += width + 2;
                    out += "   " + std::string(length - 2, '-') + "\n";
                }
            }
            return out;
        }

    private:
        std::vector<std::vector<std::string>> Rows;
    };

    struct SceneResult
    {
        Bench::Mode Mode = Bench::Mode::Display;
        std::string Scene;
        uint64_t Items = 0;                 // primitives + controls
        int Frames = 0;
        double Fps = 0;                     // display: measured; throughput: 1000 / max(CPU frame ms, GPU ms)
        double LoopFps = 0;                 // frames the loop really ran per second (throughput: without scene generation,
                                            // until the GPU finished the last frame)
        Distribution IntervalMs;            // frame to frame (throughput: CPU frame wall time, without generation and GPU wait)
        Distribution CyclesM;               // CPU megacycles of the frame loop thread per frame
        Distribution BuildMs;
        Distribution SubmitMs;
        Distribution GpuWaitMs;             // throughput: spinning for the oldest frame in flight
        Distribution GpuFrameMs;            // throughput: GPU time per frame from timestamp queries
        int GpuFrames = 0;                  // throughput: frames with a valid (not disjoint) GPU time
        double CpuMs = 0;                   // average CPU-busy time of the frame loop thread
        double ProcessCpuPercent = 0;       // all threads, % of one core over the whole loop
        double OtherCpuPercent = 0;         // the same without the frame loop thread (driver / runtime threads)
        double GpuPercent = -1;             // 3D engine of this process; < 0 = unavailable
        double GpuMs = -1;                  // display: GpuPercent x average frame interval; throughput: GpuFrameMs.Avg
        double GpuBusyMs = -1;              // throughput: GpuPercent / LoopFps, the GPU's busy time per frame
        double HeadroomFps = 0;             // display: 1000 / max(CpuMs, GpuMs); throughput: Fps
        MemoryUsage Memory;                 // process, at the end of the scene
        double Vertices = 0;                // average per frame, where the library exposes them
        double Indices = 0;
    };

    inline std::string Fixed(double value, int decimals) { return Format("%.*f", decimals, value); }
    inline std::string OrDash(double value, int decimals) { return value < 0 ? "-" : Fixed(value, decimals); }
    inline std::string Megabytes(uint64_t bytes) { return Fixed(double(bytes) / (1024.0 * 1024.0), 1); }
    inline double GpuLocalMb(const MemoryUsage& m) { return m.GpuLocal < 0 ? -1 : double(m.GpuLocal) / (1024.0 * 1024.0); }

    inline std::string Percentiles(const Distribution& d, int decimals)
    {
        return Format("%.*f/%.*f/%.*f", decimals, d.P50, decimals, d.P95, decimals, d.P99);
    }

    inline std::string AvgPercentiles(const Distribution& d, int decimals)
    {
        return Format("%.*f/%.*f/%.*f", decimals, d.Avg, decimals, d.P95, decimals, d.P99);
    }

    // The ring depth is filled in from GpuFramesInFlight, so the text cannot drift from the code.
    inline std::string Legend()
    {
        return Format(
            "How to read (lower is better unless noted):\n"
            "  Display     the bench window with vsync - the overlay situation. FPS is capped by the refresh rate:\n"
            "              compare the costs, not the FPS.\n"
            "  Throughput  offscreen target, no Present, up to %d frames in flight (the CPU waits for the GPU only when\n"
            "              it is that far ahead); the GPU time of every frame comes from timestamp queries.\n"
            "              FPS (higher = better) = 1000 / max(CPU frame ms avg, GPU ms avg): what the frame cost allows\n"
            "              when embedded in a game, where CPU and GPU work in parallel. The Ramp uses this FPS too.\n"
            "  Loop FPS    throughput: frames the loop really ran per second, until the GPU finished the last one\n"
            "              (without scene generation). Close to FPS = the GPU was fed continuously; lower = it idled\n"
            "              (finished frames noticed late) or the clocks dropped.\n"
            "  Interval    display: time between frames p50/p95/p99, ms. A high p99 means stutter.\n"
            "  CPU frame   throughput: wall time of the CPU part of a frame p50/p95/p99, ms: BeginFrame .. flush,\n"
            "              without scene generation and GPU wait.\n"
            "  CPU Mcyc    CPU megacycles of the render thread per frame avg/p95: the whole frame loop except scene\n"
            "              generation, incl. BeginFrame/message pump/Present; blocking waits cost nothing.\n"
            "              CPU ms is the same average in milliseconds at the measured cycle clock.\n"
            "  Build       the UI code: after BeginFrame/NewFrame .. before EndFrame/Render, ms avg/p95/p99.\n"
            "  Submit      EndFrame, or Render + RenderDrawData + Present (throughput: + clear, timestamps, flush),\n"
            "              ms avg/p95/p99.\n"
            "  GPU ms      throughput: GPU time of a frame avg/p95 from timestamps (clear .. last draw); intervals\n"
            "              with a GPU clock change (disjoint) are discarded. It includes the GPU waiting for the commands\n"
            "              while the library renders on the CPU: * = CPU-bound (CPU frame > GPU ms), an upper bound - see\n"
            "              GPU busy. Display: GPU %% x frame interval.\n"
            "  GPU busy    throughput: GPU %% / Loop FPS, ms: what the frame keeps the GPU busy (PDH, ~16 ms ticks).\n"
            "              Compare shader work by it, or by GPU ms in GPU-bound scenes.\n"
            "  GPU wait    throughput: the CPU spinning for the oldest frame in flight when all %d are in flight\n"
            "              (not in CPU Mcyc, CPU frame or FPS).\n"
            "  Proc CPU    display: CPU time of all threads of the process (incl. driver threads and scene\n"
            "              generation), %% of one core over the whole loop.\n"
            "  Other CPU   throughput: the same without the render thread, which spins instead of blocking when it\n"
            "              waits for the GPU (its cost is CPU Mcyc): driver and runtime threads. Both tick at ~16 ms:\n"
            "              noisy on short runs.\n"
            "  GPU %%       3D-engine utilization of this process over the whole loop (PDH).\n"
            "  Headroom    display: 1000 / max(CPU ms, GPU ms): the FPS this frame cost would allow without vsync\n"
            "              (higher = better).\n"
            "  Memory      of the whole process at the end of each scene (MB): working set, peak working set,\n"
            "              private bytes, GPU local memory (- = unavailable). Cumulative over the run: scenes run in\n"
            "              the listed order.\n"
            "  Vtx/Idx     vertices / indices per frame (Dear ImGui draw data).\n"
            "  Items       primitives + controls per frame.\n",
            GpuFramesInFlight, GpuFramesInFlight);
    }

    // ------------------------------------------------------------------------------------
    // Runner: the frame loop, scenes, modes and the report
    // ------------------------------------------------------------------------------------

    struct FrameSample
    {
        int64_t Start = 0;                  // QPC after scene generation (throughput: and after the GPU wait)
        int64_t End = 0;                    // QPC at the end of the frame (throughput: after the flush)
        uint64_t Cycles = 0;                // CPU cycles of this thread for the measured part
        int64_t Build = 0;                  // QPC ticks
        int64_t Submit = 0;
        int64_t GpuWait = 0;                // throughput: waiting for a free timestamp slot, before Start
        double GpuMs = -1;                  // throughput: from timestamps, written a few frames later; < 0 = disjoint
    };

    class Runner
    {
    public:
        Runner(Backend& backend, const Options& options) : Lib(backend), Opt(options) {}

        int Run()
        {
            SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            // Windows 11 may ignore timeBeginPeriod while all windows of the process are hidden (the throughput mode).
            PROCESS_POWER_THROTTLING_STATE throttling = { PROCESS_POWER_THROTTLING_CURRENT_VERSION,
                                                          PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, 0 };
            SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));
            if (!Opt.Only.empty() && SelectedScenes(Mode::Throughput).empty())
            {
                std::printf("Unknown scene: %s\n\n", Opt.Only.c_str());
                PrintUsage();
                return 1;
            }
            if (!CreateBenchWindow(Window, L"UI benchmark"))
            {
                std::printf("Cannot create the bench window.\n");
                return 1;
            }

            ComPtr<IDXGIFactory1> factory;
            ComPtr<IDXGIAdapter1> adapter;
            if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumAdapters1(0, &adapter)))
                adapter.As(&Adapter);
            System = QuerySystem(Window.Handle, adapter.Get());
            PrintHeader();
            if (!Gpu.Open())
                Out.Print("Note: the GPU utilization counters are unavailable (PDH), GPU %% and GPU ms are not reported.\n");

            bool completed = (Opt.ThroughputOnly || RunMode(Mode::Display)) && (Opt.DisplayOnly || RunMode(Mode::Throughput));
            if (!completed)
                Out.Print("\n*** The run was stopped: the results below are partial. ***\n");
            PrintSummary();
            WriteResultsFile();
            DestroyWindow(Window.Handle);
            return completed ? 0 : 2;
        }

    private:
        std::vector<const Scene*> SelectedScenes(Mode mode) const
        {
            std::vector<const Scene*> scenes;
            for (const Scene& scene : Scenes)
                if ((mode == Mode::Throughput || !scene.ThroughputOnly) && (Opt.Only.empty() || EqualsIgnoreCase(Opt.Only, scene.Name)))
                    scenes.push_back(&scene);
            return scenes;
        }

        void PrintHeader()
        {
            SYSTEMTIME now;
            GetLocalTime(&now);
            std::string rule(100, '=');
            Out.Print("%s\n %s %s benchmark   %04u-%02u-%02u %02u:%02u\n%s\n", rule.c_str(), Lib.Name(), Lib.Version().c_str(),
                      now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, rule.c_str());
            if (IsDebugBuild)
                Out.Print("\n !!! DEBUG BUILD: these numbers are meaningless. Build and run Release x64. !!!\n\n");
            Out.Print(" CPU       %s (thread cycle clock %.2f GHz)\n", System.Cpu.c_str(), System.CyclesPerMs / 1e6);
            Out.Print(" GPU       %s (%llu MB dedicated)\n", System.Gpu.c_str(), (unsigned long long)(System.GpuMemory >> 20));
            Out.Print(" Display   %u Hz, DPI scale %.2f, window %ux%u px (%.0fx%.0f DIPs)\n", System.RefreshRate, Window.DpiScale,
                      Window.Width, Window.Height, ViewWidth, ViewHeight);
            Out.Print(" Windows   %s\n", System.Windows.c_str());
            Out.Print(" Build     %s, %s, exe %llu bytes\n", IsDebugBuild ? "DEBUG" : "Release", Compiler, (unsigned long long)System.ExeSize);
            if (System.OnBattery)
                Out.Print(" Power     ON BATTERY: expect lower and noisier numbers, plug in the charger\n");
            Out.Print(" Options   scale %.2f, warmup %d frames (>= %.1f s), display %d frames, throughput %d frames, ramp %d frames/step,\n"
                      "           Dear ImGui effect layers %d\n",
                      Opt.Scale, Opt.Warmup, MinWarmupMs / 1000, Opt.MeasuredFrames(DefaultDisplayFrames),
                      Opt.MeasuredFrames(DefaultThroughputFrames), Opt.RampFrames(), Opt.Layers);
            Out.Print("\n%s\nDo not move the mouse (it is placed in the middle of the window). Esc aborts.\n", Legend().c_str());
        }

        bool RunMode(Mode mode)
        {
            std::vector<const Scene*> scenes = SelectedScenes(mode);
            if (scenes.empty())
                return true;
            Out.Print("\n== %s ==\n", ModeName(mode));

            if (mode == Mode::Display)
                ShowBenchWindow(Window, true);
            else if (!Target.Create(Window.Width, Window.Height))
            {
                Out.Print("Cannot create the D3D11 device / offscreen target.\n");
                return false;
            }

            bool ok = Lib.Open(mode, Window, Target);
            if (!ok)
                Out.Print("%s failed to initialize in %s mode.\n", Lib.Name(), ModeName(mode));
            // Throughput: finished frames are noticed at the timer tick, every 1 ms instead of ~15.6 ms.
            bool fineTimer = mode == Mode::Throughput && timeBeginPeriod(1) == TIMERR_NOERROR;
            for (size_t i = 0; ok && i < scenes.size(); ++i)
                ok = scenes[i]->Id == SceneId::Ramp ? RunRamp(*scenes[i]) : RunScene(*scenes[i], mode);
            if (fineTimer)
                timeEndPeriod(1);
            Lib.Close();

            if (mode == Mode::Display)
                ShowBenchWindow(Window, false);
            else
                Target = {};
            return ok;
        }

        bool RunScene(const Scene& scene, Mode mode)
        {
            Out.Print("  %-12s ", scene.Name);
            int frames = Opt.MeasuredFrames(mode == Mode::Display ? DefaultDisplayFrames : DefaultThroughputFrames);
            SceneResult r;
            if (!Measure(scene, mode, 0, frames, r))
            {
                Out.Print("stopped\n");
                return false;
            }
            PrintProgress(r);
            Results.push_back(r);
            return true;
        }

        // Doubling object counts until the throughput FPS drops below 20.
        bool RunRamp(const Scene& scene)
        {
            for (int step = 0; step <= 8; ++step)
            {
                uint32_t count = Count(1000 << step, Opt.Scale);
                Out.Print("  %-12s ", Format("Ramp %u", count).c_str());
                SceneResult r;
                if (!Measure(scene, Mode::Throughput, count, Opt.RampFrames(), r))
                {
                    Out.Print("stopped\n");
                    return false;
                }
                r.Scene = "Ramp";
                PrintProgress(r);
                RampSteps.push_back(r);
                if (r.Fps < 20)
                    break;
            }
            return true;
        }

        bool Measure(const Scene& scene, Mode mode, uint32_t rampCount, int frames, SceneResult& r)
        {
            Lib.BeginScene(scene);
            if (mode == Mode::Display)
                PlaceCursor(Window);

            // Warmup: caches fill, animations (panel fade-in) finish, CPU and GPU clocks ramp up.
            FrameSample sample;
            uint32_t frame = 0;
            int64_t warmupStart = Now();
            for (int i = 0; i < Opt.Warmup || TicksToMs(Now() - warmupStart) < MinWarmupMs; ++i)
                if (!RunFrame(scene, mode, rampCount, frame++, sample))
                    return Stop();
            Target.Drain(); // the warmup frames in flight write into `sample`

            // Collecting the counters takes a while: two more frames restore the frame pacing after it.
            Gpu.Start();
            for (int i = 0; i < 2; ++i)
                if (!RunFrame(scene, mode, rampCount, frame++, sample))
                    return Stop();
            int64_t start = sample.End;
            uint64_t cpuStart = ProcessCpuTime();
            uint64_t threadStart = ThreadCpuTime();

            std::vector<FrameSample> samples(static_cast<size_t>(frames));
            double vertices = 0;
            double indices = 0;
            for (FrameSample& s : samples)
            {
                if (!RunFrame(scene, mode, rampCount, frame++, s))
                    return Stop();
                DrawCounts counts = Lib.LastDrawCounts();
                vertices += double(counts.Vertices);
                indices += double(counts.Indices);
            }
            uint64_t cpuEnd = ProcessCpuTime();
            uint64_t threadEnd = ThreadCpuTime();
            double gpuPercent = Gpu.Stop();
            double wallMs = TicksToMs(samples.back().End - start);
            // The GPU times of the last frames. The loop ends when the GPU finished them: with a deep ring, leaving
            // them out would overstate the Loop FPS of GPU-bound scenes.
            int64_t drainStart = Now();
            Target.Drain();
            double drainMs = TicksToMs(Now() - drainStart);

            // Display: frame to frame, so that pacing and stutter show. Throughput: the CPU part of the frame alone.
            bool display = mode == Mode::Display;
            std::vector<double> interval, cycles, build, submit, gpuWait, gpuFrame;
            double loopMs = drainMs;
            int64_t previous = start;
            for (const FrameSample& s : samples)
            {
                interval.push_back(TicksToMs(s.End - (display ? previous : s.Start)));
                cycles.push_back(double(s.Cycles) / 1e6);
                build.push_back(TicksToMs(s.Build));
                submit.push_back(TicksToMs(s.Submit));
                gpuWait.push_back(TicksToMs(s.GpuWait));
                if (s.GpuMs >= 0)
                    gpuFrame.push_back(s.GpuMs);
                loopMs += TicksToMs(s.End - s.Start + s.GpuWait);
                previous = s.End;
            }

            r.Mode = mode;
            r.Scene = scene.Name;
            r.Items = Model.Items();
            r.Frames = frames;
            r.IntervalMs = Summarize(std::move(interval));
            r.CyclesM = Summarize(std::move(cycles));
            r.BuildMs = Summarize(std::move(build));
            r.SubmitMs = Summarize(std::move(submit));
            r.GpuWaitMs = Summarize(std::move(gpuWait));
            r.GpuFrames = int(gpuFrame.size());
            r.GpuFrameMs = Summarize(std::move(gpuFrame));
            r.CpuMs = r.CyclesM.Avg * 1e6 / System.CyclesPerMs;
            // Throughput reports the other threads: the frame loop thread never blocks there (it spins when it
            // waits for the GPU), and its own cost is CPU Mcyc.
            uint64_t processCpu = cpuEnd - cpuStart;
            uint64_t otherCpu = processCpu - std::min(processCpu, threadEnd - threadStart);
            r.ProcessCpuPercent = double(processCpu) / 1e4 / wallMs * 100; // 100 ns units -> ms
            r.OtherCpuPercent = double(otherCpu) / 1e4 / wallMs * 100;
            r.GpuPercent = gpuPercent;
            if (display)
            {
                r.Fps = frames * 1000.0 / wallMs;
                r.LoopFps = r.Fps;
                r.GpuMs = gpuPercent < 0 ? -1 : gpuPercent / 100 * r.IntervalMs.Avg;
                r.HeadroomFps = 1000 / std::max(r.CpuMs, r.GpuMs);
            }
            else
            {
                // CPU and GPU overlap when embedded: the slower of the two sets the frame rate.
                r.LoopFps = frames * 1000.0 / loopMs;
                r.GpuMs = r.GpuFrames > 0 ? r.GpuFrameMs.Avg : -1;
                r.GpuBusyMs = gpuPercent < 0 ? -1 : gpuPercent / 100 * 1000 / r.LoopFps;
                r.Fps = 1000 / std::max(r.IntervalMs.Avg, r.GpuMs);
                r.HeadroomFps = r.Fps;
            }
            r.Memory = QueryMemory(Adapter.Get());
            r.Vertices = vertices / frames;
            r.Indices = indices / frames;
            return true;
        }

        // An aborted scene: the frames in flight still point into its samples.
        bool Stop()
        {
            Target.Drain();
            return false;
        }

        bool RunFrame(const Scene& scene, Mode mode, uint32_t rampCount, uint32_t frame, FrameSample& s)
        {
            if (AbortRequested())
                return false;
            bool offscreen = mode == Mode::Throughput;
            GenerateFrame(scene, frame, Opt.Scale, rampCount, offscreen ? ThroughputFlipScale : 1, Model); // not measured

            int64_t waitStart = Now();
            if (offscreen)
                Target.WaitForSlot();
            s.Start = Now();
            s.GpuWait = s.Start - waitStart;
            s.GpuMs = -1;
            uint64_t cycles = ThreadCycles();
            if (!Lib.BeginFrame())
                return false;
            int64_t buildStart = Now();
            Lib.Build(Model);
            int64_t submitStart = Now();
            // The GPU interval starts right before the library renders: neither library issues GPU commands earlier,
            // and an earlier start could include GPU idle time while the CPU builds.
            if (offscreen)
                Target.BeginFrame(&s.GpuMs);
            Lib.EndFrame();
            if (offscreen)
                Target.EndFrame();
            s.End = Now();
            s.Cycles = ThreadCycles() - cycles;
            s.Build = submitStart - buildStart;
            s.Submit = s.End - submitStart;
            return true;
        }

        void PrintProgress(const SceneResult& r)
        {
            Out.Print("%8llu items %8.1f FPS   CPU %7.3f ms   build %7.3f   submit %7.3f", (unsigned long long)r.Items, r.Fps, r.CpuMs,
                      r.BuildMs.Avg, r.SubmitMs.Avg);
            if (r.Mode == Mode::Display)
                Out.Print("   GPU %s ms   headroom %.0f FPS\n", OrDash(r.GpuMs, 3).c_str(), r.HeadroomFps);
            else
                Out.Print("   frame %7.3f   GPU %s ms   loop %.1f FPS%s\n", r.IntervalMs.Avg, OrDash(r.GpuMs, 3).c_str(), r.LoopFps,
                          r.GpuFrames < r.Frames ? Format("   (%d disjoint GPU intervals discarded)", r.Frames - r.GpuFrames).c_str() : "");
        }

        void PrintSummary()
        {
            bool drawCounts = false;
            for (const SceneResult& r : Results)
                drawCounts |= r.Vertices > 0;

            for (Mode mode : { Mode::Display, Mode::Throughput })
            {
                bool display = mode == Mode::Display;
                Table timing(display ? std::vector<std::string>{ "Scene", "Items", "FPS", "Interval p50/95/99", "CPU Mcyc avg/p95", "CPU ms",
                                                                 "Proc CPU %", "GPU %", "GPU ms", "Headroom FPS" }
                                     : std::vector<std::string>{ "Scene", "Items", "FPS", "Loop FPS", "CPU frame ms p50/95/99", "CPU Mcyc avg/p95",
                                                                 "CPU ms", "GPU ms avg/p95", "GPU busy ms", "GPU wait ms", "Other CPU %", "GPU %" });
                std::vector<std::string> costHeader = { "Scene", "Build ms avg/p95/p99", "Submit ms avg/p95/p99", "WS MB", "Peak WS MB",
                                                        "Private MB", "GPU MB" };
                if (drawCounts)
                    costHeader.insert(costHeader.end(), { "Vtx", "Idx" });
                Table costs(costHeader);

                int frames = 0;
                for (const SceneResult& r : Results)
                {
                    if (r.Mode != mode)
                        continue;
                    frames = r.Frames;
                    std::string cycles = Format("%.2f/%.2f", r.CyclesM.Avg, r.CyclesM.P95);
                    if (display)
                        timing.Add({ r.Scene, std::to_string(r.Items), Fixed(r.Fps, 1), Percentiles(r.IntervalMs, 2), cycles, Fixed(r.CpuMs, 3),
                                     Fixed(r.ProcessCpuPercent, 1), OrDash(r.GpuPercent, 1), OrDash(r.GpuMs, 3), Fixed(r.HeadroomFps, 0) });
                    else
                        timing.Add({ r.Scene, std::to_string(r.Items), Fixed(r.Fps, 1), Fixed(r.LoopFps, 1), Percentiles(r.IntervalMs, 3), cycles,
                                     Fixed(r.CpuMs, 3), GpuFrameText(r), OrDash(r.GpuBusyMs, 3), Fixed(r.GpuWaitMs.Avg, 3),
                                     Fixed(r.OtherCpuPercent, 1), OrDash(r.GpuPercent, 1) });
                    std::vector<std::string> row = { r.Scene, AvgPercentiles(r.BuildMs, 3), AvgPercentiles(r.SubmitMs, 3),
                                                     Megabytes(r.Memory.WorkingSet), Megabytes(r.Memory.PeakWorkingSet),
                                                     Megabytes(r.Memory.Private), OrDash(GpuLocalMb(r.Memory), 1) };
                    if (drawCounts)
                        row.insert(row.end(), { Fixed(r.Vertices, 0), Fixed(r.Indices, 0) });
                    costs.Add(std::move(row));
                }
                if (frames == 0)
                    continue;
                Out.Print("\n%s %s - %s (%d frames per scene):\n", Lib.Name(), Lib.Version().c_str(), ModeName(mode), frames);
                Out.Write(timing.Render() + "\n" + costs.Render());
            }
            PrintRampSummary();
        }

        void PrintRampSummary()
        {
            if (RampSteps.empty())
                return;
            Table table({ "Objects", "FPS", "Loop FPS", "CPU frame ms p50/95/99", "CPU ms", "GPU ms avg/p95", "GPU wait ms", "Other CPU %",
                          "WS MB", "GPU MB" });
            for (const SceneResult& r : RampSteps)
                table.Add({ std::to_string(r.Items), Fixed(r.Fps, 1), Fixed(r.LoopFps, 1), Percentiles(r.IntervalMs, 3), Fixed(r.CpuMs, 3),
                            GpuFrameText(r), Fixed(r.GpuWaitMs.Avg, 3), Fixed(r.OtherCpuPercent, 1), Megabytes(r.Memory.WorkingSet),
                            OrDash(GpuLocalMb(r.Memory), 1) });
            Out.Print("\n%s %s - Ramp (throughput, %d frames per step; rects, circles, lines and texts in equal parts):\n", Lib.Name(),
                      Lib.Version().c_str(), RampSteps.front().Frames);
            Out.Write(table.Render() + "\n");

            std::vector<uint32_t> targets = { 60, 144 };
            if (System.RefreshRate && std::find(targets.begin(), targets.end(), System.RefreshRate) == targets.end())
                targets.push_back(System.RefreshRate);
            for (uint32_t fps : targets)
                Out.Print("  Most objects at >= %u throughput FPS%s: %s\n", fps, fps == System.RefreshRate ? " (your refresh rate)" : "",
                          RampCapacity(fps).c_str());
        }

        // The largest measured count reaching `fps`, plus a log-log interpolation towards the next step.
        std::string RampCapacity(double fps) const
        {
            size_t best = RampSteps.size();
            for (size_t i = 0; i < RampSteps.size(); ++i)
                if (RampSteps[i].Fps >= fps)
                    best = i;
            if (best == RampSteps.size())
                return Format("fewer than %llu", (unsigned long long)RampSteps.front().Items);
            const SceneResult& a = RampSteps[best];
            if (best + 1 == RampSteps.size())
                return Format("%llu (the largest step measured)", (unsigned long long)a.Items);
            const SceneResult& b = RampSteps[best + 1];
            double t = (std::log(a.Fps) - std::log(fps)) / (std::log(a.Fps) - std::log(b.Fps));
            double estimate = double(a.Items) * std::pow(double(b.Items) / double(a.Items), std::clamp(t, 0.0, 1.0));
            return Format("%llu (interpolated ~%.0f)", (unsigned long long)a.Items, estimate);
        }

        static std::string GpuFrameText(const SceneResult& r)
        {
            if (r.GpuFrames == 0)
                return "-";
            bool cpuBound = r.IntervalMs.Avg > r.GpuFrameMs.Avg; // the interval includes GPU idle: an upper bound
            return Format("%.3f/%.3f%s", r.GpuFrameMs.Avg, r.GpuFrameMs.P95, cpuBound ? "*" : "");
        }

        std::string CsvLine(const SceneResult& r) const
        {
            // Throughput-only columns are -1 in display rows.
            const Distribution& g = r.GpuFrameMs;
            bool gpu = r.GpuFrames > 0;
            std::string extra = Format(",%.2f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%.4f\n", r.LoopFps, gpu ? g.Avg : -1, gpu ? g.P50 : -1, gpu ? g.P95 : -1,
                                       gpu ? g.P99 : -1, gpu ? g.Max : -1, r.Mode == Mode::Display ? -1 : r.GpuFrames, r.GpuBusyMs);
            return Format("%s,%s,%s,%s,%llu,%d,%.2f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f,%.4f,"
                          "%.1f,%.2f,%.2f,%.2f,%.2f,%.0f,%.0f",
                          Lib.Name(), Lib.Version().c_str(), ModeName(r.Mode), r.Scene.c_str(), (unsigned long long)r.Items, r.Frames, r.Fps,
                          r.IntervalMs.Avg, r.IntervalMs.P50, r.IntervalMs.P95, r.IntervalMs.P99, r.IntervalMs.Max, r.CyclesM.Avg, r.CyclesM.P95,
                          r.CpuMs, r.BuildMs.Avg, r.BuildMs.P95, r.BuildMs.P99, r.SubmitMs.Avg, r.SubmitMs.P95, r.SubmitMs.P99, r.GpuWaitMs.Avg,
                          r.ProcessCpuPercent, r.OtherCpuPercent, r.GpuPercent, r.GpuMs, r.HeadroomFps, double(r.Memory.WorkingSet) / 1048576.0,
                          double(r.Memory.PeakWorkingSet) / 1048576.0, double(r.Memory.Private) / 1048576.0, GpuLocalMb(r.Memory),
                          r.Vertices, r.Indices) +
                   extra;
        }

        // "<ExeName>_results.txt" next to the exe: the report followed by a CSV block.
        void WriteResultsFile()
        {
            std::wstring exe = ExePath();
            std::wstring path = exe.substr(0, exe.find_last_of(L'.')) + L"_results.txt";
            std::string text = Out.Content() + "\nCSV (GPU values < 0 = unavailable)\n"
                               "library,version,mode,scene,items,frames,fps,interval_avg_ms,interval_p50_ms,interval_p95_ms,interval_p99_ms,"
                               "interval_max_ms,cpu_mcycles_avg,cpu_mcycles_p95,cpu_ms,build_avg_ms,build_p95_ms,build_p99_ms,submit_avg_ms,"
                               "submit_p95_ms,submit_p99_ms,gpu_wait_avg_ms,process_cpu_pct,other_cpu_pct,gpu_pct,gpu_ms,headroom_fps,working_set_mb,"
                               "peak_working_set_mb,private_mb,gpu_local_mb,vertices,indices,loop_fps,gpu_frame_avg_ms,gpu_frame_p50_ms,"
                               "gpu_frame_p95_ms,gpu_frame_p99_ms,gpu_frame_max_ms,gpu_frames,gpu_busy_ms\n";
            for (const SceneResult& r : Results)
                text += CsvLine(r);
            for (const SceneResult& r : RampSteps)
                text += CsvLine(r);

            FILE* file = nullptr;
            bool written = _wfopen_s(&file, path.c_str(), L"w") == 0 && file; // text mode: CRLF line ends
            if (written)
            {
                written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
                written &= std::fclose(file) == 0;
            }
            std::printf(written ? "\nResults written to %s\n" : "\nCannot write %s\n", ToUtf8(path).c_str());
        }

        Backend& Lib;
        const Options& Opt;
        BenchWindow Window;
        SystemInfo System;
        OffscreenTarget Target;
        GpuUsage Gpu;
        ComPtr<IDXGIAdapter3> Adapter;      // for this process's GPU memory
        FrameModel Model;
        Report Out;
        std::vector<SceneResult> Results;
        std::vector<SceneResult> RampSteps;
    };

    inline int Run(Backend& backend, const Options& options)
    {
        Runner runner(backend, options);
        return runner.Run();
    }
}
