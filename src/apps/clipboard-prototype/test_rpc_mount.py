#!/usr/bin/env python3
# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT
"""AF_UNIX + FUSE receiver checks in private test directories; no desktop clipboard."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import contextlib
import errno
import hashlib
import json
import os
from pathlib import Path
import select
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import time


def records(path):
    if not path.exists():
        return []
    def unique_fields(pairs):
        result = {}
        for key, value in pairs:
            assert key not in result, f"duplicate trace field {key}"
            result[key] = value
        return result
    return [json.loads(line, object_pairs_hook=unique_fields)
            for line in path.read_text().splitlines() if line.strip()]


def packet(connection):
    def exact(size):
        data = bytearray()
        while len(data) < size:
            chunk = connection.recv(size - len(data))
            if not chunk:
                raise EOFError("peer closed the proxy stream")
            data += chunk
        return bytes(data)
    prefix = exact(4)
    length, = struct.unpack("!I", prefix)
    assert 8 <= length <= 1048576
    return prefix + exact(length)


@contextlib.contextmanager
def stall_response(root, backend, operation, resume_after=None):
    """Forward real RPC metadata; hold one complete response until cancellation."""
    path = root / "proxy.sock"
    listener = socket.socket(socket.AF_UNIX)
    listener.bind(str(path))
    listener.listen(1)
    held = threading.Event()
    stopped = threading.Event()
    connections = []
    failures = []

    def forward():
        try:
            front, _ = listener.accept()
            connections.append(front)
            back = socket.socket(socket.AF_UNIX)
            connections.append(back)
            back.connect(str(backend))
            triggered = False
            while not stopped.is_set():
                request = packet(front)
                # Prefix, field count, first field length, first field bytes.
                first_length, = struct.unpack_from("!I", request, 8)
                requested_operation = request[12:12 + first_length].decode()
                back.sendall(request)
                response = packet(back)
                if requested_operation == operation and not triggered:
                    triggered = True
                    held.set()
                    if resume_after is not None:
                        if stopped.wait(resume_after):
                            return
                        front.sendall(response)
                        continue
                    while not stopped.is_set():
                        if select.select([front], [], [], 0.02)[0] and not front.recv(1):
                            return
                    return
                front.sendall(response)
        except (EOFError, OSError):
            pass
        except BaseException as error:
            failures.append(error)
        finally:
            for connection in connections:
                connection.close()

    worker = threading.Thread(target=forward)
    worker.start()
    try:
        yield path, held
    finally:
        stopped.set()
        for connection in list(connections):
            with contextlib.suppress(OSError):
                connection.shutdown(socket.SHUT_RDWR)
        listener.close()
        worker.join(timeout=2)
        assert not worker.is_alive(), "test response proxy leaked"
        assert not failures, failures
        path.unlink(missing_ok=True)


def wait_for(predicate, process, timeout=5):
    deadline = time.monotonic() + timeout
    while not predicate():
        if process.poll() is not None:
            raise AssertionError((process.returncode, process.communicate()))
        assert time.monotonic() < deadline, "startup/evidence timeout"
        time.sleep(0.01)


@contextlib.contextmanager
def serve(tool, root, source, extra=()):
    socket = root / "files.sock"
    log = root / "source.jsonl"
    process = subprocess.Popen([tool, "serve", "--socket", str(socket), "--source", str(source),
                                "--log", str(log), *extra], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        wait_for(socket.exists, process)
        yield socket, log, process
    finally:
        if process.poll() is None:
            process.terminate()
        process.communicate(timeout=5)
        assert not socket.exists(), "server left its socket behind"


@contextlib.contextmanager
def mount(tool, root, socket, extra=()):
    point = root / "mount"
    point.mkdir()
    log = root / "receiver.jsonl"
    process = subprocess.Popen([tool, "--socket", str(socket), "--mount", str(point), "--log", str(log),
                                *extra], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        wait_for(lambda: os.path.ismount(point) and any(r["operation"] == "ready" for r in records(log)), process)
        yield point, log, process
    finally:
        if os.path.ismount(point):
            subprocess.run(["fusermount3", "-u", str(point)], check=True, capture_output=True, timeout=5)
        if process.poll() is None:
            try:
                process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.communicate(timeout=5)
        else:
            process.communicate(timeout=5)
        assert not os.path.ismount(point), "FUSE mount leaked after exit"


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def read_error(path, length=4096):
    try:
        with path.open("rb", buffering=0) as stream:
            return os.pread(stream.fileno(), length, 0)
    except OSError as error:
        return error


def run_success(file_tool, service_tool, parent):
    root = parent / "success"
    root.mkdir(mode=0o700)
    source = root / "source"
    source.mkdir()
    payload = bytes((index + 41) % 251 for index in range(1024 * 1024 + 37))
    (source / "含 空格.bin").write_bytes(payload)
    (source / "empty.txt").write_bytes(b"")
    (source / "folder").mkdir()
    (source / "folder" / "empty dir").mkdir()
    (source / "folder" / "later.txt").write_bytes(b"unopened child retained by whole-offer lease\n")
    (source / "change.txt").write_bytes(b"original version\n")
    with (source / "large.bin").open("wb") as stream:
        stream.truncate(4294967419)
        stream.seek(4294967305)
        stream.write(bytes(range(97)))
    with serve(service_tool, root, source, ("--source-chunk-size", "4093")) as (socket, log, _):
        with mount(file_tool, root, socket, ("--detectable", "--lease-ttl-ms", "600")) as (point, receiver, process):
            projected = point / "source"
            for base, directories, files in os.walk(projected):
                for name in directories + files:
                    (Path(base) / name).stat()
            assert not records(log), "publishing/listing metadata read file bodies"
            assert (projected / "large.bin").stat().st_size == 4294967419
            # No handles remain open. The later descendant must outlive initial TTL.
            time.sleep(1.35)
            assert sum(x["operation"] == "lease_renewed" for x in records(receiver)) >= 2
            assert (projected / "folder" / "later.txt").read_bytes() == (source / "folder" / "later.txt").read_bytes()
            with (projected / "含 空格.bin").open("rb", buffering=0) as stream:
                assert os.pread(stream.fileno(), 10000, 5151) == payload[5151:15151]
            output = root / "copied.bin"
            shutil.copyfile(projected / "含 空格.bin", output)
            assert digest(output) == hashlib.sha256(payload).hexdigest()
            assert output.stat().st_mode & 0o200, "read-only source should not force a read-only destination"
            shutil.copytree(projected / "folder", root / "copied-folder")
            assert (root / "copied-folder" / "empty dir").is_dir()
            assert (root / "copied-folder" / "later.txt").read_bytes() == (source / "folder" / "later.txt").read_bytes()
            assert (projected / "empty.txt").read_bytes() == b""
            with (projected / "large.bin").open("rb", buffering=0) as stream:
                assert os.pread(stream.fileno(), 97, 4294967305) == bytes(range(97))
                assert os.pread(stream.fileno(), 20, 4294967419) == b""

            def concurrent_range(index):
                with (projected / "含 空格.bin").open("rb", buffering=0) as stream:
                    offset = 19001 * index
                    return os.pread(stream.fileno(), 8192, offset) == payload[offset:offset + 8192]

            with ThreadPoolExecutor(max_workers=4) as workers:
                assert all(workers.map(concurrent_range, range(8))), "concurrent FUSE calls were not serialized safely"
            with (projected / "change.txt").open("rb", buffering=0) as stream:
                (source / "change.txt").write_bytes(b"changed and longer version\n")
                try:
                    os.pread(stream.fileno(), 4096, 0)
                except OSError as error:
                    assert error.errno == errno.ESTALE, error
                else:
                    raise AssertionError("source mutation did not fail the pinned-version read")
            memory = [line for line in Path(f"/proc/{process.pid}/status").read_text().splitlines()
                      if line.startswith(("VmRSS:", "VmHWM:"))]
            reads = records(log)
            assert any(r["status"] == "SOURCE_CHANGED" for r in reads)
            assert all(isinstance(r["offset"], str) and r["length"] <= 262144 and r["returnedBytes"] <= 4093
                       for r in reads)
            assert any(r["status"] == "OK" and r["returnedBytes"] == 4093 and not r["eof"] for r in reads)
            assert not list(root.glob("*.deskflow-*")), "unexpected complete intermediate file"
        final = records(receiver)
        assert final and all(r["offerId"] and r["offerId"] != "mock-offer-1" for r in final), final
        assert any(r["operation"] == "lease_released" and r["state"] == "CLOSED" for r in final), final[-5:]
        assert final[-2]["operation"] == "rpc_shutdown" and final[-2]["status"] == "OK", final[-5:]
        return memory


def run_immutable(file_tool, service_tool, parent):
    root = parent / "immutable"
    root.mkdir(mode=0o700)
    source = root / "plain.txt"
    source.write_bytes(b"mutable local file")
    with serve(service_tool, root, source) as (socket, log, _):
        with mount(file_tool, root, socket) as (point, receiver, _):
            failure = read_error(point / "plain.txt")
            assert isinstance(failure, OSError) and failure.errno == errno.EOPNOTSUPP, failure
            assert not records(log), "immutable refusal still read source bytes"
            assert any(r["operation"] == "rpc_open" and r["status"] == "SOURCE_NOT_STABLE" for r in records(receiver))


def run_timeout(file_tool, service_tool, parent):
    root = parent / "timeout"
    root.mkdir(mode=0o700)
    source = root / "plain.txt"
    source.write_bytes(b"slow local file")
    with serve(service_tool, root, source, ("--delay-ms", "1000")) as (socket, _, _):
        with mount(file_tool, root, socket, ("--detectable", "--timeout-ms", "50")) as (point, _, _):
            start = time.monotonic()
            failure = read_error(point / "plain.txt")
            assert isinstance(failure, OSError) and failure.errno == errno.ETIMEDOUT, failure
            assert time.monotonic() - start < 0.8, "timeout did not bound the waiting read"


def run_disconnect_and_cancel(file_tool, service_tool, parent, cancel):
    root = parent / ("cancel" if cancel else "disconnect")
    root.mkdir(mode=0o700)
    source = root / "plain.txt"
    source.write_bytes(b"pending local file")
    with serve(service_tool, root, source, ("--delay-ms", "2000")) as (socket, _, server):
        with mount(file_tool, root, socket, ("--detectable", "--timeout-ms", "5000")) as (point, receiver, process):
            with ThreadPoolExecutor(max_workers=1) as workers:
                pending = workers.submit(read_error, point / "plain.txt")
                wait_for(lambda: any(r["operation"] == "rpc_open" for r in records(receiver)), process)
                time.sleep(0.08)
                start = time.monotonic()
                if cancel:
                    process.terminate()  # Session-exit monitor must cancel the blocked client.
                else:
                    server.terminate()
                    server.communicate(timeout=2)
                failure = pending.result(timeout=2)
                assert isinstance(failure, OSError), failure
                if cancel:
                    assert failure.errno in (errno.EINTR, errno.ENOTCONN, errno.EIO), failure
                    process.communicate(timeout=2)
                else:
                    assert failure.errno == errno.ENOTCONN, failure
                assert time.monotonic() - start < 1.5, "pending range did not wake promptly"


def run_metadata_phase_cancellation(file_tool, service_tool, parent, operation):
    root = parent / f"blocked-{operation}"
    root.mkdir(mode=0o700)
    source = root / "plain.txt"
    source.write_bytes(b"metadata-only cancellation; do not read these bytes")
    with serve(service_tool, root, source) as (backend, source_log, _):
        with stall_response(root, backend, operation) as (front, held):
            with mount(file_tool, root, front, ("--detectable", "--timeout-ms", "600000",
                                               "--lease-ttl-ms", "300")) as (point, _, process):
                def only_open_and_close():
                    try:
                        descriptor = os.open(point / "plain.txt", os.O_RDONLY)
                        os.close(descriptor)
                    except OSError:
                        pass

                with ThreadPoolExecutor(max_workers=1) as workers:
                    pending = None
                    if operation in ("open", "close"):
                        pending = workers.submit(only_open_and_close)
                    if operation != "release":
                        assert held.wait(timeout=3), f"did not block {operation} response"
                    start = time.monotonic()
                    process.terminate()
                    if operation == "release":
                        assert held.wait(timeout=2), "shutdown never attempted lease release"
                    process.communicate(timeout=2)
                    if pending:
                        pending.result(timeout=1)
                    elapsed = time.monotonic() - start
                    assert elapsed < 1.5, (operation, elapsed)
                    assert not records(source_log), f"{operation} cancellation unexpectedly read a body"
                    return round(elapsed * 1000, 1)


def run_normal_renew_shutdown(file_tool, service_tool, parent):
    root = parent / "normal-renew"
    root.mkdir(mode=0o700)
    source = root / "plain.txt"
    source.write_bytes(b"renew is in flight; grace should still allow release")
    with serve(service_tool, root, source) as (backend, source_log, _):
        with stall_response(root, backend, "renew", resume_after=0.08) as (front, held):
            with mount(file_tool, root, front, ("--detectable", "--timeout-ms", "600000",
                                               "--lease-ttl-ms", "300")) as (_, receiver, process):
                assert held.wait(timeout=3), "no renewal in flight"
                process.terminate()
                process.communicate(timeout=2)
                final = records(receiver)
                assert any(r["operation"] == "lease_released" and r["state"] == "CLOSED" for r in final), final
                assert any(r["operation"] == "rpc_shutdown" and r["status"] == "OK" for r in final), final
                assert not records(source_log), "graceful renewal shutdown read file bytes"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--file-tool", required=True)
    parser.add_argument("--service-tool", required=True)
    args = parser.parse_args()
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    if not runtime or not Path(runtime).is_dir():
        runtime = "/tmp"
    with tempfile.TemporaryDirectory(prefix="deskflow-rpc-mount-", dir=runtime) as temporary:
        parent = Path(temporary)
        memory = run_success(args.file_tool, args.service_tool, parent)
        run_immutable(args.file_tool, args.service_tool, parent)
        run_timeout(args.file_tool, args.service_tool, parent)
        run_disconnect_and_cancel(args.file_tool, args.service_tool, parent, False)
        run_disconnect_and_cancel(args.file_tool, args.service_tool, parent, True)
        shutdown_bounds = {}
        for operation in ("open", "renew", "close", "release"):
            shutdown_bounds[operation] = run_metadata_phase_cancellation(
                args.file_tool, args.service_tool, parent, operation)
        run_normal_renew_shutdown(args.file_tool, args.service_tool, parent)
    print(json.dumps({"rpc_mount": "passed", "memory": memory,
                      "blocked_phase_shutdown_ms": shutdown_bounds,
                      "checks": ["metadata only", "SHA-256", "positive short responses", "64-bit range",
                                 "idle whole-offer renewal", "directory copy", "immutable refusal", "source change",
                                 "concurrent calls", "timeout", "disconnect", "SIGTERM cancellation",
                                 "bounded open/renew/close/release shutdown", "in-flight renew graceful release",
                                 "source-specific trace identity"]}, indent=2))


if __name__ == "__main__":
    main()
