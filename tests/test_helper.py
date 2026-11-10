"""Exercise real C playback and Python recording against a local fake compositor."""
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import socketserver
import stat
import subprocess
import sys
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("recorder", ROOT / "tools/record.py")
recorder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(recorder)
CLASS = 'Test "App" \\ café'
ARGUMENTS = ["space argument", 'quote"slash\\', "$(not-a-command)", "line\nbreak", "雪"]


class Compositor(socketserver.UnixStreamServer):
    def __init__(self, directory):
        self.directory = directory
        self.marker = directory / "launched.json"
        self.commands = []
        self.supports_clicks = True
        self.fail_click = False
        self.refuse_size = False
        self.refuse_position = False
        self.lose_focus = False
        self.close_window = False
        self.existing = False
        self.multiple = False
        self.moved = False
        self.move_during_settle = False
        self.move_after_click = False
        self.invalid_json = False
        self.position = [-500, 200]
        self.window = {"address": "0x123", "initialClass": CLASS, "class": CLASS,
                       "mapped": True, "hidden": False, "floating": False,
                       "at": self.position, "size": [400, 300], "title": "Test"}
        path = directory / "hypr/test/.socket.sock"
        path.parent.mkdir(parents=True)
        super().__init__(str(path), Handler)
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()

    def reply(self, command):
        self.commands.append(command)
        if command == "/dispatch sendshortcut ,mouse:271,":
            return "sendshortcut: invalid mouse button" if self.supports_clicks else "invalid key"
        if command == "j/clients":
            if self.invalid_json:
                return "not json"
            if self.close_window and self.moved:
                return "[]"
            if not self.marker.exists() and not self.existing:
                return "[]"
            windows = [self.window]
            if self.multiple and self.marker.exists():
                windows.append(self.window | {"address": "0x456"})
            return json.dumps(windows)
        if command == "j/activewindow":
            if self.lose_focus and self.moved:
                return json.dumps({"address": "0x999"})
            return json.dumps(self.window)
        if command.startswith("/dispatch setfloating "):
            self.window["floating"] = True
        if command.startswith("/dispatch resizewindowpixel exact ") and not self.refuse_size:
            self.window["size"] = [800, 600]
        if command.startswith("/dispatch movewindowpixel exact ") and not self.refuse_position:
            self.window["at"] = [0, 0]
        if command.startswith("/dispatch movecursor "):
            self.moved = True
            if self.move_during_settle:
                self.window["at"] = [0, 0]
        if command == "/dispatch sendshortcut ,mouse:272," and self.fail_click:
            return "mouse input rejected"
        if command == "/dispatch sendshortcut ,mouse:272," and self.move_after_click:
            self.window["at"] = [50, 50]
        return "ok"

    def stop(self):
        self.shutdown()
        self.server_close()
        self.thread.join()


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        command = self.request.recv(4096).decode()
        reply = self.server.reply(command).encode()
        # Fragment responses to exercise the client's receive loop.
        middle = len(reply) // 2
        self.request.sendall(reply[:middle])
        self.request.sendall(reply[middle:])


class HelperTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build_dir = tempfile.TemporaryDirectory(prefix="ch-build-")
        cls.build = Path(cls.build_dir.name)
        cls.argv = [sys.executable, "-c",
                    "import json,os,sys; from pathlib import Path; "
                    "Path(os.environ['LAUNCH_LOG']).write_text(json.dumps(sys.argv[1:]))",
                    *ARGUMENTS]
        compiler = shutil.which("cc")
        if not compiler:
            raise unittest.SkipTest("C compiler is required")
        cls.binaries = {}
        for position in ("relative", "absolute", "legacy"):
            header = cls.build / "config.h"
            recorder.write_header(header, cls.argv, CLASS, 800, 600,
                                  [(10, 20), (300, 400)], 10, 0, 600,
                                  "relative" if position == "legacy" else position)
            if position == "legacy":
                header.write_text(header.read_text().replace("#define POSITION_ABSOLUTE 0\n", ""))
            binary = cls.build / ("helper-" + position)
            subprocess.run([compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Wpedantic",
                            "-Werror", "-I" + str(cls.build), "-I" + str(ROOT / "vendor"),
                            str(ROOT / "src/helper.c"), str(ROOT / "vendor/cJSON.c"),
                            "-o", str(binary)], check=True)
            cls.binaries[position] = binary

    @classmethod
    def tearDownClass(cls):
        cls.build_dir.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ch-test-")
        self.directory = Path(self.temp.name)
        self.server = Compositor(self.directory)
        self.env = dict(os.environ, XDG_RUNTIME_DIR=str(self.directory),
                        HYPRLAND_INSTANCE_SIGNATURE="test", LAUNCH_LOG=str(self.server.marker),
                        PATH="/nonexistent")

    def tearDown(self):
        self.server.stop()
        self.temp.cleanup()

    def playback(self, *args, position="relative"):
        return subprocess.run([self.binaries[position], *args], env=self.env,
                              capture_output=True, text=True, timeout=5)

    def clicks(self):
        return self.server.commands.count("/dispatch sendshortcut ,mouse:272,")

    def test_playback_relative_coordinates_and_literal_program_arguments(self):
        result = self.playback()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(self.server.marker.read_text()), ARGUMENTS)
        self.assertEqual(self.clicks(), 2)
        movements = [s for s in self.server.commands if s.startswith("/dispatch movecursor ")]
        self.assertEqual(movements, ["/dispatch movecursor -490 220", "/dispatch movecursor -200 600"])
        self.assertIn("/dispatch resizewindowpixel exact 800 600,address:0x123", self.server.commands)
        self.assertFalse(any("movewindowpixel" in command for command in self.server.commands))

    def test_absolute_playback_moves_and_clicks_at_desktop_origin(self):
        result = self.playback(position="absolute")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.server.window["at"], [0, 0])
        self.assertIn("/dispatch movewindowpixel exact 0 0,address:0x123", self.server.commands)
        movements = [s for s in self.server.commands if s.startswith("/dispatch movecursor ")]
        self.assertEqual(movements, ["/dispatch movecursor 10 20", "/dispatch movecursor 300 400"])
        self.assertEqual(self.clicks(), 2)

    def test_absolute_playback_refused_placement_stops_before_click(self):
        self.server.refuse_position = True
        result = self.playback(position="absolute")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("window size or position", result.stderr)
        self.assertEqual(self.clicks(), 0)

    def test_absolute_playback_stops_if_window_leaves_origin(self):
        self.server.move_after_click = True
        result = self.playback(position="absolute")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("moved away from desktop", result.stderr)
        self.assertEqual(self.clicks(), 1)

    def test_relative_playback_tracks_moves_between_clicks(self):
        self.server.move_after_click = True
        result = self.playback()
        self.assertEqual(result.returncode, 0, result.stderr)
        movements = [s for s in self.server.commands if s.startswith("/dispatch movecursor ")]
        self.assertEqual(movements, ["/dispatch movecursor -490 220", "/dispatch movecursor 350 450"])

    def test_legacy_header_preserves_relative_behavior(self):
        result = self.playback(position="legacy")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("/dispatch movecursor -490 220", self.server.commands)
        self.assertFalse(any("movewindowpixel" in command for command in self.server.commands))

    def test_no_arguments_are_accepted(self):
        result = self.playback("play")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("takes no arguments", result.stderr)
        self.assertEqual(self.server.commands, [])

    def test_unsupported_compositor_does_not_launch_app(self):
        self.server.supports_clicks = False
        result = self.playback()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.server.marker.exists())
        self.assertEqual(self.clicks(), 0)

    def test_click_failure_stops_sequence(self):
        self.server.fail_click = True
        result = self.playback()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("mouse input rejected", result.stderr)
        self.assertEqual(self.clicks(), 1)

    def test_focus_loss_stops_before_click(self):
        self.server.lose_focus = True
        result = self.playback()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("lost focus", result.stderr)
        self.assertEqual(self.clicks(), 0)

    def test_closed_window_stops_before_click(self):
        self.server.close_window = True
        result = self.playback()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.clicks(), 0)

    def test_window_moving_before_click_stops_playback(self):
        self.server.move_during_settle = True
        result = self.playback()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("moved or resized", result.stderr)
        self.assertEqual(self.clicks(), 0)

    def test_refused_window_size_stops_before_click(self):
        self.server.refuse_size = True
        result = self.playback()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("window size", result.stderr)
        self.assertEqual(self.clicks(), 0)

    def test_single_instance_window_can_be_reused(self):
        self.server.existing = True
        result = self.playback()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.clicks(), 2)

    def test_ambiguous_new_windows_are_rejected(self):
        self.server.multiple = True
        result = self.playback()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("several new windows", result.stderr)
        self.assertEqual(self.clicks(), 0)

    def test_invalid_json_is_reported(self):
        self.server.invalid_json = True
        result = self.playback()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("invalid JSON", result.stderr)

    def test_missing_desktop_is_reported(self):
        self.env.pop("HYPRLAND_INSTANCE_SIGNATURE")
        result = self.playback()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("inside Hyprland", result.stderr)

    def record(self, points, position=None, interactive=False):
        # Copy only the recorder into a temporary repo, so tests cannot replace
        # the user's real generated/config.h.
        repo = self.directory / "repo"
        (repo / "tools").mkdir(parents=True)
        shutil.copy(ROOT / "tools/record.py", repo / "tools/record.py")
        bindir = self.directory / "bin"
        bindir.mkdir()
        picker = bindir / "slurp"
        picker.write_text(f"#!{sys.executable}\n" +
                          "import json,os,sys\nfrom pathlib import Path\n" +
                          "p=Path(os.environ['POINTS']); points=json.loads(p.read_text())\n" +
                          "if not points: sys.exit(1)\n" +
                          "print(points.pop(0)); p.write_text(json.dumps(points))\n")
        picker.chmod(0o700)
        point_file = self.directory / "points.json"
        point_file.write_text(json.dumps(points))
        destination = repo / "generated/config.h"
        destination.parent.mkdir()
        destination.write_text("old header\n")
        env = self.env | {"PATH": str(bindir), "POINTS": str(point_file)}
        command = [sys.executable, str(repo / "tools/record.py"), "--width", "800", "--height", "600",
                   "--startup-delay", "0.01", "--click-delay", "0", "--timeout", "1"]
        if position is not None:
            command += ["--position", position]
        if not interactive:
            command += ["--", *self.argv]
        # The interactive prompt reads one line; newline-containing argument
        # coverage belongs to the explicit argv tests above.
        interactive_command = shlex.join(self.argv[:-len(ARGUMENTS)])
        result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=5,
                                input=interactive_command + "\nabsolute\n" if interactive else None)
        return result, destination

    def test_record_generates_private_header_with_relative_coordinates(self):
        result, destination = self.record(["-490 220", "-200 600"])
        self.assertEqual(result.returncode, 0, result.stderr)
        content = destination.read_text()
        self.assertIn("{10, 20}", content)
        self.assertIn("{300, 400}", content)
        self.assertIn("#define WINDOW_WIDTH 800", content)
        self.assertIn("#define POSITION_ABSOLUTE 0", content)
        self.assertFalse(any("movewindowpixel" in command for command in self.server.commands))
        self.assertIn(recorder.c_string(CLASS), content)
        self.assertEqual(stat.S_IMODE(destination.stat().st_mode), 0o600)
        self.assertEqual(self.clicks(), 2)

    def test_absolute_recording_moves_window_and_embeds_mode(self):
        result, destination = self.record(["10 20", "300 400"], position="absolute")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("#define POSITION_ABSOLUTE 1", destination.read_text())
        self.assertIn("{10, 20}", destination.read_text())
        self.assertIn("/dispatch movewindowpixel exact 0 0,address:0x123", self.server.commands)
        self.assertEqual(self.clicks(), 2)

    def test_interactive_recording_prompts_for_position(self):
        result, destination = self.record(["10 20"], interactive=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Window position (absolute/relative)", result.stdout)
        self.assertIn("#define POSITION_ABSOLUTE 1", destination.read_text())

    def test_explicit_relative_recording_preserves_placement(self):
        result, destination = self.record(["-490 220"], position="relative")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("#define POSITION_ABSOLUTE 0", destination.read_text())
        self.assertEqual(self.server.window["at"], [-500, 200])

    def test_absolute_recording_refused_placement_preserves_header(self):
        self.server.refuse_position = True
        result, destination = self.record(["10 20"], position="absolute")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("window size or position", result.stderr)
        self.assertEqual(destination.read_text(), "old header\n")
        self.assertEqual(self.clicks(), 0)

    def test_invalid_position_mode_does_not_launch_or_replace_header(self):
        result, destination = self.record(["10 20"], position="invalid")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("invalid choice", result.stderr)
        self.assertEqual(destination.read_text(), "old header\n")
        self.assertFalse(self.server.marker.exists())

    def test_record_skips_outside_points(self):
        result, destination = self.record(["5000 5000", "-490 220"])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("outside", result.stdout)
        self.assertNotIn("5000", destination.read_text())
        self.assertEqual(self.clicks(), 1)

    def test_empty_recording_preserves_existing_header(self):
        result, destination = self.record([])
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(destination.read_text(), "old header\n")

    def test_record_focus_loss_preserves_existing_header(self):
        self.server.lose_focus = True
        result, destination = self.record(["-490 220"])
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(destination.read_text(), "old header\n")
        self.assertEqual(self.clicks(), 0)


if __name__ == "__main__":
    unittest.main()
