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
typedef struct {
    RECT bounds;
    UINT dpi;
    wchar_t class_name[256];
} Bubble;

#ifndef CLICK_HELPER_RECORD
#include "config.h"
#endif

#ifndef CLICK_ON_POPOUT
#define CLICK_ON_POPOUT(index) false
#endif
#ifndef CLICK_BUBBLE
#define CLICK_BUBBLE(index) NULL
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
    /* Keep the launched app from holding the recorder's redirected output open. */
    if (!CreateProcessW(settings->path, command, NULL, NULL, FALSE, DETACHED_PROCESS,
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

static HWND window_at(POINT point)
{
    HWND hit = WindowFromPoint(point);
    return hit ? GetAncestor(hit, GA_ROOT) : NULL;
}

static bool owned_window(const Target *owner, HWND window)
{
    DWORD pid = 0;
    return owner->window && window && window != owner->window && IsWindowVisible(window) &&
        GetWindowThreadProcessId(window, &pid) && pid == owner->pid &&
        GetAncestor(window, GA_ROOTOWNER) == owner->window;
}

static bool has_surface_focus(const Target *owner, HWND window)
{
    HWND foreground = GetForegroundWindow();
    return window && (foreground == window ||
        (owner && foreground == owner->window && owned_window(owner, window)));
}

static bool describe_bubble(const Target *owner, HWND window, const RECT *owner_rect,
                            RECT *screen_bounds, Bubble *bubble)
{
    if (!owned_window(owner, window) || !GetWindowRect(window, screen_bounds) ||
        !GetClassNameW(window, bubble->class_name, ARRAYSIZE(bubble->class_name)))
        return false;
    bubble->dpi = GetDpiForWindow(window);
    bubble->bounds = *screen_bounds;
    OffsetRect(&bubble->bounds, -owner_rect->left, -owner_rect->top);
    return bubble->dpi != 0;
}

#if defined(CLICK_HELPER_RECORD) || defined(POPOUT_AFTER_CLICK)
typedef struct {
    const Target *main;
    const Snapshot *before;
    HWND window;
    size_t count;
} PopoutSearch;

static bool new_popout(HWND window, const Target *main, const Snapshot *before)
{
    DWORD pid = 0;
    wchar_t class_name[256];
    return window && window != main->window && !was_present(before, window) &&
        GetWindowThreadProcessId(window, &pid) && pid == main->pid && normal_window(window) &&
        GetClassNameW(window, class_name, ARRAYSIZE(class_name)) &&
        wcscmp(class_name, main->class_name) == 0;
}

static BOOL CALLBACK search_popout(HWND window, LPARAM parameter)
{
    PopoutSearch *search = (PopoutSearch *)parameter;
    if (new_popout(window, search->main, search->before)) {
        search->window = window;
        ++search->count;
    }
    return TRUE;
}

static PopoutSearch find_popout(const Target *main, const Snapshot *before)
{
    PopoutSearch search = {.main = main, .before = before};
    if (!EnumWindows(search_popout, (LPARAM)&search))
        win_fail("enumerating popout windows");
    return search;
}

static bool popout_rect(const Target *popout, int width, int height, UINT dpi, RECT *rect)
{
    return target_rect(popout, rect) && rect->right - rect->left == width &&
        rect->bottom - rect->top == height && GetDpiForWindow(popout->window) == dpi;
}
#endif

#ifdef CLICK_HELPER_RECORD

typedef struct { Point point; bool popout; Bubble bubble; } RecordedClick;

static Target recording_target;
static Target recording_popout;
static const Settings *recording_settings;
static UINT recording_dpi;
static RecordedClick *points;
static size_t point_count, point_capacity;
static bool follow_popout, snapshot_taken;
static Snapshot before_popout;
static size_t popout_after_click;
static wchar_t popout_title[512];
static int popout_width, popout_height;
static UINT popout_dpi;
static HWND pressed_window;
static POINT pressed_at;
static RECT pressed_bounds;
static const char *recording_error;

static void abort_recording(const char *message)
{
    recording_error = message;
    PostQuitMessage(1);
}

static const Target *recording_owner(HWND window)
{
    if (window == recording_target.window || owned_window(&recording_target, window))
        return &recording_target;
    if (recording_popout.window && (window == recording_popout.window || owned_window(&recording_popout, window)))
        return &recording_popout;
    return NULL;
}

static bool has_recording_focus(HWND foreground)
{
    return recording_owner(foreground) || (!recording_popout.window && snapshot_taken &&
        new_popout(foreground, &recording_target, &before_popout));
}

static bool check_recording(RECT *rect)
{
    if (!target_rect(&recording_target, rect)) {
        abort_recording("the original window closed, was minimized, or changed identity");
    } else if (!correct_geometry(recording_settings, rect) ||
               GetDpiForWindow(recording_target.window) != recording_dpi) {
        abort_recording("the original window changed size, placement, or DPI");
    } else {
        HWND foreground = GetForegroundWindow();
        if (!foreground || has_recording_focus(foreground))
            return true;
        /* Closing a separate Chrome window can briefly activate another app. */
        if (recording_popout.window && !IsWindowVisible(recording_popout.window)) {
            pressed_window = NULL;
            return false;
        }
        abort_recording(follow_popout ? "a window outside the recording took focus" :
                        "another window took focus; use --follow-popout to record one website popout");
    }
    return false;
}

static bool bind_recording_popout(HWND window)
{
    PopoutSearch search = find_popout(&recording_target, &before_popout);
    if (search.count != 1 || search.window != window || window != GetForegroundWindow()) {
        abort_recording("could not identify one new foreground popout");
        return false;
    }
    int title_length = GetWindowTextW(window, popout_title, ARRAYSIZE(popout_title));
    RECT rect;
    recording_popout = recording_target;
    recording_popout.window = window;
    if (!target_rect(&recording_popout, &rect) || !title_length || title_length >= (int)ARRAYSIZE(popout_title) - 1) {
        abort_recording("the popout needs a stable window title and geometry");
        return false;
    }
    popout_width = rect.right - rect.left;
    popout_height = rect.bottom - rect.top;
    popout_dpi = GetDpiForWindow(window);
    if (!popout_dpi) {
        abort_recording("could not read the popout's DPI");
        return false;
    }
    fputs("Recording clicks in the popout.\n", stderr);
    return true;
}

static bool recorded_surface(const Target *owner, HWND window, RECT *owner_rect,
                             RECT *bounds, Bubble *bubble)
{
    if (owner == &recording_popout &&
        !popout_rect(owner, popout_width, popout_height, popout_dpi, owner_rect)) {
        abort_recording("the popout changed size, DPI, or identity");
        return false;
    }
    *bounds = *owner_rect;
    if (window != owner->window && !describe_bubble(owner, window, owner_rect, bounds, bubble)) {
        abort_recording("the attached popup closed or changed identity");
        return false;
    }
    return true;
}

static LRESULT CALLBACK observe_mouse(int code, WPARAM event, LPARAM data)
{
    if (code == HC_ACTION && (event == WM_LBUTTONDOWN || event == WM_LBUTTONUP)) {
        const MSLLHOOKSTRUCT *input = (const MSLLHOOKSTRUCT *)data;
        RECT rect;
        if (!check_recording(&rect))
            return CallNextHookEx(NULL, code, event, data);
        if (event == WM_LBUTTONDOWN) {
            pressed_window = NULL;
            HWND window = window_at(input->pt);
            const Target *owner = recording_owner(window);
            /* A click that only brings a window forward is not part of the sequence. */
            if (!has_surface_focus(owner, window))
                return CallNextHookEx(NULL, code, event, data);
            if (owner == &recording_target && follow_popout && !recording_popout.window) {
                before_popout.count = 0;
                if (!EnumWindows(snapshot_window, (LPARAM)&before_popout))
                    win_fail("enumerating windows before a recorded click");
                snapshot_taken = true;
                popout_after_click = point_count;
            } else if (!owner && !recording_popout.window && snapshot_taken && point_count > popout_after_click &&
                       new_popout(window, &recording_target, &before_popout)) {
                if (!bind_recording_popout(window))
                    return CallNextHookEx(NULL, code, event, data);
                owner = &recording_popout;
            }
            Bubble bubble = {0};
            if (owner && recorded_surface(owner, window, &rect, &pressed_bounds, &bubble) &&
                PtInRect(&pressed_bounds, input->pt)) {
                pressed_at = input->pt;
                pressed_window = window;
            }
        } else if (pressed_window) {
            HWND window = pressed_window;
            const Target *owner = recording_owner(window);
            RECT bounds;
            Bubble bubble = {0};
            pressed_window = NULL;
            if (!owner) {
                abort_recording("the window under the recorded click closed or changed identity");
            } else if (!recorded_surface(owner, window, &rect, &bounds, &bubble)) {
                return CallNextHookEx(NULL, code, event, data);
            } else if (pressed_at.x != input->pt.x || pressed_at.y != input->pt.y ||
                !EqualRect(&pressed_bounds, &bounds) || window_at(pressed_at) != window) {
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
                points[point_count++] = (RecordedClick){
                    {pressed_at.x - rect.left, pressed_at.y - rect.top}, owner == &recording_popout, bubble
                };
            }
        }
    }
    return CallNextHookEx(NULL, code, event, data);
}

static LRESULT CALLBACK observe_escape(int code, WPARAM event, LPARAM data)
{
    if (code == HC_ACTION && ((const KBDLLHOOKSTRUCT *)data)->vkCode == VK_ESCAPE &&
        has_recording_focus(GetForegroundWindow())) {
        if (event == WM_KEYUP || event == WM_SYSKEYUP)
            PostQuitMessage(0);
        return 1;
    }
    return CallNextHookEx(NULL, code, event, data);
}

static void json_string(const wchar_t *value)
{
    putchar('"');
    for (; *value; ++value)
        printf("\\u%04x", (unsigned int)*value);
    putchar('"');
}

static void record_clicks(const Target *target, const Settings *settings, bool follow)
{
    recording_target = *target;
    recording_settings = settings;
    follow_popout = follow;
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
        if (message.message == WM_TIMER)
            check_recording(&rect);
    }
    UnhookWindowsHookEx(keyboard);
    UnhookWindowsHookEx(mouse);
    KillTimer(NULL, timer);
    if (result == -1)
        win_fail("reading input messages");
    if (recording_error)
        fail(recording_error);
    if (pressed_window || !point_count)
        fail("no completed recording to save");
    fputs("{\"class\":", stdout);
    json_string(target->class_name);
    printf(",\"dpi\":%u", recording_dpi);
    if (recording_popout.window) {
        printf(",\"popout\":{\"after\":%zu,\"width\":%d,\"height\":%d,\"dpi\":%u,\"title\":",
               popout_after_click, popout_width, popout_height, popout_dpi);
        json_string(popout_title);
        putchar('}');
    }
    fputs(",\"points\":[", stdout);
    for (size_t i = 0; i < point_count; ++i)
        printf("%s[%d,%d,%d]", i ? "," : "", points[i].point.x, points[i].point.y, points[i].popout);
    fputs("],\"bubbles\":[", stdout);
    for (size_t i = 0; i < point_count; ++i) {
        const Bubble *bubble = &points[i].bubble;
        if (i)
            putchar(',');
        if (!bubble->dpi) {
            fputs("null", stdout);
            continue;
        }
        printf("{\"bounds\":[%ld,%ld,%ld,%ld],\"dpi\":%u,\"class\":", bubble->bounds.left,
               bubble->bounds.top, bubble->bounds.right, bubble->bounds.bottom, bubble->dpi);
        json_string(bubble->class_name);
        putchar('}');
    }
    puts("]}");
    free(points);
    free(before_popout.windows);
}

#else

#ifdef POPOUT_AFTER_CLICK
static Target wait_for_popout(const Target *main, const Snapshot *before, const Settings *settings)
{
    fputs("Waiting for the recorded popout.\n", stderr);
    ULONGLONG started = GetTickCount64();
    while (GetTickCount64() - started < settings->timeout_ms) {
        RECT rect;
        if (!target_rect(main, &rect) || !correct_geometry(settings, &rect) ||
            GetDpiForWindow(main->window) != WINDOW_DPI)
            fail("the original window changed while waiting for the popout");
        PopoutSearch search = find_popout(main, before);
        if (search.count > 1)
            fail("several new windows could be the popout; playback stopped");
        if (search.count == 1 && GetForegroundWindow() == search.window) {
            wchar_t title[512];
            Target popout = *main;
            popout.window = search.window;
            if (GetWindowTextW(search.window, title, ARRAYSIZE(title)) && wcscmp(title, POPOUT_TITLE) == 0 &&
                popout_rect(&popout, POPOUT_WIDTH, POPOUT_HEIGHT, POPOUT_DPI, &rect))
                return popout;
        }
        Sleep(50);
    }
    fail("the recorded popout did not open in the foreground with its expected title, size, and DPI");
}
#endif

static HWND matching_bubble(const Target *owner, const RECT *owner_rect, POINT point, const Bubble *expected)
{
    HWND window = window_at(point);
    RECT screen_bounds;
    Bubble actual;
    if (!describe_bubble(owner, window, owner_rect, &screen_bounds, &actual) ||
        !PtInRect(&screen_bounds, point) || !has_surface_focus(owner, window) ||
        actual.dpi != expected->dpi || !EqualRect(&actual.bounds, &expected->bounds) ||
        wcscmp(actual.class_name, expected->class_name) != 0)
        return NULL;
    return window;
}

static HWND wait_for_bubble(const Target *owner, const RECT *rect, POINT point, const Bubble *expected,
                            UINT dpi, DWORD timeout_ms)
{
    ULONGLONG started = GetTickCount64();
    do {
        RECT current;
        HWND foreground = GetForegroundWindow();
        if (foreground && foreground != owner->window && !owned_window(owner, foreground))
            fail("target lost focus while waiting for the attached popup; playback stopped");
        if (!target_rect(owner, &current) || !EqualRect(rect, &current) || GetDpiForWindow(owner->window) != dpi)
            fail("target changed while waiting for the attached popup; playback stopped");
        HWND window = matching_bubble(owner, rect, point, expected);
        if (window)
            return window;
        Sleep(50);
    } while (GetTickCount64() - started < timeout_ms);
    fail("the attached popup did not match the recording; playback stopped");
}

static void play_clicks(const Target *target, const Settings *settings)
{
    Target popout = {0};
#ifdef POPOUT_AFTER_CLICK
    Snapshot before_popout = {0};
#endif
    for (size_t i = 0; i < ARRAYSIZE(CLICKS); ++i) {
        bool on_popout = CLICK_ON_POPOUT(i);
        const Target *surface = on_popout ? &popout : target;
        UINT dpi = WINDOW_DPI;
        RECT main_rect, rect, settled;
        if (!target_rect(target, &main_rect))
            fail("target window closed or changed identity");
        if (!correct_geometry(settings, &main_rect) || GetDpiForWindow(target->window) != WINDOW_DPI)
            fail("window size, placement, or DPI differs from the recording");
        rect = main_rect;
#ifdef POPOUT_AFTER_CLICK
        if (on_popout) {
            if (!popout.window)
                popout = wait_for_popout(target, &before_popout, settings);
            dpi = POPOUT_DPI;
            if (!popout_rect(&popout, POPOUT_WIDTH, POPOUT_HEIGHT, dpi, &rect))
                fail("the popout closed or changed size, DPI, or identity; playback stopped");
        }
#endif
        Point click = CLICKS[i];
        POINT screen = {rect.left + click.x, rect.top + click.y};
        const Bubble *bubble = CLICK_BUBBLE(i);
        HWND click_window = surface->window;
        if (bubble && bubble->dpi) {
            click_window = wait_for_bubble(surface, &rect, screen, bubble, dpi, settings->timeout_ms);
        } else {
            bubble = NULL;
            if (GetForegroundWindow() != click_window)
                fail("target lost focus; playback stopped");
            if (!PtInRect(&rect, screen) || window_at(screen) != click_window)
                fail("recorded point is outside or obscured; playback stopped");
        }
        if (!SetCursorPos(screen.x, screen.y))
            win_fail("moving the cursor");
        Sleep(100);
#ifdef POPOUT_AFTER_CLICK
        if (i == POPOUT_AFTER_CLICK && !EnumWindows(snapshot_window, (LPARAM)&before_popout))
            win_fail("enumerating windows before the popout opens");
#endif
        POINT cursor;
        if (!GetCursorPos(&cursor) || cursor.x != screen.x || cursor.y != screen.y)
            fail("cursor moved before the click; playback stopped");
        if (!target_rect(surface, &settled) || !EqualRect(&rect, &settled) ||
            GetDpiForWindow(surface->window) != dpi || !has_surface_focus(surface, click_window) ||
            window_at(screen) != click_window ||
            (bubble && matching_bubble(surface, &rect, screen, bubble) != click_window))
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
#ifdef POPOUT_AFTER_CLICK
    free(before_popout.windows);
#endif
}

#endif

int wmain(int argc, wchar_t **argv)
{
#ifdef CLICK_HELPER_RECORD
    /* tools/windows.py validates these arguments and encodes the app command line. */
    if (argc != 10)
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
    record_clicks(&target, &settings, argv[9][0] == L'1');
#else
    play_clicks(&target, &settings);
#endif
    return EXIT_SUCCESS;
}
