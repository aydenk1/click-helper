# Click helper for Hyprland / Omarchy and Windows

Record a sequence of left-clicks in an app and build a standalone replay executable.
Use the [Windows workflow](#windows) or the Hyprland workflow below.

## Windows

Use an unlocked x64 Windows 10 (version 1703 or newer) or Windows 11 desktop.
Recording and building require Python 3.10+ and the x64 Windows UCRT package
from [LLVM-MinGW releases](https://github.com/mstorsjo/llvm-mingw/releases).
Extract the package and put its `bin` directory on `PATH`, or pass
`--cc FULL_COMPILER_PATH` before `record` or `build`.

From the checkout in a Windows terminal:

```powershell
py tools/windows.py record -- "C:\Program Files\Google\Chrome\Application\chrome.exe" --new-window about:blank
py tools/windows.py build
.\build\windows\click-helper.exe
```

1. Wait for the recorder to report that it is ready. If the helper asks you to
   activate the target, select the app from the taskbar.
2. Left-click the sequence inside the target window. Each click goes through to
   the app as you record. Press **Esc** to save or **Ctrl+C** in the launching
   console to cancel. A failed or empty recording preserves the last saved profile.
3. Close or reset the app before replaying. Run `build` after changing the
   recording. The resulting `click-helper.exe` takes zero arguments and runs
   with Windows system libraries. It launches the recorded app from the saved
   absolute executable path and leaves the app open.

The Windows recorder accepts `--width`, `--height`, `--position relative|absolute`,
`--match-class`, `--startup-delay`, `--click-delay`, and `--timeout` before `--`.
The default is an 800×600 window at its launch position. Width and height refer
to the outer window rectangle in physical pixels. Absolute mode places that
rectangle at desktop `(0, 0)`. Use a resizable window that fits on your monitor.
The helper prefers a new window and uses an already-open window only when its
executable and class match and the launch activates it. Record a new sequence if
the display scale, app layout, or initial app state changes. Windows input
permissions apply, so launch normal apps and the helper at the same elevation.
Keep the app visible and avoid using the mouse during playback. Playback stops
if the app loses focus, the recorded size, placement, or DPI changes, or a click
would land outside the target window. Relative mode allows the window to move
between clicks.

The private recording is `generated/windows/config.h`. The Windows build output
is `build/windows/click-helper.exe`. These directories are ignored by Git. The
executable embeds the sequence and launch arguments, which can be inspected.

The opt-in smoke test uses the actual Windows pointer in an isolated Chrome
window. It creates a temporary Chrome profile and a local two-button test page.
Run it on an unlocked desktop and leave the pointer free while it runs.

```powershell
py tests/windows_smoke.py
```

## Hyprland / Omarchy

Record a GUI sequence, then bake its program, arguments, position mode, window size and click
positions into a C executable that takes **no arguments**. Everything works
offline. Mouse movement and left clicks go directly through Hyprland's Unix
socket: **no ydotool, input daemon, sudo at runtime, or hyprctl subprocess**.

## Record, then compile

From your checkout, inside your normal Hyprland desktop:

```bash
./setup.sh
make
./build/click-helper
```

Setup prompts for a program, position mode, and window dimensions. Or supply them explicitly:

```bash
./setup.sh --position relative --width 800 --height 600 -- gnome-calculator
make
./build/click-helper
```

1. Setup checks for a C compiler, Make, Python 3, and `slurp`. On Omarchy/Arch,
   it offers to install missing packages using `sudo pacman -S --needed`.
2. It verifies native mouse-click support in the running Hyprland instance,
   launches your program, focuses its window, disables fullscreen, makes it
   floating, and applies the desired width, height, and position mode.
3. Select each point with the left mouse button. **Each selection is immediately
   replayed as a real click**, so the application's state advances while recording.
   The point picker itself is a `slurp` overlay.
4. Press **Escape** to finish. Ctrl+C aborts without replacing an existing header.
5. Setup writes `generated/config.h`; run `make` to produce
   `build/click-helper`. Reset or close the target app before testing playback.

The generated directory and build outputs are excluded from Git. The header is
created atomically with mode `0600`; new generated directories have mode `0700`.
Distribute the executable with `build/LICENSE.cJSON`, not the generated header.
It is stripped by default.

## Window position

Choose a mode during setup; the compiled executable still takes no arguments:

```bash
# Place the window at the desktop origin and apply the requested size:
./setup.sh --position absolute --width 800 --height 600 -- gnome-calculator

# Keep the compositor-chosen position and apply the requested size:
./setup.sh --position relative --width 800 --height 600 -- gnome-calculator
```

`relative` is the default. Clicks follow the window's top-left corner wherever
it opens. `absolute` moves the floating window to desktop coordinate `(0, 0)`
before both recording and playback, and stops if it is moved away during the
sequence. This is placement for the sequence, not a permanent window-manager
rule preventing the user from dragging it.

Both modes apply the specified size so the application's layout stays consistent.
Coordinates use Hyprland's logical desktop space: `(0, 0)` is the configured
desktop origin, not the focused monitor's corner. With multiple monitors,
screens can extend left/up into negative coordinates. Absolute mode requires
the window to fit at the origin; it does not offset for panels or reserved space.
If window rules prevent the requested size or placement, recording/playback stops.

Run `make` after recording to bake the chosen mode into the executable. Existing
headers generated before this option was added retain relative behavior.

## Programs, timing, and window matching

Any executable that opens a normal, resizable Hyprland or XWayland application
window can be used. Shell syntax is not evaluated. Program arguments retain
spaces, quotes and Unicode literally. For a terminal application, launch a
terminal emulator with its execute-program option. A `.desktop` file itself
is not an executable; use its underlying command.

```bash
./setup.sh --width 1000 --height 700 \
  --startup-delay 2 --click-delay 0.4 --timeout 30 \
  -- /usr/bin/your-program --new-window "argument with spaces"
```

All options above are compiled in. `--startup-delay` is the wait after a matching
window appears; it is not proof that an app's content has finished loading.
Increase it for slow applications. `--click-delay` is the delay after each click;
there is also a 100 ms cursor-settle delay before each click.

Setup discovers the window's initial class/app ID automatically. If other windows
open at the same time, use `--match-class EXACT_APP_ID` to select the target.
You can inspect classes with `hyprctl clients`. The match is exact, not a regex.
For apps that reuse an existing window, specify `--match-class` during recording;
playback already knows the class. Reuse works when launching activates that
window. New matching windows are preferred; multiple new windows with the same
class cause playback to stop rather than guess. A program-specific new-window
or custom-app-ID option can resolve ambiguity.

Click positions are stored relative to the window, in Hyprland logical coordinate
units. Playback reads its current position before each click. The recorded size
must be accepted by the app. Font/DPI/theme/layout or application-state changes
can still change button positions. Use a window size that fits the target monitor.
Recording supports points inside the main window, including menus within its
bounds; separate dialogs and menus outside its bounds need another recording.

Avoid moving the mouse or switching windows during playback. The helper stops
on IPC failures, loss of target focus, window closure, unexpected resizing, or
movement during cursor settling. It leaves the launched application running.
Ctrl+C stops playback when launched from a terminal.

## Runtime requirements and compatibility

- Linux with Hyprland's command socket at
  `$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket.sock`.
- Native `sendshortcut` support for `mouse:272`, plus the `focuswindow`,
  `fullscreenstate`, `setfloating`, `resizewindowpixel`, and `movecursor`
  dispatchers (`movewindowpixel` is also required in absolute mode).
  These interfaces were verified in Hyprland v0.54.0 source.
  Setup/playback use an invalid-button probe that sends no input to check the
  mouse dispatcher. Unsupported or renamed interfaces produce an error.
- The recorded program installed at the **same absolute executable path** with
  compatible command-line arguments. Playback doesn't search `PATH` for it.
- A compatible architecture and C runtime. Build on the target distro when
  distributing; this isn't a universal static binary.

Python, `slurp`, Make and the compiler are only needed for recording/building.
The program inherits the current working directory and desktop environment.
Use absolute paths for file arguments that must work from other directories.
The executable accepts no arguments and no click configuration file or delay
environment variable. To change its settings, record again and rebuild.

The mouse shortcut intentionally has an **empty window selector**. After
focusing and moving the pointer, this clicks at its current position; explicitly
targeting a window in `sendshortcut` can reset pointer focus to local `(1, 1)`.

## Tests

```bash
make test
```

Tests compile the actual C source with strict warnings and run both playback and
recording against a simulated Hyprland Unix socket. They cover program argument
escaping, window-relative positions, recording/header generation, unsupported
compositors, and failure handling. They do not verify real desktop input delivery;
test a harmless sequence in your actual app before distributing a build.

## Source confidentiality

Embedding data and removing subprocess arguments makes casual inspection harder.
It is not encryption: someone controlling the computer can inspect the binary,
memory, or IPC traffic. The generated header contains your complete recording.

The bundled JSON parser is cJSON v1.7.19, from
https://github.com/DaveGamble/cJSON/tree/v1.7.19, under the MIT license in
`vendor/LICENSE.cJSON`. Preserve that notice when redistributing it in source or
binary form. No JSON development package or network access is needed to build.
