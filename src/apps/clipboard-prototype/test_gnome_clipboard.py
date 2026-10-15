#!/usr/bin/python3
# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT
"""Explicit native GNOME Wayland ↔ XWayland clipboard acceptance.

Uses dedicated GTK4 windows and REPLACES THE DESKTOP CLIPBOARD. Not registered
with CTest. Run with system Python (python3-gi, GTK4 introspection required).
Does not claim that the full core/Portal or a real Mac peer has been tested.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time


def fixture(args):
    import gi
    gi.require_version("Gtk", "4.0")
    gi.require_version("Gdk", "4.0")
    from gi.repository import Gdk, Gio, GLib, Gtk

    result = 1
    app = Gtk.Application(application_id="org.deskflow.ClipboardAcceptance",
                          flags=Gio.ApplicationFlags.NON_UNIQUE)

    def activate(app):
        nonlocal result
        window = Gtk.ApplicationWindow(application=app, title="Deskflow clipboard acceptance")
        window.set_default_size(400, 100)
        window.set_child(Gtk.Label(label="Deskflow GNOME Wayland clipboard test"))
        window.present()
        display = window.get_display()
        if display.__gtype__.name != "GdkWaylandDisplay":
            app.quit()
            return
        clipboard = display.get_clipboard()
        published_manifest = None

        def publish_current():
            nonlocal published_manifest
            manifest = args.fixture_publish.read_bytes()
            if manifest == published_manifest:
                return True
            entries = json.loads(manifest)
            providers = [Gdk.ContentProvider.new_for_bytes(mime, GLib.Bytes.new(Path(path).read_bytes()))
                         for mime, path in entries.items()]
            provider = Gdk.ContentProvider.new_union(providers)
            if not clipboard.set_content(provider):
                app.quit()
                return False
            published_manifest = manifest
            print(json.dumps({"event": "ready", "backend": display.__gtype__.name,
                              "active": window.is_active()}), flush=True)
            return True

        def start():
            nonlocal result
            if args.fixture_publish:
                publish_current()
                GLib.timeout_add(100, publish_current)
            else:
                data = bytearray()

                def received(stream, res):
                    nonlocal result
                    try:
                        chunk = stream.read_bytes_finish(res).get_data()
                        data.extend(chunk)
                        if len(data) > 16 * 1024 * 1024:
                            raise RuntimeError("native format exceeds limit")
                        if chunk:
                            stream.read_bytes_async(65536, GLib.PRIORITY_DEFAULT, None, received)
                        else:
                            args.output.write_bytes(data)
                            print(json.dumps({"event": "read", "backend": display.__gtype__.name,
                                              "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}), flush=True)
                            result = 0
                            app.quit()
                    except Exception as error:
                        print(str(error), file=sys.stderr)
                        app.quit()

                def opened(clipboard, res):
                    try:
                        stream, mime = clipboard.read_finish(res)
                        if mime != args.fixture_read:
                            raise RuntimeError("unexpected native MIME")
                        stream.read_bytes_async(65536, GLib.PRIORITY_DEFAULT, None, received)
                    except Exception as error:
                        print(str(error), file=sys.stderr)
                        app.quit()

                clipboard.read_async([args.fixture_read], GLib.PRIORITY_DEFAULT, None, opened)
            return False

        GLib.timeout_add(300, start)
        GLib.timeout_add_seconds(15 if args.fixture_read else 90, lambda: app.quit() or False)
        GLib.timeout_add(100, lambda: True)

    app.connect("activate", activate)
    signal.signal(signal.SIGTERM, lambda *_: app.quit())
    app.run([])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path)
    parser.add_argument("--fixture-publish", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--fixture-read", help=argparse.SUPPRESS)
    parser.add_argument("--output", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.fixture_publish or args.fixture_read:
        return fixture(args)
    if not args.probe or os.environ.get("XDG_SESSION_TYPE") != "wayland" or not os.environ.get("DISPLAY"):
        parser.error("requires --probe and a GNOME Wayland session with XWayland")
    from test_x11_formats import png_bytes, rows, stop, wait_for

    tool = str(args.probe.resolve())
    env = dict(os.environ, GDK_BACKEND="wayland", GTK_A11Y="none")
    processes, streams = [], []
    with tempfile.TemporaryDirectory(prefix="deskflow-native-gnome-") as directory:
        work = Path(directory)

        def launch(name, command):
            log, err = work / (name + ".jsonl"), work / (name + ".stderr")
            out, errors = log.open("wb"), err.open("wb")
            streams.extend([out, errors])
            p = subprocess.Popen(command, env=env, stdout=out, stderr=errors)
            processes.append(p)
            wait_for(lambda: any(r.get("event") == "ready" for r in rows(log)))
            return p

        def native_publish(name, entries):
            manifest = work / (name + ".json")
            manifest.write_text(json.dumps({mime: str(path) for mime, path in entries.items()}))
            return launch(name, [sys.executable, __file__, "--fixture-publish", str(manifest)])

        def native_read(mime, expected):
            output = work / "native-output"
            completed = subprocess.run([sys.executable, __file__, "--fixture-read", mime, "--output", str(output)],
                                       env=env, capture_output=True, timeout=20)
            assert completed.returncode == 0, (mime, completed.stdout, completed.stderr)
            assert output.read_bytes() == expected, mime

        def probe(*argv):
            result = subprocess.run([tool, *map(str, argv)], env=env, capture_output=True, timeout=10)
            assert result.returncode == 0, (argv, result.stdout, result.stderr)
            return result.stdout

        try:
            formats = {"text/plain;charset=utf-8": "GNOME 原生复制\n".encode(),
                       "text/html": b"<p><b>Wayland</b> &amp; XWayland</p>", "image/png": png_bytes()}
            inputs = {}
            for index, (mime, data) in enumerate(formats.items()):
                path = work / ("format-" + str(index))
                path.write_bytes(data)
                inputs[mime] = path
            owner = native_publish("native-formats", inputs)
            for mime, data in formats.items():
                assert probe("read", "--target", mime) == data, "native capture " + mime
            stop(owner)

            command = [tool, "publish-formats"]
            for mime, path in inputs.items():
                command += ["--format", mime + "=" + str(path)]
            owner = launch("x11-formats", command)
            for mime, data in formats.items():
                native_read(mime, data)
            stop(owner)

            source = work / "Unicode 文件 folder"
            source.mkdir()
            (source / "hello.txt").write_text("file reference, body stays local")
            uri_list = work / "uri-list"
            uri_list.write_text(source.as_uri() + "\r\n")
            owner = native_publish("native-files", {"text/uri-list": uri_list})
            assert json.loads(probe("capture-files")) == [str(source)]
            stop(owner)
            owner = launch("x11-files", [tool, "publish", str(source)])
            native_read("text/uri-list", (source.as_uri() + "\r\n").encode())
            stop(owner)
            print("PASS: native GdkWaylandDisplay ↔ XWayland; text/HTML/PNG/file-list, exact bytes both directions")
        except Exception:
            for path in work.glob("*.stderr"):
                print(path.name + ":\n" + path.read_text(errors="replace"), file=sys.stderr)
            raise
        finally:
            for p in reversed(processes):
                stop(p)
            for stream in streams:
                stream.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
