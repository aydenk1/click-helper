"""Opt-in Windows desktop smoke test. Takes control of the pointer in a test Chrome window."""

import argparse
from contextlib import contextmanager
import ctypes as ct
from ctypes import wintypes as win
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import uuid


ROOT = Path(__file__).resolve().parents[1]
CHROME = Path(r"C:\Program Files\Google\Chrome\Application\chrome.exe")
WAIT = 60


class MouseInput(ct.Structure):
    _fields_ = [("x", win.LONG), ("y", win.LONG), ("data", win.DWORD),
                ("flags", win.DWORD), ("time", win.DWORD), ("extra", ct.c_size_t)]


class KeyboardInput(ct.Structure):
    _fields_ = [("key", win.WORD), ("scan", win.WORD), ("flags", win.DWORD),
                ("time", win.DWORD), ("extra", ct.c_size_t)]


class InputData(ct.Union):
    _fields_ = [("mouse", MouseInput), ("keyboard", KeyboardInput)]


class Input(ct.Structure):
    _fields_ = [("type", win.DWORD), ("data", InputData)]


class Desktop:
    def __init__(self):
        user = ct.WinDLL("user32", use_last_error=True)
        kernel = ct.WinDLL("kernel32", use_last_error=True)
        self.enumerator = ct.WINFUNCTYPE(win.BOOL, win.HWND, win.LPARAM)
        signatures = [
            (user, "SetProcessDpiAwarenessContext", win.BOOL, win.HANDLE),
            (user, "EnumWindows", win.BOOL, self.enumerator, win.LPARAM),
            (user, "GetWindowTextW", ct.c_int, win.HWND, win.LPWSTR, ct.c_int),
            (user, "GetWindowRect", win.BOOL, win.HWND, ct.POINTER(win.RECT)),
            (user, "GetForegroundWindow", win.HWND),
            (user, "SetCursorPos", win.BOOL, ct.c_int, ct.c_int),
            (user, "WindowFromPoint", win.HWND, win.POINT),
            (user, "GetAncestor", win.HWND, win.HWND, win.UINT),
            (user, "SendInput", win.UINT, win.UINT, ct.POINTER(Input), ct.c_int),
            (user, "SetWindowPos", win.BOOL, win.HWND, win.HWND,
             ct.c_int, ct.c_int, ct.c_int, ct.c_int, win.UINT),
            (user, "SendMessageTimeoutW", win.LPARAM, win.HWND, win.UINT, win.WPARAM, win.LPARAM,
             win.UINT, win.UINT, ct.POINTER(ct.c_size_t)),
            (user, "GetWindowThreadProcessId", win.DWORD, win.HWND, ct.POINTER(win.DWORD)),
            (user, "PostMessageW", win.BOOL, win.HWND, win.UINT, win.WPARAM, win.LPARAM),
            (kernel, "OpenProcess", win.HANDLE, win.DWORD, win.BOOL, win.DWORD),
            (kernel, "WaitForSingleObject", win.DWORD, win.HANDLE, win.DWORD),
            (kernel, "CloseHandle", win.BOOL, win.HANDLE),
        ]
        for library, name, result, *arguments in signatures:
            function = getattr(library, name)
            function.argtypes, function.restype = arguments, result
        self.user, self.kernel = user, kernel
        self.check(user.SetProcessDpiAwarenessContext(-4), "enable per-monitor DPI awareness")

    @staticmethod
    def check(ok, action):
        if not ok:
            raise RuntimeError(f"Could not {action} (Windows error {ct.get_last_error()})")

    def title(self, window):
        buffer = ct.create_unicode_buffer(512)
        self.user.GetWindowTextW(window, buffer, len(buffer))
        return buffer.value

    def window(self, title):
        matches = []

        @self.enumerator
        def collect(window, _):
            if self.title(window).startswith(title):
                matches.append(window)
            return True

        self.check(self.user.EnumWindows(collect, 0), "enumerate test windows")
        require(len(matches) <= 1, "More than one window has the test page title")
        return matches[0] if matches else None

    def wait_title(self, title, process=None):
        end = time.monotonic() + WAIT
        while time.monotonic() < end:
            if window := self.window(title):
                return window
            if process is not None and process.poll() is not None:
                raise RuntimeError(f"Process exited with {process.returncode} before {title}")
            time.sleep(0.03)
        raise RuntimeError(f"Timed out waiting for {title}")

    def rect(self, window):
        rect = win.RECT()
        self.check(self.user.GetWindowRect(window, ct.byref(rect)), "read test window geometry")
        return rect

    def point_at(self, window, x, y):
        hit = self.user.WindowFromPoint(win.POINT(x, y))
        require(hit and self.user.GetAncestor(hit, 2) == window, "Chrome test window must be visible")
        self.check(self.user.SetCursorPos(x, y), "move the pointer into the test page")

    def left_click(self):
        self.send(Input(0, InputData(mouse=MouseInput(flags=0x0002))),
                  Input(0, InputData(mouse=MouseInput(flags=0x0004))))

    def click(self, window, second):
        require(self.user.GetForegroundWindow() == window, "Chrome test window lost focus")
        rect = self.rect(window)
        x = rect.left + (rect.right - rect.left) * (3 if second else 1) // 4
        y = rect.top + (rect.bottom - rect.top) * 2 // 3
        self.point_at(window, x, y)
        self.left_click()

    def activate_fixture(self, window):
        # This is a disposable test window. Click its verified title bar to
        # exercise the same focus handoff as a user at an interactive desktop.
        flags = 0x0013  # no activation, movement, or resize
        self.check(self.user.SetWindowPos(window, -1, 0, 0, 0, 0, flags), "show the test title bar")
        try:
            rect = self.rect(window)
            x = rect.left + (rect.right - rect.left) // 3
            for offset in (16, 24, 32, 40, 48):
                y = rect.top + offset
                hit = ct.c_size_t()
                coordinates = ((y & 0xffff) << 16) | (x & 0xffff)
                self.check(self.user.SendMessageTimeoutW(window, 0x0084, 0, coordinates, 2, 500,
                                                         ct.byref(hit)), "find the test title bar")
                if hit.value == 2:  # HTCAPTION
                    self.point_at(window, x, y)
                    if self.user.GetForegroundWindow() != window:
                        self.left_click()
                    break
            else:
                raise RuntimeError("Chrome test title bar could not be located")
        finally:
            self.check(self.user.SetWindowPos(window, -2, 0, 0, 0, 0, flags), "restore test window stacking")

    def wait_focus(self, prefix, process, focus_requested):
        window = self.wait_title(prefix + "ready", process)
        end = time.monotonic() + WAIT
        activated = False
        while time.monotonic() < end and process.poll() is None:
            if self.user.GetForegroundWindow() == window:
                return window
            if focus_requested.is_set() and not activated:
                self.activate_fixture(window)
                activated = True
            time.sleep(0.03)
        raise RuntimeError("The Chrome test window did not get foreground focus")

    def escape(self, window):
        require(self.user.GetForegroundWindow() == window, "Chrome test window lost focus")
        self.send(Input(1, InputData(keyboard=KeyboardInput(key=0x1B))),
                  Input(1, InputData(keyboard=KeyboardInput(key=0x1B, flags=0x0002))))

    def send(self, *events):
        inputs = (Input * len(events))(*events)
        self.check(self.user.SendInput(len(events), inputs, ct.sizeof(Input)) == len(events),
                   "send test input")

    def change(self, window, *, move):
        rect = self.rect(window)
        x = rect.left + (80 if move else 0)
        width = rect.right - rect.left - (0 if move else 80)
        height = rect.bottom - rect.top
        # Preserve focus and z-order while changing geometry between recorded clicks.
        self.check(self.user.SetWindowPos(window, None, x, rect.top, width, height, 0x0014),
                   "change test window geometry")
        updated = self.rect(window)
        require(updated.left == x and updated.right - updated.left == width,
                "Chrome did not accept the geometry change")

    def close(self, prefix):
        window = self.window(prefix)
        if window is None:
            return
        pid = win.DWORD()
        self.check(self.user.GetWindowThreadProcessId(window, ct.byref(pid)), "identify test Chrome")
        process = self.kernel.OpenProcess(0x00100000, False, pid.value)
        self.check(process, "open test Chrome for shutdown verification")
        try:
            self.check(self.user.PostMessageW(window, 0x0010, 0, 0), "close the test Chrome window")
            require(self.kernel.WaitForSingleObject(process, WAIT * 1000) == 0,
                    "Isolated Chrome did not shut down after closing its test window")
        finally:
            self.kernel.CloseHandle(process)


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


@contextmanager
def running(command, cwd):
    with subprocess.Popen(command, cwd=cwd, stdout=subprocess.DEVNULL,
                          stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace") as process:
        diagnostics = []
        ready = threading.Event()
        focus_requested = threading.Event()

        def collect():
            for line in process.stderr:
                diagnostics.append(line)
                if line.startswith("Activate the target window"):
                    focus_requested.set()
                if line.startswith("Recording."):
                    ready.set()
            ready.set()

        reader = threading.Thread(target=collect, daemon=True)
        reader.start()
        try:
            yield process, diagnostics, ready, focus_requested
        except Exception as error:
            details = "".join(diagnostics).strip()
            if details:
                raise RuntimeError(f"{error}\n{details}") from error
            raise
        finally:
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=5)
            reader.join(timeout=5)


def page_html(prefix):
    return f"""<!doctype html>
<meta charset=utf-8>
<title>{prefix}ready</title>
<style>
body {{ position: fixed; inset: 0; margin: 0; display: flex; }}
button {{ width: 50%; font: 2rem sans-serif; }}
</style>
<button id=first>First</button><button id=second>Second</button>
<script>
let clicks = '';
const positions = [];
for (const [id, value] of [['first', '1'], ['second', '2']]) {{
  document.getElementById(id).onclick = (event) => {{
    clicks += value;
    positions.push(`${{event.clientX}},${{event.clientY}}`);
    const state = clicks === '1' ? 'one' : clicks === '12' ? 'pass' : 'wrong';
    document.title = '{prefix}' + state + '-' + positions.join(';');
  }};
}}
</script>
"""


def record(desktop, command, workspace, prefix, *, empty=False):
    try:
        with running(command, workspace) as (process, diagnostics, ready, focus_requested):
            window = desktop.wait_focus(prefix, process, focus_requested)
            require(ready.wait(WAIT) and any(line.startswith("Recording.") for line in diagnostics),
                    "Recorder did not arm")
            if not empty:
                desktop.click(window, second=False)
                desktop.wait_title(prefix + "one", process)
                desktop.click(window, second=True)
                desktop.wait_title(prefix + "pass", process)
            desktop.escape(window)
            code = process.wait(timeout=WAIT)
            require((code != 0) if empty else (code == 0), f"Unexpected recording exit code {code}")
            if not empty:
                require(desktop.title(window).startswith(prefix + "pass"),
                        "The recorded clicks did not reach Chrome exactly once")
                return desktop.title(window)
    finally:
        desktop.close(prefix)


def playback(desktop, exe, workspace, prefix, *, mode, expected_title):
    try:
        with running([str(exe)], workspace) as (process, diagnostics, _, focus_requested):
            desktop.wait_focus(prefix, process, focus_requested)
            window = desktop.wait_title(prefix + "one", process)
            if mode == "absolute":
                rect = desktop.rect(window)
                require((rect.left, rect.top) == (0, 0), "Absolute playback did not use desktop origin")
            else:
                desktop.change(window, move=mode == "relative")
            if mode == "resize":
                require(process.wait(timeout=WAIT) != 0, "Player accepted an unexpected resize")
                require(desktop.title(window).startswith(prefix + "one"), "Player clicked after the resize")
                require(any("size, placement, or DPI" in line for line in diagnostics),
                        "Player failed for a reason other than changed geometry")
            else:
                desktop.wait_title(prefix + "pass", process)
                require(process.wait(timeout=WAIT) == 0, "Player did not exit successfully")
                require(desktop.title(window) == expected_title,
                        "The replayed clicks did not match the recorded page coordinates")
    finally:
        desktop.close(prefix)


def test_imports(exe, compiler):
    objdump = compiler.with_name("llvm-objdump.exe")
    result = subprocess.check_output([str(objdump), "-p", str(exe)], text=True)
    imports = {name.lower() for name in re.findall(r"DLL Name:\s+(\S+)", result)}
    require({"kernel32.dll", "user32.dll"} <= imports, "Could not inspect the executable's Windows imports")
    system_dlls = {"dwmapi.dll", "kernel32.dll", "user32.dll", "ucrtbase.dll", "msvcrt.dll"}
    unexpected = {name for name in imports if name not in system_dlls and not name.startswith("api-ms-win-")}
    require(not unexpected, f"Unexpected runtime DLLs: {sorted(unexpected)}")
    print(f"PE imports verified ({len(imports)} Windows system DLLs)", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="x86_64-w64-mingw32-clang.exe")
    parser.add_argument("--chrome", type=Path, default=CHROME)
    args = parser.parse_args()
    if sys.platform != "win32":
        parser.error("run this smoke test from an active Windows desktop")
    compiler = shutil.which(args.cc)
    if not compiler or not args.chrome.is_file():
        parser.error("the test requires x64 LLVM-MinGW and an installed Chrome executable")
    compiler = Path(compiler).resolve()
    desktop = Desktop()
    with tempfile.TemporaryDirectory(prefix="click-helper-smoke-") as temporary:
        workspace = Path(temporary)
        for relative in ("src/helper_windows.c", "tools/windows.py", "tools/record.py"):
            target = workspace / relative
            target.parent.mkdir(exist_ok=True)
            shutil.copyfile(ROOT / relative, target)
        command = [sys.executable, str(workspace / "tools/windows.py"), "--cc", str(compiler)]
        page = workspace / "click test 雪.html"
        prefix = f"click-helper-{uuid.uuid4().hex}-"
        page.write_text(page_html(prefix), encoding="utf-8")
        profile = workspace / "isolated Chrome 雪"
        chrome = [str(args.chrome), f"--user-data-dir={profile}", "--no-first-run",
                  "--no-default-browser-check", "--disable-background-mode",
                  "--disable-background-networking", f"--app={page.as_uri()}"]
        exe = workspace / "build/windows/click-helper.exe"
        header = workspace / "generated/windows/config.h"
        for mode in ("relative", "absolute"):
            recorder = [*command, "record", "--position", mode, "--click-delay", "1.5",
                        "--timeout", "45", "--", *chrome]
            expected_title = record(desktop, recorder, workspace, prefix)
            print(f"{mode}: recorded two ordered clicks through Chrome", flush=True)
            if mode == "relative":
                saved = header.read_bytes()
                record(desktop, recorder, workspace, prefix, empty=True)
                require(header.read_bytes() == saved, "Empty recording replaced the previous profile")
                print("Empty recording preserved the profile", flush=True)
            subprocess.run([*command, "build"], cwd=workspace, check=True)
            playback(desktop, exe, workspace, prefix, mode=mode, expected_title=expected_title)
            print(f"{mode}: standalone playback passed", flush=True)
            if mode == "relative":
                playback(desktop, exe, workspace, prefix, mode="resize", expected_title=expected_title)
                print("Playback stopped after a resize without clicking again", flush=True)
        invalid = subprocess.run([str(exe), "unexpected"], cwd=workspace, capture_output=True, text=True)
        require(invalid.returncode != 0 and "takes no arguments" in invalid.stderr and not desktop.window(prefix),
                "The executable accepted an argument or launched Chrome")
        print("Argument rejection passed", flush=True)
        test_imports(exe, compiler)


if __name__ == "__main__":
    main()
