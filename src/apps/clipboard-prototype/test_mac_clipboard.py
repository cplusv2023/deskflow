#!/usr/bin/env python3
"""Isolated AppKit probe tests; never reads or changes the general pasteboard.

Usage: python3 test_mac_clipboard.py --probe /path/to/deskflow-clipboard-probe
The tests exercise the local AppKit boundary, not Finder or a network peer.
"""

import argparse
import base64
import contextlib
import json
import pathlib
import subprocess
import struct
import tempfile
import time
import uuid
import zlib


def png_pixels(data):
    """Validate CRCs/zlib and decode small 8-bit PNG fixtures without native codecs."""
    assert data.startswith(b"\x89PNG\r\n\x1a\n")
    position = 8
    compressed = bytearray()
    header = None
    while position < len(data):
        length = struct.unpack_from(">I", data, position)[0]
        kind = data[position + 4:position + 8]
        payload = data[position + 8:position + 8 + length]
        expected = struct.unpack_from(">I", data, position + 8 + length)[0]
        assert zlib.crc32(kind + payload) & 0xffffffff == expected, kind
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", payload)
        if kind == b"IDAT":
            compressed.extend(payload)
        position += 12 + length
        if kind == b"IEND":
            break
    assert position == len(data) and header
    width, height, depth, color, compression, filtering, interlace = header
    assert depth == 8 and color in (0, 2, 4, 6) and compression == filtering == interlace == 0
    channels = {0: 1, 2: 3, 4: 2, 6: 4}[color]
    raw = zlib.decompress(compressed)
    stride = width * channels
    assert len(raw) == height * (stride + 1)
    previous = bytearray(stride)
    rows = []
    for row_index in range(height):
        start = row_index * (stride + 1)
        method = raw[start]
        row = bytearray(raw[start + 1:start + 1 + stride])
        assert 0 <= method <= 4
        for index in range(stride):
            left = row[index - channels] if index >= channels else 0
            above = previous[index]
            upper_left = previous[index - channels] if index >= channels else 0
            if method == 1:
                prediction = left
            elif method == 2:
                prediction = above
            elif method == 3:
                prediction = (left + above) // 2
            elif method == 4:
                value = left + above - upper_left
                candidates = [left, above, upper_left]
                prediction = min(candidates, key=lambda candidate: abs(value - candidate))
            else:
                prediction = 0
            row[index] = (row[index] + prediction) & 255
        rows.append(bytes(row))
        previous = row
    return width, height, channels, b"".join(rows)


def rgb_tiff(width=2, height=2, orientation=1):
    """Valid little-endian TIFF; large images use streamed deflate-compressed rows."""
    pixels = bytes([255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 0])
    compression = 1
    if (width, height) != (2, 2):
        compression = 8
        encoder = zlib.compressobj()
        row = b"\x00" * (width * 3)
        pixels = b"".join(encoder.compress(row) for _ in range(height)) + encoder.flush()
    count = 11
    bits_offset = 8 + 2 + 12 * count + 4
    data_offset = bits_offset + 6
    tags = [
        (256, 4, 1, width), (257, 4, 1, height), (258, 3, 3, bits_offset),
        (259, 3, 1, compression), (262, 3, 1, 2), (273, 4, 1, data_offset),
        (274, 3, 1, orientation), (277, 3, 1, 3), (278, 4, 1, height),
        (279, 4, 1, len(pixels)), (284, 3, 1, 1),
    ]
    return (b"II\x2a\x00" + struct.pack("<I", 8) + struct.pack("<H", count) +
            b"".join(struct.pack("<HHII", *tag) for tag in tags) + struct.pack("<I", 0) +
            struct.pack("<HHH", 8, 8, 8) + pixels)


def records(path):
    if not path.exists():
        return []
    # Concurrent append can leave the last JSON line incomplete briefly.
    return [json.loads(line) for line in path.read_text().splitlines(keepends=True) if line.endswith("\n")]


def wait_for(predicate, process=None, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        if process is not None and process.poll() is not None:
            raise AssertionError(f"probe exited early: {process.returncode}")
        time.sleep(0.02)
    raise AssertionError("probe event timeout")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", required=True)
    parser.add_argument("--native-format-fixture")
    args = parser.parse_args()
    probe = str(pathlib.Path(args.probe).resolve())
    board = "org.deskflow.test." + str(uuid.uuid4())
    processes = []

    def invoke(*arguments, check=True):
        return subprocess.run(
            [probe, arguments[0], "--pasteboard", board, *arguments[1:]],
            check=check,
            capture_output=True,
            timeout=10,
        )

    def inspect(origin=""):
        arguments = ["inspect"]
        if origin:
            arguments += ["--origin", origin]
        return json.loads(invoke(*arguments).stdout)

    with tempfile.TemporaryDirectory(prefix="deskflow-mac-pasteboard-") as temporary:
        root = pathlib.Path(temporary)

        @contextlib.contextmanager
        def publisher(command, *arguments, origin="local-source"):
            log = root / (str(uuid.uuid4()) + ".jsonl")
            stderr = root / (str(uuid.uuid4()) + ".stderr")
            with stderr.open("wb") as error_stream:
                process = subprocess.Popen(
                    [probe, command, "--pasteboard", board, "--origin", origin, "--log", str(log), *arguments],
                    stdout=subprocess.DEVNULL,
                    stderr=error_stream,
                )
                processes.append(process)
                try:
                    wait_for(lambda: any(row["event"] == "published" for row in records(log)), process)
                    yield process, log
                finally:
                    if process.poll() is None:
                        process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)
                    assert process.returncode == 0, stderr.read_text()

        try:
            # Nonexistent paths demonstrate URL publication has no file existence requirement.
            paths = [root / "not-created 空格 #%.txt", root / "also-not-created" / "nested.bin"]
            with publisher("publish", *map(str, paths)) as (owner, _):
                state = inspect("local-source")
                assert state["stable"] and state["itemCount"] == 2, state
                assert state["selfOrigin"], state
                assert all("public.file-url" in item["types"] for item in state["items"]), state
                payload = invoke("read", "--target", "text/uri-list").stdout.decode()
                assert payload.splitlines() == [path.as_uri() for path in paths], payload

                watch_log = root / "watch.jsonl"
                watcher = subprocess.Popen(
                    [probe, "watch", "--pasteboard", board, "--origin", "local-source", "--log", str(watch_log),
                     "--poll-ms", "20"],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.PIPE,
                )
                processes.append(watcher)
                try:
                    wait_for(lambda: any(row["event"] == "selfChange" for row in records(watch_log)), watcher)
                    with publisher("publish", str(root / "remote.txt"), origin="remote-source"):
                        wait_for(lambda: any(row["event"] == "clipboardChanged" and row.get("origin") == "remote-source"
                                             for row in records(watch_log)), watcher)
                        owner.wait(timeout=5)
                        assert owner.returncode == 0
                finally:
                    # The events, not elapsed wall time, determine when this watcher
                    # has finished. Process startup can exceed 2 s under a core build.
                    if watcher.poll() is None:
                        watcher.terminate()
                    try:
                        watcher.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        watcher.kill()
                        watcher.wait(timeout=5)
                    assert watcher.returncode == 0, watcher.stderr.read()
            print("PASS: multiple native URLs, Unicode escaping, self-origin watch, ownership replacement")

            data = {
                "text/plain": "clipboard 跨平台\nsecond line\n".encode(),
                "text/html": "<p><strong>clipboard</strong> 跨平台</p>".encode(),
                "image/png": base64.b64decode(
                    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+ip1sAAAAASUVORK5CYII="
                ),
            }
            assert png_pixels(data["image/png"])[:2] == (1, 1)
            arguments = []
            for number, (target, content) in enumerate(data.items()):
                path = root / f"representation-{number}"
                path.write_bytes(content)
                arguments += ["--target", target, "--input", str(path)]
            with publisher("publish-data", *arguments) as (_, log):
                assert not any(row["event"].startswith("provideData") for row in records(log)), records(log)
                state = inspect("local-source")
                assert state["selfOrigin"] and state["itemCount"] == 1, state
                assert {"public.utf8-plain-text", "public.html", "public.png"} <= set(state["items"][0]["types"]), state
                assert not any(row["event"].startswith("provideData") for row in records(log)), records(log)
                for number, (target, content) in enumerate(data.items()):
                    output = root / f"read-{number}"
                    invoke("read", "--target", target, "--output", str(output))
                    assert output.read_bytes() == content, target
                delivered = [row for row in records(log) if row["event"] == "provideData"]
                assert len(delivered) == 3 and all(row["status"] == "OK" for row in delivered), delivered
                assert sum(row["returnedBytes"] for row in delivered) == sum(map(len, data.values()))
            print("PASS: lazy multi-format promises, metadata-only inspection, byte-exact plain/HTML/PNG reads")

            source = root / "mutable.txt"
            source.write_text("original")
            with publisher("publish-data", "--target", "text/plain", "--input", str(source)) as (_, log):
                source.write_text("changed and longer")
                result = invoke("read", "--target", "text/plain", check=False)
                assert result.returncode != 0, result
                assert any(row.get("status") == "SOURCE_CHANGED" for row in records(log)), records(log)
            print("PASS: changed representation input is rejected")

            source.write_bytes(b"")
            with publisher("publish-data", "--target", "text/plain", "--input", str(source)):
                assert invoke("read", "--target", "text/plain").stdout == b""
            print("PASS: empty representation is distinct from unavailable data")

            before = inspect()["changeCount"]
            invalid_commands = [
                ["publish", "relative.txt"],
                ["publish-data", "--target", "application/unknown", "--input", str(source)],
                ["publish-data", "--target", "text/plain", "--input", str(root)],
                ["publish-data", "--target", "text/plain"],
                ["publish-data", "--target", "text/plain", "--input", str(source),
                 "--target", "UTF8_STRING", "--input", str(source)],
            ]
            oversized = root / "oversized"
            with oversized.open("wb") as stream:
                stream.truncate(16 * 1024 * 1024 + 1)
            invalid_commands.append(["publish-data", "--target", "image/png", "--input", str(oversized)])
            for command in invalid_commands:
                result = invoke(*command, check=False)
                assert result.returncode == 2, (command, result)
                assert inspect()["changeCount"] == before, command
            print("PASS: input, duplicate-type, and byte-budget validation leaves pasteboard unchanged")

            if args.native_format_fixture:
                helper = str(pathlib.Path(args.native_format_fixture).resolve())

                def native(*representations):
                    arguments = [helper, board]
                    for number, (target, content) in enumerate(representations):
                        fixture = root / f"native-{number}"
                        fixture.write_bytes(content)
                        arguments += [target, str(fixture)]
                    subprocess.run(arguments, check=True, capture_output=True, timeout=10)

                rtf = br"{\rtf1\ansi\uc1 Plain {\b Bold} \u20013?\u25991?\par}"
                native(("public.rtf", rtf))
                converted = invoke("read", "--target", "text/html").stdout.decode("utf-8")
                assert "Bold" in converted and ("中文" in converted or "&#20013;" in converted), converted
                assert "bold" in converted.lower() or "<b>" in converted, converted
                native(("public.tiff", rgb_tiff()))
                converted = invoke("read", "--target", "image/png").stdout
                width, height, channels, pixels = png_pixels(converted)
                assert (width, height) == (2, 2) and channels in (3, 4)
                assert [pixels[index:index + 3] for index in range(0, len(pixels), channels)] == [
                    b"\xff\x00\x00", b"\x00\xff\x00", b"\x00\x00\xff", b"\xff\xff\x00"
                ], pixels
                native(("public.tiff", rgb_tiff(orientation=3)))
                _, _, channels, pixels = png_pixels(invoke("read", "--target", "image/png").stdout)
                assert pixels[:3] == b"\xff\xff\x00", pixels
                print("PASS: native RTF→HTML preserves bold/Unicode; TIFF→PNG preserves pixels and orientation")

                # Exact types take priority even if the optional native fallback is invalid.
                native(("public.html", data["text/html"]), ("public.rtf", b"invalid RTF"))
                assert invoke("read", "--target", "text/html").stdout == data["text/html"]
                native(("public.png", data["image/png"]), ("public.tiff", b"invalid TIFF"))
                assert invoke("read", "--target", "image/png").stdout == data["image/png"]

                bad_rtf = [
                    br"{\rtf1 {\pict\pngblip 00}}", br"{\rtf1 {\object\objdata 00}}",
                    br"{\rtf1 \bin1 x}", br"{\rtf1 {\field {\fldinst INCLUDEPICTURE x}}}",
                    b"{\\rtf1 " + b"{" * 129 + b"x" + b"}" * 129 + b"}",
                    b"{\\rtf1 " + b"x" * (1024 * 1024) + b"}",
                ]
                for rtf in bad_rtf:
                    native(("public.rtf", rtf))
                    result = invoke("read", "--target", "text/html", check=False)
                    assert result.returncode == 2 and not result.stdout, result
                for tiff, reason in (
                    (b"invalid TIFF", b"valid TIFF"),
                    (rgb_tiff(width=9000), b"dimensions"),
                    (rgb_tiff(width=4096, height=4096), b"pixel"),
                ):
                    native(("public.tiff", tiff))
                    result = invoke("read", "--target", "image/png", check=False)
                    assert result.returncode == 2 and not result.stdout, result
                    assert reason in result.stderr, result.stderr
                print("PASS: exact-type priority and malformed/embedded/oversized native format rejection")
        finally:
            for process in processes:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)
            invoke("release")


if __name__ == "__main__":
    main()
