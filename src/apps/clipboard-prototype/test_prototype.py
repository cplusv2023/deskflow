#!/usr/bin/env python3
# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT
"""Exercise generated source ranges, FUSE mounts, and X11 clipboard conversions.

Mount and clipboard tests are explicit opt-ins because they use the current desktop.
"""

import argparse
import concurrent.futures
import errno
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import tempfile
import time
from urllib.parse import quote


def run(*args):
    return subprocess.run([str(arg) for arg in args], check=True, capture_output=True, timeout=30)


def wait_for(predicate, process, stderr, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        if process.poll() is not None:
            raise AssertionError(f"Prototype exited {process.returncode}: {stderr.read_text()}")
        time.sleep(0.05)
    raise AssertionError(f"Prototype not ready: {stderr.read_text()}")


def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def pattern(offset, size, seed):
    return bytes((index % 251 + seed) % 251 for index in range(offset, offset + size))


def records(path):
    if not path.exists():
        return []
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def source_test(tool, work):
    description = json.loads(run(tool, "--describe").stdout)
    assert description, "Missing fixture description"
    manifest = json.loads(Path(__file__).with_name("fixtures-v1.json").read_text())
    described = {entry["id"]: entry for entry in description["files"]}
    for entry in manifest["files"]:
        actual = described[entry["fileId"]]
        assert actual["path"] == entry["path"]
        assert actual["size"] == int(entry["size"])
        if entry["generator"] == "literal-utf8":
            assert actual["literal"] == entry["literal"]
        else:
            assert actual["seed"] == entry["seed"]
    for index, vector in enumerate(manifest["rangeVectors"]):
        output = work / f"shared-range-{index}.bin"
        run(tool, "--read", vector["fileId"], "--offset", vector["offset"],
            "--length", vector["length"], "--source-chunk-size", 4093, "--output", output)
        actual = output.read_bytes()
        assert actual == bytes.fromhex(vector["bytesHex"]), vector
        assert len(actual) == vector["returnedBytes"]
        assert hashlib.sha256(actual).hexdigest() == vector["sha256"]
    cases = [
        ("payload", 0, 17, 41),
        ("payload", 250, 270000, 41),
        ("unicode", 31, 100, 17),
        ("large", (1 << 32) + 9, 97, 97),
    ]
    for file_id, offset, length, seed in cases:
        output = work / f"{file_id}-{offset}.bin"
        run(tool, "--read", file_id, "--offset", offset, "--length", length,
            "--source-chunk-size", 4093, "--output", output)
        assert output.read_bytes() == pattern(offset, length, seed), (file_id, offset)
    output = work / "empty.bin"
    run(tool, "--read", "empty", "--offset", 0, "--length", 8192, "--output", output)
    assert output.read_bytes() == b""
    run(tool, "--read", "large", "--offset", (1 << 32) + 123, "--length", 17, "--output", output)
    assert output.read_bytes() == b"", "Read at EOF must be empty"
    invalid = subprocess.run([str(tool), "--read", "missing", "--offset", "0", "--length", "1",
                              "--output", str(output)], capture_output=True, timeout=10)
    assert invalid.returncode != 0, "Unknown source ID was accepted"
    return {"source_ranges": "passed", "shared_range_vectors": len(manifest["rangeVectors"]),
            "description": description}


def mount_test(tool, work):
    mount = work / "mount"
    mount.mkdir()
    log = work / "mount.jsonl"
    stderr = work / "mount.stderr"
    with stderr.open("w") as error_stream:
        process = subprocess.Popen([str(tool), "--mount", str(mount), "--log", str(log),
                                    "--source-chunk-size", "4093"],
                                   stdout=error_stream, stderr=error_stream)
        try:
            wait_for(lambda: os.path.ismount(mount), process, stderr)
            assert (mount / "large.bin").stat().st_size == (1 << 32) + 123
            assert (mount / "empty.txt").stat().st_size == 0
            list(mount.iterdir())
            list((mount / "nested folder").iterdir())
            assert not any(row.get("operation") == "read_range" for row in records(log)), \
                "Metadata queries read file contents"

            def check_range(spec):
                relative, offset, count, seed = spec
                descriptor = os.open(mount / relative, os.O_RDONLY)
                try:
                    data = os.pread(descriptor, count, offset)
                    assert data == pattern(offset, count, seed), spec
                finally:
                    os.close(descriptor)

            cases = [
                ("nested folder/payload.bin", 0, 65537, 41),
                ("nested folder/payload.bin", 262143, 65537, 41),
                ("nested folder/含 空格.txt", 7, 100, 17),
                ("large.bin", (1 << 32) + 9, 97, 97),
            ]
            with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
                list(pool.map(check_range, cases))
            assert (mount / "empty.txt").read_bytes() == b""
            fd = os.open(mount / "large.bin", os.O_RDONLY)
            try:
                assert os.pread(fd, 4096, (1 << 32) + 123) == b""
                assert os.pread(fd, 8192, (1 << 32) + 100) == pattern((1 << 32) + 100, 23, 97)
            finally:
                os.close(fd)
            try:
                fd = os.open(mount / "hello.txt", os.O_WRONLY)
            except OSError as error:
                assert error.errno in (errno.EROFS, errno.EACCES, errno.EPERM)
            else:
                os.close(fd)
                raise AssertionError("Virtual source allowed writes")
            requests = [row for row in records(log) if row.get("operation") == "read_range"]
            assert requests, "No source range requests recorded"
            assert max(row["length"] for row in requests) <= 256 * 1024
            assert max(row["activeRequests"] for row in requests) <= 8
            assert max(row["inFlightBytes"] for row in requests) <= 8 * 256 * 1024
            if platform.system() == "Darwin":
                memory = ["rssKiB: " + run("/bin/ps", "-o", "rss=", "-p", process.pid).stdout.decode().strip()]
            else:
                status = Path(f"/proc/{process.pid}/status").read_text()
                memory = [line for line in status.splitlines() if line.startswith(("VmRSS:", "VmHWM:"))]
            return {"mount_ranges": "passed", "range_request_count": len(requests),
                    "memory": memory}
        finally:
            stop(process)
            if os.path.ismount(mount):
                if platform.system() == "Darwin":
                    run("/sbin/umount", mount)
                else:
                    run("fusermount3", "-u", mount)
            assert not os.path.ismount(mount), "Prototype left a mounted filesystem"


def clipboard_test(tool, work):
    paths = [str(work / "含 空格.txt"), str(work / "#percent%.bin")]
    log = work / "clipboard.jsonl"
    stderr = work / "clipboard.stderr"
    with stderr.open("w") as stream:
        process = subprocess.Popen([str(tool), "publish", "--log", str(log), "--", *paths],
                                   stdout=stream, stderr=stream)
        try:
            wait_for(lambda: log.exists() and '"ready"' in log.read_text(), process, stderr)
            uris = ["file://" + quote(path, safe="/") for path in paths]
            for mime in ("x-special/gnome-copied-files", "application/x-gnome-copied-files"):
                actual = run(tool, "read", "--target", mime).stdout
                assert actual == ("copy\n" + "\n".join(uris)).encode(), (mime, actual)
            assert run(tool, "read", "--target", "text/uri-list").stdout == \
                ("\r\n".join(uris) + "\r\n").encode()
            unsupported = subprocess.run([str(tool), "read", "--target", "application/x-not-offered"],
                                         capture_output=True, timeout=10)
            assert unsupported.returncode != 0, "Unsupported target unexpectedly succeeded"
            return {"clipboard_formats": "passed", "published_paths": paths}
        finally:
            stop(process)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--file-tool", type=Path)
    parser.add_argument("--clipboard-tool", type=Path)
    parser.add_argument("--source", action="store_true")
    parser.add_argument("--mount", action="store_true")
    parser.add_argument("--clipboard", action="store_true")
    args = parser.parse_args()
    runtime = os.environ.get("XDG_RUNTIME_DIR", tempfile.gettempdir())
    with tempfile.TemporaryDirectory(prefix="deskflow-prototype-test-", dir=runtime) as directory:
        work = Path(directory)
        report = {}
        if args.source:
            report.update(source_test(args.file_tool.resolve(), work))
        if args.mount:
            report.update(mount_test(args.file_tool.resolve(), work))
        if args.clipboard:
            report.update(clipboard_test(args.clipboard_tool.resolve(), work))
        assert report, "Select at least one test"
        print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
