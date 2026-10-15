#!/usr/bin/env python3
# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT
"""Two-process local-file RPC checks; never touch the desktop clipboard."""
import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import threading
import time


def frame(fields, body=b""):
    encoded = [value.encode() for value in fields]
    data = struct.pack("!I", len(fields)) + b"".join(struct.pack("!I", len(x)) + x for x in encoded)
    data += struct.pack("!I", len(body)) + body
    return struct.pack("!I", len(data)) + data


def exact(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise RuntimeError("incomplete frame")
        data += chunk
    return bytes(data)


def receive(sock):
    length, = struct.unpack("!I", exact(sock, 4))
    assert 8 <= length <= 1048576
    data = memoryview(exact(sock, length))
    count, = struct.unpack_from("!I", data)
    offset = 4
    fields = []
    for _ in range(count):
        size, = struct.unpack_from("!I", data, offset)
        offset += 4
        fields.append(bytes(data[offset:offset + size]).decode())
        offset += size
    size, = struct.unpack_from("!I", data, offset)
    offset += 4
    assert len(data) - offset == size
    return fields, bytes(data[offset:])


class Peer:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(3)
        self.sock.connect(str(path))
        self.sequence = 0

    def call(self, operation, *args):
        self.sequence += 1
        request = [operation, str(self.sequence), *map(str, args)]
        self.sock.sendall(frame(request))
        fields, body = receive(self.sock)
        assert fields[:2] == request[:2]
        return fields, body

    def close(self):
        self.sock.close()


def manifest(fields):
    assert fields[2] == "OK", fields
    epoch, offer, version = fields[3:6]
    roots = int(fields[8])
    count_position = 9 + roots
    count = int(fields[count_position])
    entries = {}
    for index in range(count):
        file = fields[count_position + 1 + 7 * index:count_position + 8 + 7 * index]
        entries[file[3]] = file
    return epoch, offer, version, entries


@contextlib.contextmanager
def server(tool, root, source, extra=()):
    path = root / "files.sock"
    log = root / "requests.jsonl"
    process = subprocess.Popen([tool, "serve", "--socket", str(path), "--source", str(source),
                                "--log", str(log), *extra], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 5
        while not path.exists():
            if process.poll() is not None:
                raise AssertionError(process.communicate())
            assert time.monotonic() < deadline, "server startup timeout"
            time.sleep(0.01)
        yield path, log, process
    finally:
        process.terminate()
        process.communicate(timeout=5)
        assert not path.exists(), "server socket leaked after stop"


def run(tool, *args, ok=True):
    result = subprocess.run([tool, *map(str, args)], capture_output=True, timeout=15)
    assert (result.returncode == 0) == ok, (result.returncode, result.stderr.decode(errors="replace"))
    return result


@contextlib.contextmanager
def fault_proxy(path, upstream_path, mode):
    """Inject once at the read response boundary, preserving other operations."""
    listener = socket.socket(socket.AF_UNIX)
    listener.bind(str(path))
    listener.listen(1)
    listener.settimeout(5)
    errors = []

    def forward():
        try:
            with listener.accept()[0] as downstream, socket.socket(socket.AF_UNIX) as upstream:
                upstream.settimeout(5)
                downstream.settimeout(5)
                upstream.connect(str(upstream_path))
                while True:
                    request, body = receive(downstream)
                    upstream.sendall(frame(request, body))
                    response, body = receive(upstream)
                    if request[0] == "hello" and mode.startswith("manifest-"):
                        first = 10 + int(response[8])
                        if mode == "manifest-name":
                            response[first + 2] = "../escape"
                        elif mode == "manifest-parent":
                            response[first + 1] = "missing-parent"
                        elif mode == "manifest-root":
                            response[9] = "missing-root"
                        downstream.sendall(frame(response, body))
                        return
                    if request[0] == "read":
                        if mode == "disconnect":
                            return
                        if mode == "truncated":
                            downstream.sendall(frame(response, body)[:17])
                            return
                        if mode == "zero":
                            body = b""
                            response[-1] = "0"
                        elif mode == "version":
                            response[6] = "unrequested-version"
                        elif mode == "correlation":
                            response[1] = "unrequested-id"
                        elif mode == "epoch":
                            response[3] = "expired-session"
                        downstream.sendall(frame(response, body))
                        return
                    downstream.sendall(frame(response, body))
        except Exception as error:
            errors.append(error)

    thread = threading.Thread(target=forward, daemon=True)
    thread.start()
    try:
        yield
    finally:
        listener.close()
        thread.join(6)
        path.unlink(missing_ok=True)
        assert not thread.is_alive(), "fault proxy did not terminate"
        assert not errors, errors


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", required=True)
    tool = parser.parse_args().tool
    with tempfile.TemporaryDirectory(prefix="deskflow-file-rpc-") as temporary:
        root = Path(temporary)
        source = root / "source"
        source.mkdir()
        payload = bytes((x + 41) % 251 for x in range(1024 * 1024 + 37))
        (source / "含 空格.bin").write_bytes(payload)
        (source / "empty").write_bytes(b"")
        (source / "folder").mkdir()
        (source / "folder" / "later.txt").write_bytes(b"old directory child\n")
        sparse = source / "large.bin"
        with sparse.open("wb") as output:
            output.truncate(4294967419)
            output.seek(4294967305)
            output.write(bytes(range(97)))
        with server(tool, root, source, ("--source-chunk-size", "4093")) as (path, log, _):
            offered = json.loads(run(tool, "inspect", "--socket", path).stdout)
            assert any(x["size"] == "4294967419" for x in offered["files"])
            assert not log.read_text(), "metadata read file bodies"
            destination = root / "result.bin"
            run(tool, "read", "--socket", path, "--path", "source/含 空格.bin", "--offset", 0,
                "--length", len(payload), "--output", destination, "--detectable")
            assert hashlib.sha256(destination.read_bytes()).digest() == hashlib.sha256(payload).digest()
            records = [json.loads(x) for x in log.read_text().splitlines()]
            assert len(records) > len(payload) // 4093
            assert max(x["returnedBytes"] for x in records) <= 4093
            assert all(isinstance(x["offset"], str) and x["status"] == "OK" for x in records)
            assert not records[0]["eof"] and records[-1]["eof"]
            large_output = root / "large-range"
            run(tool, "read", "--socket", path, "--path", "source/large.bin", "--offset", 4294967305,
                "--length", 97, "--output", large_output, "--detectable")
            assert large_output.read_bytes() == bytes(range(97))
            bad_output = root / "ungranted"
            failure = run(tool, "read", "--socket", path, "--path", "../secret", "--offset", 0,
                          "--length", 1, "--output", bad_output, "--detectable", ok=False)
            assert b"NOT_FOUND" in failure.stderr and not bad_output.exists()
            peer = Peer(path)
            try:
                epoch, offer, version, files = manifest(peer.call("hello")[0])
                lease = peer.call("acquire", epoch, offer, version, 30000)[0][3]
                file = files["source/folder/later.txt"]
                assert peer.call("open", epoch, lease, file[0], file[4], "immutable")[0][2] == "SOURCE_NOT_STABLE"
                new = manifest(peer.call("refresh")[0])
                assert new[1] != offer
                opened = peer.call("open", epoch, lease, file[0], file[4], "detectable")[0]
                assert opened[2] == "OK", opened
                handle = opened[3]
                fields, body = peer.call("read", epoch, lease, handle, 0, 100)
                assert fields[2] == "OK" and fields[-1] == "1" and body == b"old directory child\n"
                fields, body = peer.call("read", epoch, lease, handle, 18446744073709551615, 97)
                assert fields[2] == "OK" and fields[-1] == "1" and body == b""
                (source / "folder" / "later.txt").write_bytes(b"changed version\n")
                fields, body = peer.call("read", epoch, lease, handle, 0, 100)
                assert fields[2] == "SOURCE_CHANGED" and fields[-1] == "0" and body == b""
            finally:
                peer.close()
        with server(tool, root, source, ("--delay-ms", "100")) as (path, _, _):
            failed = root / "timed-out"
            failure = run(tool, "read", "--socket", path, "--path", "source/含 空格.bin", "--offset", 0,
                          "--length", 4096, "--output", failed, "--detectable", "--timeout-ms", 20, ok=False)
            assert b"TIMEOUT" in failure.stderr and not failed.exists(), failure.stderr
            assert not list(root.glob("timed-out.deskflow-*"))
            # A new connection receives a new session and cannot consume the late response.
            assert json.loads(run(tool, "inspect", "--socket", path).stdout)["sessionEpoch"]
        # A malformed peer must not make the listener or a later client unusable.
        with server(tool, root, source) as (path, _, _):
            bad = socket.socket(socket.AF_UNIX)
            bad.connect(str(path)); bad.sendall(struct.pack("!I", 1048577)); bad.close()
            assert json.loads(run(tool, "inspect", "--socket", path).stdout)["files"]
            for mode in ("manifest-name", "manifest-parent", "manifest-root"):
                with fault_proxy(root / "proxy.sock", path, mode):
                    failure = run(tool, "inspect", "--socket", root / "proxy.sock", ok=False)
                assert b"INVALID_SOURCE_RESPONSE" in failure.stderr, (mode, failure.stderr)
            for mode in ("disconnect", "truncated", "zero", "version", "correlation", "epoch"):
                output = root / ("fault-" + mode)
                with fault_proxy(root / "proxy.sock", path, mode):
                    failure = run(tool, "read", "--socket", root / "proxy.sock", "--path", "source/含 空格.bin",
                                  "--offset", 0, "--length", 4096, "--output", output, "--detectable", ok=False)
                expected = b"DISCONNECTED" if mode in ("disconnect", "truncated") else b"INVALID_SOURCE_RESPONSE"
                assert expected in failure.stderr, (mode, failure.stderr)
                assert not output.exists() and not list(root.glob(output.name + ".deskflow-*"))
        with server(tool, root, source) as (path, _, _):
            queued = []
            try:
                for _ in range(12):
                    sock = socket.socket(socket.AF_UNIX)
                    sock.settimeout(0.05)
                    queued.append(sock)
                    try:
                        sock.connect(str(path))
                    except (socket.timeout, BlockingIOError):
                        break
                before = time.monotonic()
                failure = run(tool, "inspect", "--socket", path, "--timeout-ms", 20, ok=False)
                assert b"TIMEOUT" in failure.stderr and time.monotonic() - before < 2, failure.stderr
            finally:
                for sock in queued:
                    sock.close()
    print("PASS: local RPC metadata, short aggregation, >4GiB range, whole-offer retention, version errors, timeout, isolation")


if __name__ == "__main__":
    main()
