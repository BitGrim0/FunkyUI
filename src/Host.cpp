// Host — the transparent overlay window that follows the target window.

#include "Internal.h"

#include <windows.h>
#include <dwmapi.h>

#include <new>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "dwmapi.lib")

namespace Funky
{
    namespace
    {
        // Host::Impl is private to Host: the window procedure and helpers work on this base.
        struct HostState
        {
            HWND Target = nullptr;
            HWND Window = nullptr;
            HINSTANCE Instance = nullptr;
            ATOM Atom = 0;
            RECT Client = {};           // target's client area, screen coordinates (physical pixels)
            bool Visible = false;
            bool ClickThrough = true;   // created with WS_EX_TRANSPARENT
            bool Captured = false;
            float Scale = 1;
            InputEvents Events = {};
        };

        void Release(HostState& s, int button)
        {
            if (!s.Events.ButtonDown[button])
                return; // an up without our down (pressed elsewhere) is not a release
            s.Events.ButtonDown[button] = false;
            ++s.Events.Released[button];
        }

        void Press(HostState& s, int button)
        {
            s.Events.ButtonDown[button] = true;
            ++s.Events.Pressed[button];
        }

        // GetAsyncKeyState reports physical buttons: map through the "swap buttons" setting.
        bool IsPhysicallyDown(int button)
        {
            int vk = VK_MBUTTON;
            if (button < 2)
                vk = ((button == 0) == (GetSystemMetrics(SM_SWAPBUTTON) == 0)) ? VK_LBUTTON : VK_RBUTTON;
            return GetAsyncKeyState(vk) < 0;
        }

        bool IsTargetShown(HWND target)
        {
            DWORD cloaked = 0; // e.g. on another virtual desktop
            DwmGetWindowAttribute(target, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            return !IsIconic(target) && IsWindowVisible(target) && !cloaked;
        }

        RECT TargetClientRect(HWND target)
        {
            RECT r = {};
            GetClientRect(target, &r);
            POINT origin = {};
            ClientToScreen(target, &origin);
            OffsetRect(&r, origin.x, origin.y);
            return r;
        }

        bool IsTopmost(HWND window)
        {
            return (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        }

        // Directly above the target, in the target's band: topmost only when the target itself is.
        // HWND_NOTOPMOST only leaves the topmost band (it is a no-op for a non-topmost window), so the
        // non-topmost case climbs with HWND_TOP, which stops below the topmost band.
        void PlaceAboveTarget(const HostState& s)
        {
            constexpr UINT Flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
            bool topmost = IsTopmost(s.Target);
            HWND prev = GetWindow(s.Target, GW_HWNDPREV);
            if (prev == s.Window && IsTopmost(s.Window) == topmost)
                return;
            if (!topmost && IsTopmost(s.Window))
                SetWindowPos(s.Window, HWND_NOTOPMOST, 0, 0, 0, 0, Flags);
            HWND insertAfter = prev && prev != s.Window && IsTopmost(prev) == topmost ? prev
                             : topmost ? HWND_TOPMOST : HWND_TOP;
            SetWindowPos(s.Window, insertAfter, 0, 0, 0, 0, Flags);
        }

        LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
        {
            if (message == WM_NCCREATE)
            {
                auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
                return DefWindowProcW(hwnd, message, wParam, lParam);
            }

            auto* s = reinterpret_cast<HostState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (!s)
                return DefWindowProcW(hwnd, message, wParam, lParam);

            switch (message)
            {
            case WM_MOUSEACTIVATE:
                return MA_NOACTIVATE;
            case WM_LBUTTONDOWN: Press(*s, 0); return 0;
            case WM_RBUTTONDOWN: Press(*s, 1); return 0;
            case WM_MBUTTONDOWN: Press(*s, 2); return 0;
            case WM_LBUTTONUP: Release(*s, 0); return 0;
            case WM_RBUTTONUP: Release(*s, 1); return 0;
            case WM_MBUTTONUP: Release(*s, 2); return 0;
            case WM_MOUSEWHEEL:
                s->Events.Wheel += float(GET_WHEEL_DELTA_WPARAM(wParam)) / 120.0f;
                return 0;
            case WM_CAPTURECHANGED:
                // Capture taken away while held: the button-up will never reach us.
                if (s->Captured)
                {
                    s->Captured = false;
                    for (int i = 0; i < 3; ++i)
                        Release(*s, i);
                }
                return 0;
            case WM_DESTROY:
                s->Window = nullptr;
                return 0;
            default:
                return DefWindowProcW(hwnd, message, wParam, lParam);
            }
        }
    }

    struct Host::Impl : HostState
    {
    };

    bool Host::Init(HWND__* target, std::string_view className, Arena& scratch)
    {
        State = static_cast<Impl*>(MemAlloc(sizeof(Impl)));
        if (!State)
            return false;
        Impl& s = *new (State) Impl{};
        s.Target = target;
        s.Instance = GetModuleHandleW(nullptr);

        wchar_t* name = nullptr;
        Utf8ToUtf16(className, scratch, &name);
        if (!name)
        {
            Shutdown();
            return false;
        }

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = s.Instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = name;
        s.Atom = RegisterClassExW(&wc);
        if (!s.Atom)
        {
            Shutdown();
            return false;
        }

        // The window is Per-Monitor v2 and works in physical pixels whatever the client's thread uses:
        // the thread's context is switched only around such calls (here and in Update).
        DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        s.Client = TargetClientRect(target);
        s.Window = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                   reinterpret_cast<LPCWSTR>(uintptr_t(s.Atom)), nullptr, WS_POPUP,
                                   s.Client.left, s.Client.top, s.Client.right - s.Client.left, s.Client.bottom - s.Client.top,
                                   nullptr, nullptr, s.Instance, static_cast<HostState*>(&s));
        SetThreadDpiAwarenessContext(previous);
        if (!s.Window)
        {
            Shutdown();
            return false;
        }

        SetLayeredWindowAttributes(s.Window, 0, 255, LWA_ALPHA);
        s.Scale = float(GetDpiForWindow(s.Window)) / 96.0f;
        s.Visible = IsTargetShown(target);
        if (s.Visible)
        {
            ShowWindow(s.Window, SW_SHOWNOACTIVATE);
            PlaceAboveTarget(s);
        }
        return true;
    }

    void Host::Shutdown()
    {
        if (!State)
            return;
        Impl& s = *State;
        if (s.Captured)
        {
            s.Captured = false;
            ReleaseCapture();
        }
        if (s.Window)
            DestroyWindow(s.Window);
        if (s.Atom)
            UnregisterClassW(reinterpret_cast<LPCWSTR>(uintptr_t(s.Atom)), s.Instance);
        MemFree(State);
        State = nullptr;
    }

    bool Host::Update()
    {
        Impl& s = *State;

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                PostQuitMessage(int(msg.wParam)); // leave it for the client's own loop
                return false;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (!s.Window || !IsWindow(s.Target))
            return false;

        // A release that went to another window (we were click-through or lost capture).
        for (int i = 0; i < 3; ++i)
            if (s.Events.ButtonDown[i] && !IsPhysicallyDown(i))
                Release(s, i);

        bool visible = IsTargetShown(s.Target);
        bool shown = visible && !s.Visible;
        if (visible != s.Visible)
        {
            s.Visible = visible;
            ShowWindow(s.Window, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
        }

        DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        if (visible)
        {
            // Only while shown: a minimized target reports an empty client rect.
            RECT client = TargetClientRect(s.Target);
            if (!EqualRect(&client, &s.Client))
            {
                s.Client = client;
                SetWindowPos(s.Window, nullptr, client.left, client.top, client.right - client.left, client.bottom - client.top,
                             SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
            }
            if (shown || GetForegroundWindow() == s.Target)
                PlaceAboveTarget(s);
        }

        POINT cursor;
        if (GetCursorPos(&cursor) && ScreenToClient(s.Window, &cursor))
            s.Events.PointerPx = { float(cursor.x), float(cursor.y) };
        SetThreadDpiAwarenessContext(previous);

        s.Scale = float(GetDpiForWindow(s.Window)) / 96.0f;
        return true;
    }

    HWND__* Host::Window() const { return State ? State->Window : nullptr; }
    uint32_t Host::Width() const { return uint32_t(State->Client.right - State->Client.left); }
    uint32_t Host::Height() const { return uint32_t(State->Client.bottom - State->Client.top); }
    bool Host::IsVisible() const { return State->Visible; }
    float Host::DpiScale() const { return State->Scale; }
    const InputEvents& Host::Input() const { return State->Events; }

    void Host::ClearInputEdges()
    {
        InputEvents& e = State->Events;
        MemZero(e.Pressed, sizeof(e.Pressed));
        MemZero(e.Released, sizeof(e.Released));
        e.Wheel = 0;
    }

    void Host::SetClickThrough(bool clickThrough)
    {
        Impl& s = *State;
        if (s.ClickThrough == clickThrough)
            return;
        s.ClickThrough = clickThrough;
        LONG_PTR ex = GetWindowLongPtrW(s.Window, GWL_EXSTYLE);
        ex = clickThrough ? (ex | WS_EX_TRANSPARENT) : (ex & ~LONG_PTR(WS_EX_TRANSPARENT));
        SetWindowLongPtrW(s.Window, GWL_EXSTYLE, ex);
    }

    void Host::SetPointerCapture(bool capture)
    {
        Impl& s = *State;
        if (s.Captured == capture)
            return;
        s.Captured = capture; // cleared before ReleaseCapture so WM_CAPTURECHANGED is not taken as a loss
        if (capture)
            SetCapture(s.Window);
        else
            ReleaseCapture();
    }

    void Host::WaitForFrame(void* frameWaitable, bool presentedLastFrame)
    {
        if (presentedLastFrame && frameWaitable)
            WaitForSingleObjectEx(frameWaitable, 100, TRUE);
        else if (FAILED(DwmFlush()))
            Sleep(1);
    }
}
