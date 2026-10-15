#!/usr/bin/env python3
"""Isolated X11 format, lazy-provider, owner-generation and INCR integration tests.

Never uses the caller's DISPLAY. Starts Xvfb on an automatically allocated
private display. Set XVFB_BINARY or pass --xvfb for an unpacked local Xvfb.
"""

import argparse
import ctypes as C
import ctypes.util
import json
import os
from pathlib import Path
import random
import select
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib


class SelectionEvent(C.Structure):
    _fields_ = [("type", C.c_int), ("serial", C.c_ulong), ("send_event", C.c_int),
                ("display", C.c_void_p), ("requestor", C.c_ulong), ("selection", C.c_ulong),
                ("target", C.c_ulong), ("property", C.c_ulong), ("time", C.c_ulong)]


class SelectionRequest(C.Structure):
    _fields_ = [("type", C.c_int), ("serial", C.c_ulong), ("send_event", C.c_int),
                ("display", C.c_void_p), ("owner", C.c_ulong), ("requestor", C.c_ulong),
                ("selection", C.c_ulong), ("target", C.c_ulong), ("property", C.c_ulong),
                ("time", C.c_ulong)]


class PropertyEvent(C.Structure):
    _fields_ = [("type", C.c_int), ("serial", C.c_ulong), ("send_event", C.c_int),
                ("display", C.c_void_p), ("window", C.c_ulong), ("atom", C.c_ulong),
                ("time", C.c_ulong), ("state", C.c_int)]


class Event(C.Union):
    _fields_ = [("type", C.c_int), ("selection", SelectionEvent),
                ("request", SelectionRequest), ("property", PropertyEvent), ("pad", C.c_long * 24)]


def xlib():
    lib = C.CDLL(ctypes.util.find_library("X11"))
    signatures = {
        "XOpenDisplay": (C.c_void_p, [C.c_char_p]),
        "XDefaultRootWindow": (C.c_ulong, [C.c_void_p]),
        "XCreateSimpleWindow": (C.c_ulong, [C.c_void_p, C.c_ulong, C.c_int, C.c_int, C.c_uint,
                                          C.c_uint, C.c_uint, C.c_ulong, C.c_ulong]),
        "XSelectInput": (C.c_int, [C.c_void_p, C.c_ulong, C.c_long]),
        "XInternAtom": (C.c_ulong, [C.c_void_p, C.c_char_p, C.c_int]),
        "XGetSelectionOwner": (C.c_ulong, [C.c_void_p, C.c_ulong]),
        "XSetSelectionOwner": (C.c_int, [C.c_void_p, C.c_ulong, C.c_ulong, C.c_ulong]),
        "XConvertSelection": (C.c_int, [C.c_void_p, C.c_ulong, C.c_ulong, C.c_ulong,
                                        C.c_ulong, C.c_ulong]),
        "XPending": (C.c_int, [C.c_void_p]),
        "XNextEvent": (C.c_int, [C.c_void_p, C.POINTER(Event)]),
        "XFlush": (C.c_int, [C.c_void_p]),
        "XSync": (C.c_int, [C.c_void_p, C.c_int]),
        "XCloseDisplay": (C.c_int, [C.c_void_p]),
        "XSendEvent": (C.c_int, [C.c_void_p, C.c_ulong, C.c_int, C.c_long, C.POINTER(Event)]),
        "XChangeProperty": (C.c_int, [C.c_void_p, C.c_ulong, C.c_ulong, C.c_ulong,
                                     C.c_int, C.c_int, C.c_void_p, C.c_int]),
        "XGetWindowProperty": (C.c_int, [C.c_void_p, C.c_ulong, C.c_ulong, C.c_long,
                                        C.c_long, C.c_int, C.c_ulong, C.POINTER(C.c_ulong),
                                        C.POINTER(C.c_int), C.POINTER(C.c_ulong),
                                        C.POINTER(C.c_ulong), C.POINTER(C.c_void_p)]),
        "XFree": (C.c_int, [C.c_void_p]),
    }
    for name, (result, arguments) in signatures.items():
        method = getattr(lib, name)
        method.restype = result
        method.argtypes = arguments
    return lib


class Peer:
    def __init__(self, lib, display):
        self.lib = lib
        self.display = lib.XOpenDisplay(display.encode())
        assert self.display, "Could not connect to isolated display"
        self.window = lib.XCreateSimpleWindow(self.display, lib.XDefaultRootWindow(self.display),
                                              0, 0, 1, 1, 0, 0, 0)
        lib.XSelectInput(self.display, self.window, 1 << 22)
        self.clipboard = self.atom("CLIPBOARD")
        self.property = self.atom("_DESKFLOW_TEST_PROPERTY")
        lib.XSync(self.display, 0)

    def close(self):
        if self.display:
            self.lib.XCloseDisplay(self.display)
            self.display = None

    def atom(self, name):
        return self.lib.XInternAtom(self.display, name.encode(), 0)

    def wait(self, predicate, timeout=5):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            while self.lib.XPending(self.display):
                event = Event()
                self.lib.XNextEvent(self.display, C.byref(event))
                if predicate(event):
                    return event
            time.sleep(0.005)
        raise AssertionError("Timed out waiting for isolated X11 event")

    def request(self, target, property_atom=None):
        property_atom = self.property if property_atom is None else property_atom
        self.lib.XConvertSelection(self.display, self.clipboard, self.atom(target), property_atom, self.window, 0)
        self.lib.XFlush(self.display)
        return self.wait(lambda event: event.type == 31 and event.selection.target == self.atom(target))

    def get(self, window, property_atom, remove=False):
        actual_type, width, count, remaining, pointer = C.c_ulong(), C.c_int(), C.c_ulong(), C.c_ulong(), C.c_void_p()
        status = self.lib.XGetWindowProperty(self.display, window, property_atom, 0, 5 * 1024 * 1024,
                                            int(remove), 0, C.byref(actual_type), C.byref(width),
                                            C.byref(count), C.byref(remaining), C.byref(pointer))
        assert status == 0 and remaining.value == 0
        try:
            if width.value == 32:
                data = list(C.cast(pointer, C.POINTER(C.c_ulong))[:count.value])
            else:
                data = C.string_at(pointer, count.value * width.value // 8) if count.value else b""
            return actual_type.value, width.value, data
        finally:
            if pointer.value:
                self.lib.XFree(pointer)

    def put32(self, window, property_atom, type_atom, values):
        payload = (C.c_ulong * len(values))(*values)
        self.lib.XChangeProperty(self.display, window, property_atom, type_atom, 32, 0,
                                 C.cast(payload, C.c_void_p), len(values))

    def put8(self, window, property_atom, type_atom, data):
        payload = C.create_string_buffer(data)
        self.lib.XChangeProperty(self.display, window, property_atom, type_atom, 8, 0,
                                 C.cast(payload, C.c_void_p), len(data))
        self.lib.XFlush(self.display)

    def notify(self, request, property_atom):
        event = Event()
        event.selection = SelectionEvent(31, 0, 1, self.display, request.requestor, request.selection,
                                          request.target, property_atom, request.time)
        self.lib.XSendEvent(self.display, request.requestor, 0, 0, C.byref(event))
        self.lib.XFlush(self.display)


def png_bytes():
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    pixels = random.Random(412).randbytes(512 * 512 * 3)
    scanlines = b"".join(b"\0" + pixels[start:start + 1536] for start in range(0, len(pixels), 1536))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 512, 512, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(scanlines)) + chunk(b"IEND", b""))


def rows(path):
    if not path.exists():
        return []
    return [json.loads(line) for line in path.read_text().splitlines() if line]


def wait_for(predicate, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.01)
    raise AssertionError("Condition timed out")


def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--service-tool", type=Path, help="Also test clipboard-to-authorized-file-service capture")
    parser.add_argument("--xvfb", default=os.environ.get("XVFB_BINARY", shutil.which("Xvfb")))
    args = parser.parse_args()
    if not args.xvfb:
        print("SKIP: Xvfb unavailable; pass --xvfb or XVFB_BINARY", file=sys.stderr)
        return 77
    tool = args.tool.resolve()
    processes = []
    peers = []
    with tempfile.TemporaryDirectory(prefix="deskflow-x11-test-") as directory:
        work = Path(directory)
        read_fd, write_fd = os.pipe()
        with (work / "xvfb.stderr").open("wb") as errors:
            server = subprocess.Popen([str(args.xvfb), "-displayfd", str(write_fd), "-screen", "0", "1024x768x24",
                                       "-nolisten", "tcp", "-noreset", "-ac"], pass_fds=(write_fd,),
                                      stdout=subprocess.DEVNULL, stderr=errors)
        os.close(write_fd)
        processes.append(server)
        try:
            ready, _, _ = select.select([read_fd], [], [], 10)
            assert ready, (work / "xvfb.stderr").read_text()
            display = ":" + os.read(read_fd, 64).decode().strip()
            assert display != ":", (work / "xvfb.stderr").read_text()
            environment = dict(os.environ, DISPLAY=display)
            environment.pop("WAYLAND_DISPLAY", None)
            library = xlib()

            def run(*arguments, success=True):
                result = subprocess.run([str(tool), *map(str, arguments)], env=environment,
                                        capture_output=True, timeout=10)
                assert (result.returncode == 0) == success, (arguments, result.stdout, result.stderr)
                return result

            def launch(name, *arguments):
                log = work / (name + ".jsonl")
                errors = (work / (name + ".stderr")).open("wb")
                try:
                    process = subprocess.Popen([str(tool), str(arguments[0]), "--log", str(log), *map(str, arguments[1:])],
                                               env=environment, stdout=subprocess.DEVNULL, stderr=errors)
                finally:
                    errors.close()
                processes.append(process)
                wait_for(lambda: any(row["event"] == "ready" for row in rows(log)) or process.poll() is not None)
                assert process.poll() is None, (name, rows(log), (work / (name + ".stderr")).read_text())
                return process, log

            values = {
                "text/plain;charset=utf-8": "hello Ubuntu\n含 空格\0binary-safe\n".encode(),
                "text/html": "<p>Deskflow <b>测试</b></p>\n".encode(),
                "image/png": png_bytes(),
                "text/uri-list": b"file:///tmp/deskflow-only-metadata.txt\r\nfile:///tmp/a%20b\r\n",
                "x-special/gnome-copied-files": b"copy\nfile:///tmp/deskflow-only-metadata.txt\nfile:///tmp/a%20b",
            }
            assert len(values["image/png"]) > 65536
            arguments = []
            sources = {}
            for index, (mime, content) in enumerate(values.items()):
                source = work / (str(index) + ".bin")
                source.write_bytes(content)
                sources[mime] = source
                arguments += ["--format", mime + "=" + str(source)]

            watcher, watch_log = launch("watch", "watch", "--origin", "watch-service")
            publisher, publisher_log = launch("publisher", "publish-formats", *arguments,
                                               "--origin", "remote-offer-a", "--timeout-ms", "1000")
            wait_for(lambda: any(row["event"] == "clipboard_offer" for row in rows(watch_log)))
            announced = [row for row in rows(watch_log) if row["event"] == "clipboard_offer"][-1]
            assert set(values) <= set(announced["targets"])
            assert not any(row["event"] == "data_provided" for row in rows(publisher_log)), \
                "TARGETS/watch eagerly read data providers"
            for mime, expected in values.items():
                result = run("read", "--target", mime)
                assert result.stdout == expected, mime
                if mime == "image/png":
                    assert any(row.get("incremental") for row in map(json.loads, result.stderr.splitlines()))
            assert run("read", "--target", "UTF8_STRING").stdout == values["text/plain;charset=utf-8"]
            listed = run("read", "--target", "TARGETS").stdout.decode().splitlines()
            assert set(values) <= set(listed) and {"TARGETS", "TIMESTAMP", "MULTIPLE"} <= set(listed)
            assert run("read", "--target", "TIMESTAMP").stdout.strip().isdigit()
            run("read", "--target", "application/not-offered", success=False)
            output = work / "exact.png"
            run("read", "--target", "image/png", "--output", output)
            assert output.read_bytes() == values["image/png"]

            peer = Peer(library, display)
            peers.append(peer)
            legacy = peer.request("text/html", 0)
            assert legacy.selection.property == peer.atom("text/html")
            assert peer.get(peer.window, legacy.selection.property)[2] == values["text/html"]
            text_property, missing_property = peer.atom("_TEXT"), peer.atom("_MISSING")
            pairs = [peer.atom("text/html"), text_property, peer.atom("application/not-offered"), missing_property]
            peer.put32(peer.window, peer.property, peer.atom("ATOM_PAIR"), pairs)
            multi = peer.request("MULTIPLE")
            assert multi.selection.property == peer.property
            assert peer.get(peer.window, peer.property)[2] == [pairs[0], text_property, pairs[2], 0]
            assert peer.get(peer.window, text_property)[2] == values["text/html"]

            stalled = Peer(library, display)
            peers.append(stalled)
            response = stalled.request("image/png")
            assert stalled.get(stalled.window, response.selection.property)[0] == stalled.atom("INCR")
            wait_for(lambda: any(row["event"] == "incremental_cancelled" and row.get("status") == "TIMEOUT"
                                 for row in rows(publisher_log)))
            assert run("read", "--target", "image/png").stdout == values["image/png"]
            for _ in range(9):
                vanished = Peer(library, display)
                response = vanished.request("image/png")
                assert response.selection.property != 0
                vanished.close()
            assert run("read", "--target", "image/png").stdout == values["image/png"]
            saturated = []
            for _ in range(8):
                requestor = Peer(library, display)
                peers.append(requestor)
                saturated.append(requestor)
                assert requestor.request("image/png").selection.property != 0
            overflow = Peer(library, display)
            peers.append(overflow)
            assert overflow.request("image/png").selection.property == 0
            for requestor in saturated:
                requestor.close()
            assert run("read", "--target", "image/png").stdout == values["image/png"]

            self_watch, self_log = launch("self-watch", "watch", "--origin", "remote-offer-a")
            wait_for(lambda: any(row["event"] == "self_offer_ignored" for row in rows(self_log)))
            assert not any(row["event"] == "clipboard_offer" for row in rows(self_log))
            stop(self_watch)
            once = run("watch", "--once", "--origin", "other-service")
            assert any(row["event"] == "clipboard_offer" for row in map(json.loads, once.stdout.splitlines()))

            replacement = work / "replacement.txt"
            replacement.write_bytes(b"replacement offer")
            newer, newer_log = launch("newer", "publish-formats", "--format", "text/plain;charset=utf-8=" + str(replacement),
                                      "--origin", "remote-offer-b")
            wait_for(lambda: publisher.poll() is not None)
            assert publisher.returncode == 0 and any(row["event"] == "clipboard_lost" for row in rows(publisher_log))
            wait_for(lambda: len([row for row in rows(watch_log) if row["event"] == "clipboard_offer"]) >= 2)
            assert run("read", "--target", "text/plain;charset=utf-8").stdout == b"replacement offer"
            stop(newer)

            # Capture local references only. No file has to exist on disk.
            paths = [str(work / "含 空格 #%.txt"), str(work / "not-downloaded.bin")]
            file_publisher, _ = launch("file-publisher", "publish", "--origin", "files", "--", *paths)
            assert json.loads(run("capture-files").stdout) == paths
            stop(file_publisher)
            uri_source = work / "uris.txt"
            uri_source.write_bytes(b"# RFC URI-list comment\r\nfile://localhost/tmp/a%20b\r\nfile:///tmp/%E5%90%AB.txt\r\n")
            uri_publisher, _ = launch("uri-publisher", "publish-formats", "--format", "text/uri-list=" + str(uri_source))
            assert json.loads(run("capture-files").stdout) == ["/tmp/a b", "/tmp/含.txt"]
            stop(uri_publisher)
            cut_source = work / "cut.txt"
            cut_source.write_bytes(b"cut\nfile:///tmp/cut.txt")
            cut_publisher, _ = launch("cut-publisher", "publish-formats", "--format", "text/uri-list=" + str(uri_source),
                                     "--format", "x-special/gnome-copied-files=" + str(cut_source))
            assert b'"UNSUPPORTED_ACTION"' in run("capture-files", success=False).stderr
            stop(cut_publisher)
            invalid_lists = [b"https://example.com/data", b"file://another-host/tmp/x", b"file:relative",
                             b"file:///tmp/%GG", b"file:///tmp/%", b"file:///tmp/x%00y", b"file:///tmp/x\0y",
                             b"file:///tmp/x?query", b"file:///tmp/x#fragment"]
            for index, invalid in enumerate(invalid_lists):
                uri_source.write_bytes(invalid)
                invalid_publisher, _ = launch("invalid-uri-" + str(index), "publish-formats", "--format", "text/uri-list=" + str(uri_source))
                assert b'"INVALID_SOURCE_RESPONSE"' in run("capture-files", success=False).stderr
                stop(invalid_publisher)
            for index, oversized in enumerate((b"file:///tmp/x\n" * 4097, b"file:///" + b"x" * (1024 * 1024))):
                uri_source.write_bytes(oversized)
                budget_publisher, _ = launch("budget-list-" + str(index), "publish-formats", "--format", "text/uri-list=" + str(uri_source))
                assert b'"TOO_LARGE"' in run("capture-files", success=False).stderr
                stop(budget_publisher)
            oversized_format = work / "too-large.bin"
            with oversized_format.open("wb") as output_file:
                output_file.truncate(16 * 1024 * 1024 + 1)
            run("publish-formats", "--format", "image/png=" + str(oversized_format), success=False)
            late_source = work / "lazy.txt"
            late_source.write_bytes(b"not read before request")
            lazy_publisher, lazy_log = launch("lazy-failure", "publish-formats", "--format", "text/html=" + str(late_source),
                                             "--format", "text/plain;charset=utf-8=" + str(replacement))
            late_source.unlink()
            run("read", "--target", "text/html", success=False)
            assert any(row["event"] == "selection_error" for row in rows(lazy_log))
            assert run("read", "--target", "text/plain;charset=utf-8").stdout == b"replacement offer"
            stop(lazy_publisher)
            stop(watcher)

            service_checked = False
            if args.service_tool:
                service_tool = args.service_tool.resolve()
                source_file = work / "copied 含 空格.bin"
                source_bytes = random.Random(234).randbytes(600000)
                source_file.write_bytes(source_bytes)
                service_publisher, _ = launch("service-clipboard", "publish", "--origin", "service-input", "--", source_file)
                private = work / "service-private"
                private.mkdir(mode=0o700)
                socket = private / "files.sock"
                service_log = work / "file-service.jsonl"
                service_errors = work / "file-service.stderr"
                with service_errors.open("wb") as errors:
                    service = subprocess.Popen([str(service_tool), "serve", "--from-clipboard", "--socket", str(socket),
                                                "--log", str(service_log), "--source-chunk-size", "4093"],
                                               env=environment, stdout=subprocess.DEVNULL, stderr=errors)
                processes.append(service)
                wait_for(lambda: socket.exists() or service.poll() is not None)
                assert service.poll() is None, service_errors.read_text()
                inspected = subprocess.run([str(service_tool), "inspect", "--socket", str(socket)], env=environment,
                                           capture_output=True, check=True, timeout=10)
                offer = json.loads(inspected.stdout)
                assert len(offer["files"]) == 1 and offer["files"][0]["path"] == source_file.name, offer
                assert not any(row.get("operation") == "readRange" for row in rows(service_log)), \
                    "Clipboard capture / inspect read source file bodies"
                output = work / "service-range.bin"
                subprocess.run([str(service_tool), "read", "--socket", str(socket), "--path", source_file.name,
                                "--offset", "123", "--length", "270000", "--output", str(output), "--detectable"],
                               env=environment, capture_output=True, check=True, timeout=10)
                assert output.read_bytes() == source_bytes[123:270123]
                reads = [row for row in rows(service_log) if row.get("operation") == "readRange"]
                assert reads and all(row["returnedBytes"] <= 4093 for row in reads), reads
                stop(service)
                stop(service_publisher)
                assert not socket.exists(), "File service left its socket behind"
                service_checked = True

            # The reader must also accept an independent owner's INCR chunks.
            external_owner = Peer(library, display)
            peers.append(external_owner)
            library.XSetSelectionOwner(external_owner.display, external_owner.clipboard, external_owner.window, 0)
            library.XSync(external_owner.display, 0)
            external_output = work / "external.png"
            external_reader = subprocess.Popen([str(tool), "read", "--target", "image/png", "--timeout-ms", "5000",
                                               "--output", str(external_output)],
                                               env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            processes.append(external_reader)
            request = external_owner.wait(lambda event: event.type == 30).request
            library.XSelectInput(external_owner.display, request.requestor, 1 << 22)
            external_owner.put32(request.requestor, request.property, external_owner.atom("INCR"), [len(values["image/png"])])
            external_owner.notify(request, request.property)
            for offset in range(0, len(values["image/png"]) + 4093, 4093):
                external_owner.wait(lambda event: event.type == 28 and event.property.window == request.requestor
                                     and event.property.atom == request.property and event.property.state == 1)
                chunk = values["image/png"][offset:offset + 4093]
                external_owner.put8(request.requestor, request.property, request.target, chunk)
                if not chunk:
                    break
            stdout, stderr = external_reader.communicate(timeout=5)
            assert external_reader.returncode == 0 and external_output.read_bytes() == values["image/png"], stderr

            # Same-window ownership re-acquisition is a new offer as well.
            same_reader = subprocess.Popen([str(tool), "read", "--target", "image/png", "--timeout-ms", "3000"],
                                          env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            processes.append(same_reader)
            same_request = external_owner.wait(lambda event: event.type == 30).request
            external_owner.put32(same_request.requestor, same_request.property, external_owner.atom("INCR"), [100000])
            external_owner.notify(same_request, same_request.property)
            time.sleep(0.01)
            library.XSetSelectionOwner(external_owner.display, external_owner.clipboard, external_owner.window, 0)
            library.XSync(external_owner.display, 0)
            stdout, stderr = same_reader.communicate(timeout=5)
            assert same_reader.returncode != 0 and b'"OWNER_CHANGED"' in stderr, (stdout, stderr)

            # File capture's second independent connection must reject a changed
            # timestamp even when the owner window itself has not changed.
            capture = subprocess.Popen([str(tool), "capture-files", "--timeout-ms", "3000"], env=environment,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            processes.append(capture)
            for target, values32, type_name in (("TIMESTAMP", [100], "INTEGER"),
                                                ("TARGETS", [external_owner.atom("TIMESTAMP"), external_owner.atom("text/uri-list")], "ATOM"),
                                                ("TIMESTAMP", [101], "INTEGER")):
                captured_request = external_owner.wait(lambda event: event.type == 30).request
                assert captured_request.target == external_owner.atom(target)
                external_owner.put32(captured_request.requestor, captured_request.property,
                                     external_owner.atom(type_name), values32)
                external_owner.notify(captured_request, captured_request.property)
            stdout, stderr = capture.communicate(timeout=5)
            assert capture.returncode != 0 and not stdout and b'"OWNER_CHANGED"' in stderr, (stdout, stderr)

            # A deliberately stalled independent owner lets us replace selection
            # ownership after INCR acceptance but before any successful payload.
            owner = Peer(library, display)
            peers.append(owner)
            library.XSetSelectionOwner(owner.display, owner.clipboard, owner.window, 0)
            library.XSync(owner.display, 0)
            old_output = work / "must-not-overwrite.bin"
            old_output.write_bytes(b"previous verified output")
            reader = subprocess.Popen([str(tool), "read", "--target", "image/png", "--output", str(old_output),
                                       "--timeout-ms", "3000"], env=environment, stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE)
            processes.append(reader)
            request = owner.wait(lambda event: event.type == 30 and event.request.target == owner.atom("image/png")).request
            owner.put32(request.requestor, request.property, owner.atom("INCR"), [1000000])
            owner.notify(request, request.property)
            replacement_owner = Peer(library, display)
            peers.append(replacement_owner)
            library.XSetSelectionOwner(replacement_owner.display, replacement_owner.clipboard, replacement_owner.window, 0)
            library.XSync(replacement_owner.display, 0)
            stdout, stderr = reader.communicate(timeout=5)
            assert reader.returncode != 0 and b'"OWNER_CHANGED"' in stderr, (stdout, stderr)
            assert old_output.read_bytes() == b"previous verified output"
            print(json.dumps({"status": "passed", "display": display, "pngBytes": len(values["image/png"]),
                              "clipboardFileService": "passed" if service_checked else "not requested",
                              "checks": ["all-format-bytes", "lazy-providers", "INCR-read-write", "TARGETS", "TIMESTAMP",
                                         "unsupported", "property-none", "MULTIPLE", "INCR-timeout",
                                         "requestor-disappears", "INCR-concurrency-budget", "self-loop", "watch-once", "owner-replacement",
                                         "owner-changed-read", "same-owner-new-offer", "external-INCR-read", "atomic-output",
                                         "capture-file-URIs", "GNOME-cut-rejected", "invalid-file-lists", "file-list-budgets",
                                         "format-budget", "provider-error", "capture-same-owner-new-offer"]}, indent=2))
            return 0
        finally:
            os.close(read_fd)
            for process in reversed(processes[1:]):
                stop(process)
            for peer in peers:
                peer.close()
            stop(server)


if __name__ == "__main__":
    sys.exit(main())
