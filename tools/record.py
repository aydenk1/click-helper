#!/usr/bin/env python3
"""Build-time recorder. The distributed executable does not require Python."""
import argparse
import json
import math
import os
from pathlib import Path
import re
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
MAX_REPLY = 4 * 1024 * 1024


def ipc(command):
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    signature = os.environ.get("HYPRLAND_INSTANCE_SIGNATURE")
    if not runtime or not signature:
        raise RuntimeError("Run setup inside your Hyprland desktop, without sudo.")
    path = f"{runtime}/hypr/{signature}/.socket.sock"
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(3)
        connection.connect(path)
        connection.sendall(command.encode())
        data = bytearray()
        while chunk := connection.recv(65536):
            data.extend(chunk)
            if len(data) > MAX_REPLY:
                raise RuntimeError("Hyprland response is too large.")
    return data.decode()


def dispatch(command):
    response = ipc("/dispatch " + command).strip()
    if response != "ok":
        raise RuntimeError(f"Hyprland rejected {command!r}: {response}")


def check_click_support():
    # An invalid mouse button tests the parser without sending any input.
    reply = ipc("/dispatch sendshortcut ,mouse:271,")
    if "invalid mouse button" not in reply:
        raise RuntimeError(
            "This Hyprland version does not expose the expected native mouse "
            "dispatcher. Upgrade Hyprland (mouse:272 in sendshortcut is required). "
            f"Probe response: {reply.strip()}"
        )


def clients():
    return [w for w in json.loads(ipc("j/clients"))
            if w.get("mapped", True) and not w.get("hidden", False)]


def window_class(window):
    return window.get("initialClass") or window.get("class", "")


def address(window):
    value = window.get("address", "")
    if not re.fullmatch(r"0x[0-9a-fA-F]+", value):
        raise RuntimeError("Hyprland returned an invalid window address.")
    return value


def find_window(target):
    for window in clients():
        if address(window) == target:
            return window
    raise RuntimeError("The target window closed during recording.")


def wait_for_window(before, match_class, process, timeout, settle):
    old_addresses = {address(w) for w in before}
    start = time.monotonic()
    first_seen = None
    previous_candidates = ()
    while time.monotonic() - start < timeout:
        current = clients()
        matches = [w for w in current if not match_class or window_class(w) == match_class]
        new = [w for w in matches if address(w) not in old_addresses]
        # Prefer a newly mapped window. With --match-class, a single-instance
        # application may reuse an existing window if launching activated it.
        candidates = new
        if not candidates and match_class and time.monotonic() - start >= settle:
            active = json.loads(ipc("j/activewindow")).get("address")
            candidates = [w for w in matches if address(w) == active]
        if candidates:
            identities = tuple(sorted(address(w) for w in candidates))
            if first_seen is None or identities != previous_candidates:
                first_seen = time.monotonic()
                previous_candidates = identities
            if time.monotonic() - first_seen >= settle:
                if len(candidates) == 1:
                    return candidates[0]
                if not sys.stdin.isatty():
                    raise RuntimeError("Several windows opened; use --match-class to select one.")
                print("Several windows opened:")
                for number, window in enumerate(candidates, 1):
                    print(f"  {number}: {window_class(window)} — {window.get('title', '')}")
                selection = input("Record which window number? ")
                if selection.isdigit() and 1 <= int(selection) <= len(candidates):
                    return candidates[int(selection) - 1]
                raise RuntimeError("Invalid window selection.")
        else:
            first_seen = None
        if process.poll() not in (None, 0) and not candidates:
            raise RuntimeError(f"Program exited with status {process.returncode} before opening a window.")
        time.sleep(0.05)
    raise RuntimeError("No target window appeared. Try --match-class, a new-window option, or a longer --timeout.")


def correct_geometry(window, width, height, position):
    return (window.get("floating") and window.get("size") == [width, height]
            and (position == "relative" or window.get("at") == [0, 0]))


def prepare_window(target, width, height, timeout, position="relative"):
    dispatch(f"focuswindow address:{target}")
    dispatch("fullscreenstate 0 0")
    dispatch(f"setfloating address:{target}")
    dispatch(f"resizewindowpixel exact {width} {height},address:{target}")
    if position == "absolute":
        dispatch(f"movewindowpixel exact 0 0,address:{target}")
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        window = find_window(target)
        if correct_geometry(window, width, height, position):
            time.sleep(0.3)  # Allow resize animations and application layout to settle.
            window = find_window(target)
            if correct_geometry(window, width, height, position):
                return window
        time.sleep(0.05)
    raise RuntimeError("The app did not accept the requested window size or position. Check window rules and monitor layout.")


def c_string(value):
    # Fixed-width octal escapes encode UTF-8 bytes, quotes, backslashes and newlines
    # without permitting source injection or C escape-sequence ambiguity.
    return '"' + ''.join(f"\\{byte:03o}" for byte in value.encode("utf-8")) + '"'


def write_header(destination, argv, match_class, width, height, points,
                 startup_ms, click_ms, timeout_ms, position="relative"):
    if position not in ("absolute", "relative"):
        raise ValueError("position must be absolute or relative")
    lines = [
        "/* Generated by setup.sh. Private build input; do not distribute. */",
        "#ifndef CLICK_HELPER_CONFIG_H", "#define CLICK_HELPER_CONFIG_H",
        f"#define APP_PATH {c_string(argv[0])}",
        f"#define APP_CLASS {c_string(match_class)}",
        f"#define WINDOW_WIDTH {width}", f"#define WINDOW_HEIGHT {height}",
        f"#define POSITION_ABSOLUTE {int(position == 'absolute')}",
        f"#define STARTUP_DELAY_MS {startup_ms}",
        f"#define CLICK_DELAY_MS {click_ms}",
        f"#define LAUNCH_TIMEOUT_MS {timeout_ms}",
        "static char *const APP_ARGV[] = {",
        *(f"    {c_string(arg)}," for arg in argv), "    NULL", "};",
        "static const Point CLICKS[] = {",
        *(f"    {{{x}, {y}}}," for x, y in points), "};", "#endif", ""
    ]
    destination.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".config-", dir=destination.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as output:
            output.write('\n'.join(lines))
        os.replace(temporary, destination)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def positive_size(value):
    number = int(value)
    if not 1 <= number <= 32768:
        raise argparse.ArgumentTypeError("size must be between 1 and 32768")
    return number


def seconds(value):
    number = float(value)
    if not math.isfinite(number) or not 0 <= number <= 3600:
        raise argparse.ArgumentTypeError("time must be between 0 and 3600 seconds")
    return number


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--width", type=positive_size)
    parser.add_argument("--height", type=positive_size)
    parser.add_argument("--position", choices=("absolute", "relative"),
                        help="absolute: desktop (0, 0); relative: launch position (default)")
    parser.add_argument("--match-class")
    parser.add_argument("--startup-delay", type=seconds, default=1.0)
    parser.add_argument("--click-delay", type=seconds, default=0.25)
    parser.add_argument("--timeout", type=seconds, default=20.0)
    parser.add_argument("program", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    argv = args.program
    if argv[:1] == ["--"]:
        argv = argv[1:]
    interactive = not argv
    if not argv:
        argv = shlex.split(input("Program and arguments (e.g. gnome-calculator): "))
    if not argv:
        raise RuntimeError("A program is required.")
    position = args.position
    if position is None:
        position = ((input("Window position (absolute/relative) [relative]: ").strip().lower()
                     or "relative") if interactive else "relative")
    if position not in ("absolute", "relative"):
        raise RuntimeError("Window position must be absolute or relative.")
    width = args.width or positive_size(input("Window width [800]: ") or "800")
    height = args.height or positive_size(input("Window height [600]: ") or "600")
    if round(args.timeout * 1000) <= round(args.startup_delay * 1000):
        raise RuntimeError("--timeout must be longer than --startup-delay.")
    executable = shutil.which(argv[0])
    if not executable:
        raise RuntimeError(f"Program not found or not executable: {argv[0]}")
    argv[0] = os.path.abspath(executable)  # Preserve symlink names used by multicall programs.
    slurp = shutil.which("slurp")
    if not slurp:
        raise RuntimeError("slurp is required for recording.")
    check_click_support()
    before = clients()
    print(f"Launching {shlex.join(argv)}", flush=True)
    process = subprocess.Popen(argv, start_new_session=True)
    window = wait_for_window(before, args.match_class, process, args.timeout, args.startup_delay)
    target = address(window)
    match_class = window_class(window)
    if not match_class:
        raise RuntimeError("The selected window has no class/app ID to match during playback.")
    prepare_window(target, width, height, args.timeout, position)
    print(f"Recording {match_class} at {width}×{height} ({position}). Each selected point WILL be clicked.")
    print("Select points inside this window. Escape finishes. Ctrl+C cancels without saving.", flush=True)
    points = []
    while True:
        selection = subprocess.run([slurp, "-p", "-f", "%x %y"], capture_output=True, text=True)
        if selection.returncode != 0:
            if selection.returncode == 1 and not selection.stderr.strip():
                break  # slurp uses exit status 1 for Escape/cancel.
            raise RuntimeError(f"slurp failed ({selection.returncode}): {selection.stderr.strip()}")
        if not re.fullmatch(r"\s*-?\d+\s+-?\d+\s*", selection.stdout):
            raise RuntimeError("slurp returned invalid coordinates.")
        x, y = map(int, selection.stdout.split())
        window = find_window(target)
        if window.get("size") != [width, height]:
            raise RuntimeError("Window size changed; restart recording with a fixed size.")
        if position == "absolute" and window.get("at") != [0, 0]:
            raise RuntimeError("Window moved away from desktop (0, 0); recording stopped.")
        dx, dy = x - window["at"][0], y - window["at"][1]
        if not (0 <= dx < width and 0 <= dy < height):
            print("Point is outside the target window; select another point.", flush=True)
            continue
        dispatch(f"focuswindow address:{target}")
        dispatch(f"movecursor {x} {y}")
        time.sleep(0.1)
        active = json.loads(ipc("j/activewindow")).get("address")
        latest = find_window(target)
        if (active != target or latest.get("size") != [width, height]
                or latest.get("at") != window.get("at")):
            raise RuntimeError("Window moved, resized, or lost focus before the click; recording stopped.")
        # Empty window selector preserves pointer coordinates and popup focus.
        dispatch("sendshortcut ,mouse:272,")
        points.append((dx, dy))
        print(f"Recorded click {len(points)}: {dx}, {dy}", flush=True)
        time.sleep(args.click_delay)
    if not points:
        raise RuntimeError("No clicks recorded; existing configuration was left untouched.")
    destination = ROOT / "generated/config.h"
    write_header(destination, argv, match_class, width, height, points,
                 round(args.startup_delay * 1000), round(args.click_delay * 1000),
                 round(args.timeout * 1000), position)
    print(f"Saved {len(points)} clicks to {destination}\nRun: make -C {shlex.quote(str(ROOT))}")
    print("Reset/close the target app before testing playback; recording has already performed the clicks.")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nRecording cancelled; existing configuration was left untouched.", file=sys.stderr)
        sys.exit(130)
    except (OSError, RuntimeError, ValueError, argparse.ArgumentTypeError) as error:
        print(f"Setup failed: {error}", file=sys.stderr)
        sys.exit(1)
