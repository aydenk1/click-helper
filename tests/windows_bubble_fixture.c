#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <stdio.h>

static HWND owner;
static const wchar_t *prefix;
static int step;
static const wchar_t bubble_class[] = L"ClickHelperSmokeBubble";

static void show_bubble(void)
{
    RECT rect;
    GetWindowRect(owner, &rect);
    wchar_t title[128];
    swprintf(title, ARRAYSIZE(title), L"%ls-bubble%d", prefix, step);
    wchar_t offset_flag[2];
    int offset = GetEnvironmentVariableW(L"CLICK_HELPER_SMOKE_SHIFT_BUBBLE", offset_flag,
                                         ARRAYSIZE(offset_flag)) ? 20 : 0;
    DWORD style = WS_EX_TOOLWINDOW;
    if (step == 2)
        style |= WS_EX_NOACTIVATE;
    HWND bubble = CreateWindowExW(style, bubble_class, title, WS_POPUP, rect.right - 65 + offset,
                                  rect.top + step * 130, 140, 100, owner, NULL, GetModuleHandleW(NULL), NULL);
    if (!bubble) {
        PostQuitMessage(1);
        return;
    }
    ShowWindow(bubble, step == 1 ? SW_SHOW : SW_SHOWNOACTIVATE);
    if (step == 1)
        SetForegroundWindow(bubble);
}

static LRESULT CALLBACK bubble_events(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_LBUTTONUP) {
        DestroyWindow(window);
        if (step == 1) {
            step = 2;
            SetForegroundWindow(owner);
            show_bubble();
        } else {
            wchar_t title[128];
            swprintf(title, ARRAYSIZE(title), L"%ls-pass", prefix);
            SetWindowTextW(owner, title);
        }
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

static LRESULT CALLBACK owner_events(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_LBUTTONUP && step == 0) {
        /* The harness clicks the body well below the caption to start the sequence. */
        step = 1;
        show_bubble();
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int wmain(int argc, wchar_t **argv)
{
    if (argc != 2 || !SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        return 1;
    prefix = argv[1];
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW window_class = {.lpfnWndProc = owner_events, .hInstance = instance,
                              .lpszClassName = L"ClickHelperSmokeOwner", .hbrBackground = (HBRUSH)(COLOR_WINDOW + 1)};
    if (!RegisterClassW(&window_class))
        return 1;
    window_class.lpfnWndProc = bubble_events;
    window_class.lpszClassName = bubble_class;
    window_class.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    if (!RegisterClassW(&window_class))
        return 1;
    wchar_t title[128];
    swprintf(title, ARRAYSIZE(title), L"%ls-ready", prefix);
    owner = CreateWindowExW(0, L"ClickHelperSmokeOwner", title, WS_OVERLAPPEDWINDOW,
                           40, 40, 800, 600, NULL, NULL, instance, NULL);
    if (!owner)
        return 1;
    ShowWindow(owner, SW_SHOW);
    MSG message;
    BOOL result;
    while ((result = GetMessageW(&message, NULL, 0, 0)) > 0)
        DispatchMessageW(&message);
    return result < 0 ? 1 : (int)message.wParam;
}
