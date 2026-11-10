#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <dwmapi.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

typedef struct { int x, y; } Point;

#ifndef CLICK_HELPER_RECORD
#include "config.h"
#endif

typedef struct {
    const wchar_t *path, *command, *class_name;
    int width, height;
    DWORD startup_ms, timeout_ms;
    bool absolute;
} Settings;

typedef struct {
    HWND window;
    DWORD pid;
    wchar_t class_name[256];
} Target;

typedef struct {
    HWND *windows;
    size_t count, capacity;
} Snapshot;

typedef struct {
    const Settings *settings;
    const Snapshot *before;
    DWORD launched_pid;
    HWND new_window, launched_window;
    unsigned int new_count, launched_count;
} Search;

static _Noreturn void fail(const char *message)
{
    fprintf(stderr, "click-helper: %s\n", message);
    exit(EXIT_FAILURE);
}

static _Noreturn void win_fail(const char *operation)
{
    DWORD code = GetLastError();
    fprintf(stderr, "click-helper: %s failed (Windows error %lu)\n",
            operation, (unsigned long)code);
    exit(EXIT_FAILURE);
}

static void *grow(void *items, size_t *capacity, size_t size)
{
    if (*capacity > SIZE_MAX / size / 2)
        return NULL;
    size_t next = *capacity ? 2 * *capacity : 32;
    void *larger = realloc(items, next * size);
    if (larger)
        *capacity = next;
    return larger;
}

static bool normal_window(HWND window)
{
    DWORD cloaked = 0;
    if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) ||
        (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
        return false;
    return SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked,
                                           sizeof(cloaked))) && !cloaked;
}

static DWORD matching_pid(HWND window, const Settings *settings)
{
    wchar_t class_name[256];
    if (!normal_window(window) ||
        !GetClassNameW(window, class_name, ARRAYSIZE(class_name)) ||
        (settings->class_name[0] && wcscmp(class_name, settings->class_name) != 0))
        return 0;
    DWORD pid = 0;
    if (!GetWindowThreadProcessId(window, &pid))
        return 0;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return 0;
    wchar_t path[32768];
    DWORD length = ARRAYSIZE(path);
    bool matched = QueryFullProcessImageNameW(process, 0, path, &length) &&
        CompareStringOrdinal(path, -1, settings->path, -1, TRUE) == CSTR_EQUAL;
    CloseHandle(process);
    return matched ? pid : 0;
}

static BOOL CALLBACK snapshot_window(HWND window, LPARAM parameter)
{
    Snapshot *snapshot = (Snapshot *)parameter;
    if (snapshot->count == snapshot->capacity) {
        void *larger = grow(snapshot->windows, &snapshot->capacity, sizeof(HWND));
        if (!larger)
            fail("out of memory enumerating windows");
        snapshot->windows = larger;
    }
    snapshot->windows[snapshot->count++] = window;
    return TRUE;
}

static bool was_present(const Snapshot *snapshot, HWND window)
{
    for (size_t i = 0; i < snapshot->count; ++i)
        if (snapshot->windows[i] == window)
            return true;
    return false;
}

static BOOL CALLBACK search_window(HWND window, LPARAM parameter)
{
    Search *search = (Search *)parameter;
    if (was_present(search->before, window))
        return TRUE;
    DWORD pid = matching_pid(window, search->settings);
    if (pid) {
        search->new_window = window;
        ++search->new_count;
        if (pid == search->launched_pid) {
            search->launched_window = window;
            ++search->launched_count;
        }
    }
    return TRUE;
}

static PROCESS_INFORMATION launch(const Settings *settings)
{
    size_t length = wcslen(settings->command) + 1;
    if (length > 32767)
        fail("launch command exceeds the Windows length limit");
    wchar_t *command = malloc(length * sizeof(*command));
    if (!command)
        fail("out of memory preparing launch");
    wmemcpy(command, settings->command, length);
    STARTUPINFOW startup = {.cb = sizeof(startup)};
    PROCESS_INFORMATION process;
    if (!CreateProcessW(settings->path, command, NULL, NULL, FALSE, 0,
                        NULL, NULL, &startup, &process))
        win_fail("launching the app");
    free(command);
    CloseHandle(process.hThread);
    return process;
}

static Target open_target(const Settings *settings)
{
    Snapshot before = {0};
    if (!EnumWindows(snapshot_window, (LPARAM)&before))
        win_fail("enumerating windows before launch");
    HWND original_foreground = GetForegroundWindow();
    PROCESS_INFORMATION process = launch(settings);
    ULONGLONG started = GetTickCount64(), first_seen = started;
    HWND previous = NULL;
    while (GetTickCount64() - started < settings->timeout_ms) {
        Search search = {.settings = settings, .before = &before,
                         .launched_pid = process.dwProcessId};
        if (!EnumWindows(search_window, (LPARAM)&search))
            win_fail("enumerating windows after launch");
        if (search.launched_count > 1 || (!search.launched_count && search.new_count > 1))
            fail("several new windows match the app; use a unique --match-class or app launch option");
        HWND window = search.launched_count ? search.launched_window : search.new_window;
        if (!window && settings->class_name[0] &&
            GetTickCount64() - started >= settings->startup_ms) {
            HWND foreground = GetForegroundWindow();
            if (foreground != original_foreground && matching_pid(foreground, settings))
                window = foreground;
        }
        if (window != previous) {
            previous = window;
            first_seen = GetTickCount64();
        }
        if (window && GetTickCount64() - first_seen >= settings->startup_ms) {
            Target target = {.window = window, .pid = matching_pid(window, settings)};
            if (target.pid && GetClassNameW(window, target.class_name, ARRAYSIZE(target.class_name))) {
                CloseHandle(process.hProcess);
                free(before.windows);
                return target;
            }
        }
        DWORD exit_code;
        if (!window && GetExitCodeProcess(process.hProcess, &exit_code) &&
            exit_code != STILL_ACTIVE && exit_code != 0)
            fail("app exited before opening a matching window");
        Sleep(50);
    }
    fail("timed out waiting for the app window; check the executable, class, and timeout");
}

static bool target_rect(const Target *target, RECT *rect)
{
    DWORD pid = 0;
    wchar_t class_name[256];
    return GetWindowThreadProcessId(target->window, &pid) && pid == target->pid &&
        IsWindowVisible(target->window) && !IsIconic(target->window) &&
        GetClassNameW(target->window, class_name, ARRAYSIZE(class_name)) &&
        wcscmp(class_name, target->class_name) == 0 && GetWindowRect(target->window, rect);
}

static bool correct_geometry(const Settings *settings, const RECT *rect)
{
    return rect->right - rect->left == settings->width &&
        rect->bottom - rect->top == settings->height &&
        (!settings->absolute || (rect->left == 0 && rect->top == 0));
}

static void focus_window(const Target *target, const Settings *settings)
{
    if (GetForegroundWindow() == target->window)
        return;
    SetForegroundWindow(target->window);
    if (GetForegroundWindow() != target->window)
        fputs("Activate the target window from the taskbar to continue.\n", stderr);
    ULONGLONG started = GetTickCount64();
    do {
        RECT rect;
        if (!target_rect(target, &rect) || !correct_geometry(settings, &rect))
            fail("target window changed while waiting for focus");
        if (GetForegroundWindow() == target->window) {
            /* Let taskbar or other foreground activation input finish first. */
            Sleep(300);
            if (GetForegroundWindow() == target->window)
                return;
        }
        Sleep(50);
    } while (GetTickCount64() - started < settings->timeout_ms);
    fail("Windows did not give the app focus; run from the active desktop");
}

static void prepare_window(const Target *target, const Settings *settings)
{
    HWND window = target->window;
    if (IsIconic(window) || IsZoomed(window)) {
        if (!ShowWindowAsync(window, SW_RESTORE))
            win_fail("restoring the window");
        ULONGLONG started = GetTickCount64();
        while (IsIconic(window) || IsZoomed(window)) {
            if (GetTickCount64() - started >= settings->timeout_ms)
                fail("app did not restore its window");
            Sleep(50);
        }
    }
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS;
    if (!settings->absolute)
        flags |= SWP_NOMOVE;
    ULONGLONG started = GetTickCount64();
    UINT requested_dpi = 0;
    while (GetTickCount64() - started < settings->timeout_ms) {
        RECT rect;
        if (!target_rect(target, &rect))
            fail("target window closed or changed identity");
        UINT dpi = GetDpiForWindow(window);
        if (!dpi)
            win_fail("reading window DPI");
        if (dpi != requested_dpi) {
            if (!SetWindowPos(window, NULL, 0, 0, settings->width, settings->height, flags))
                win_fail("setting window geometry");
            requested_dpi = dpi;
        }
        if (correct_geometry(settings, &rect)) {
            Sleep(300);
            if (target_rect(target, &rect) && correct_geometry(settings, &rect)) {
                focus_window(target, settings);
                return;
            }
        }
        Sleep(50);
    }
    fail("app did not accept the requested window size or position");
}

static bool point_targets_window(HWND window, POINT point)
{
    HWND hit = WindowFromPoint(point);
    return hit && GetAncestor(hit, GA_ROOT) == window;
}

#ifdef CLICK_HELPER_RECORD

static Target recording_target;
static const Settings *recording_settings;
static UINT recording_dpi;
static Point *points;
static size_t point_count, point_capacity;
static bool button_pending;
static POINT pressed_at, pressed_origin;
static const char *recording_error;

static bool recording_geometry(RECT *rect)
{
    return target_rect(&recording_target, rect) && correct_geometry(recording_settings, rect) &&
        GetForegroundWindow() == recording_target.window &&
        GetDpiForWindow(recording_target.window) == recording_dpi;
}

static void abort_recording(const char *message)
{
    recording_error = message;
    PostQuitMessage(1);
}

static LRESULT CALLBACK observe_mouse(int code, WPARAM event, LPARAM data)
{
    if (code == HC_ACTION && (event == WM_LBUTTONDOWN || event == WM_LBUTTONUP)) {
        const MSLLHOOKSTRUCT *input = (const MSLLHOOKSTRUCT *)data;
        RECT rect;
        if (!recording_geometry(&rect)) {
            abort_recording("target lost focus, closed, or changed size, placement, or DPI");
        } else if (event == WM_LBUTTONDOWN) {
            pressed_at = input->pt;
            pressed_origin = (POINT){rect.left, rect.top};
            button_pending = PtInRect(&rect, pressed_at) &&
                point_targets_window(recording_target.window, pressed_at);
        } else if (button_pending) {
            button_pending = false;
            if (pressed_at.x != input->pt.x || pressed_at.y != input->pt.y ||
                pressed_origin.x != rect.left || pressed_origin.y != rect.top ||
                !PtInRect(&rect, pressed_at) || !point_targets_window(recording_target.window, pressed_at)) {
                abort_recording("pointer or window moved during a click; record stationary left-clicks");
            } else {
                if (point_count == point_capacity) {
                    void *larger = grow(points, &point_capacity, sizeof(*points));
                    if (!larger) {
                        abort_recording("out of memory recording clicks");
                        return CallNextHookEx(NULL, code, event, data);
                    }
                    points = larger;
                }
                points[point_count++] = (Point){pressed_at.x - rect.left, pressed_at.y - rect.top};
            }
        }
    }
    return CallNextHookEx(NULL, code, event, data);
}

static LRESULT CALLBACK observe_escape(int code, WPARAM event, LPARAM data)
{
    if (code == HC_ACTION && ((const KBDLLHOOKSTRUCT *)data)->vkCode == VK_ESCAPE) {
        if (event == WM_KEYUP || event == WM_SYSKEYUP)
            PostQuitMessage(0);
        return 1;
    }
    return CallNextHookEx(NULL, code, event, data);
}

static void record_clicks(const Target *target, const Settings *settings)
{
    recording_target = *target;
    recording_settings = settings;
    recording_dpi = GetDpiForWindow(target->window);
    if (!recording_dpi)
        win_fail("reading recording DPI");
    UINT_PTR timer = SetTimer(NULL, 0, 50, NULL);
    if (!timer)
        win_fail("setting recording timer");
    HMODULE module = GetModuleHandleW(NULL);
    HHOOK mouse = SetWindowsHookExW(WH_MOUSE_LL, observe_mouse, module, 0);
    if (!mouse)
        win_fail("installing mouse hook");
    HHOOK keyboard = SetWindowsHookExW(WH_KEYBOARD_LL, observe_escape, module, 0);
    if (!keyboard)
        win_fail("installing Escape hook");
    fputs("Recording. Left-click inside the app. Esc saves. Ctrl+C cancels.\n", stderr);
    MSG message;
    BOOL result;
    while ((result = GetMessageW(&message, NULL, 0, 0)) > 0) {
        RECT rect;
        if (message.message == WM_TIMER && !recording_geometry(&rect))
            abort_recording("target lost focus, closed, or changed size, placement, or DPI");
    }
    UnhookWindowsHookEx(keyboard);
    UnhookWindowsHookEx(mouse);
    KillTimer(NULL, timer);
    if (result == -1)
        win_fail("reading input messages");
    if (recording_error)
        fail(recording_error);
    if (button_pending || !point_count)
        fail("no completed recording to save");
    fputs("{\"class\":\"", stdout);
    for (const wchar_t *c = target->class_name; *c; ++c)
        printf("\\u%04x", (unsigned int)*c);
    printf("\",\"dpi\":%u,\"points\":[", recording_dpi);
    for (size_t i = 0; i < point_count; ++i)
        printf("%s[%d,%d]", i ? "," : "", points[i].x, points[i].y);
    puts("]}");
    free(points);
}

#else

static void play_clicks(const Target *target, const Settings *settings)
{
    for (size_t i = 0; i < ARRAYSIZE(CLICKS); ++i) {
        RECT rect, settled;
        if (!target_rect(target, &rect))
            fail("target window closed or changed identity");
        if (!correct_geometry(settings, &rect) || GetDpiForWindow(target->window) != WINDOW_DPI)
            fail("window size, placement, or DPI differs from the recording");
        if (GetForegroundWindow() != target->window)
            fail("target lost focus; playback stopped");
        Point click = CLICKS[i];
        POINT screen = {rect.left + click.x, rect.top + click.y};
        if (!PtInRect(&rect, screen) || !point_targets_window(target->window, screen))
            fail("recorded point is outside or obscured; playback stopped");
        if (!SetCursorPos(screen.x, screen.y))
            win_fail("moving the cursor");
        Sleep(100);
        POINT cursor;
        if (!GetCursorPos(&cursor) || cursor.x != screen.x || cursor.y != screen.y)
            fail("cursor moved before the click; playback stopped");
        if (!target_rect(target, &settled) || !EqualRect(&rect, &settled) ||
            GetDpiForWindow(target->window) != WINDOW_DPI ||
            GetForegroundWindow() != target->window || !point_targets_window(target->window, screen))
            fail("target changed before the click; playback stopped");
        if (GetAsyncKeyState(VK_LBUTTON) & 0x8000)
            fail("left mouse button is held; playback stopped");
        INPUT input[2] = {
            {.type = INPUT_MOUSE, .mi = {.dwFlags = MOUSEEVENTF_LEFTDOWN}},
            {.type = INPUT_MOUSE, .mi = {.dwFlags = MOUSEEVENTF_LEFTUP}}
        };
        UINT sent = SendInput(ARRAYSIZE(input), input, sizeof(INPUT));
        if (sent != ARRAYSIZE(input)) {
            if (sent == 1)
                SendInput(1, &input[1], sizeof(INPUT));
            fail("Windows did not inject the full click; check the app's elevation and desktop");
        }
        Sleep(CLICK_DELAY_MS);
    }
}

#endif

int wmain(int argc, wchar_t **argv)
{
#ifdef CLICK_HELPER_RECORD
    /* tools/windows.py validates these arguments and encodes the app command line. */
    if (argc != 9)
        fail("run the recorder through tools/windows.py record");
    Settings settings = {argv[1], argv[2], argv[3], _wtoi(argv[4]), _wtoi(argv[5]),
                         wcstoul(argv[6], NULL, 10), wcstoul(argv[7], NULL, 10),
                         argv[8][0] == L'1'};
#else
    (void)argv;
    if (argc != 1)
        fail("this executable takes no arguments");
    Settings settings = {APP_PATH, APP_COMMAND, APP_CLASS, WINDOW_WIDTH, WINDOW_HEIGHT,
                         STARTUP_DELAY_MS, LAUNCH_TIMEOUT_MS, POSITION_ABSOLUTE};
#endif
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        win_fail("enabling per-monitor DPI awareness");
    Target target = open_target(&settings);
    prepare_window(&target, &settings);
#ifdef CLICK_HELPER_RECORD
    record_clicks(&target, &settings);
#else
    play_clicks(&target, &settings);
#endif
    return EXIT_SUCCESS;
}
