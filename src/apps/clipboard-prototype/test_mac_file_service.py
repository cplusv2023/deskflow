#!/usr/bin/env python3
"""Mac file-source boundary tests using independent processes and a named pasteboard.

This is a local stdin/stdout harness integration test, not network or Finder proof.
"""

import argparse
import base64
import contextlib
import json
import pathlib
import selectors
import subprocess
import tempfile
import time
import uuid


class ServiceClient:
    def __init__(self, executable, *arguments):
        self.process = subprocess.Popen(
            [executable, "--consistency", "detectable", *arguments],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)
        self.counter = 0
        self.ready = self.receive()
        assert self.ready["status"] == "OK" and self.ready["event"] == "ready", self.ready
        self.epoch = self.ready["sessionEpoch"]

    def receive(self):
        if not self.selector.select(timeout=10):
            raise AssertionError("service response timeout")
        line = self.process.stdout.readline()
        if not line:
            raise AssertionError(f"service exited: {self.process.stderr.read().decode()}")
        return json.loads(line)

    def call(self, operation, expected="OK", **fields):
        self.counter += 1
        request = {"operation": operation, "sessionEpoch": self.epoch, "requestId": f"request-{self.counter}", **fields}
        self.process.stdin.write(json.dumps(request).encode() + b"\n")
        self.process.stdin.flush()
        response = self.receive()
        assert response["requestId"] == request["requestId"], response
        assert response["sessionEpoch"] == request["sessionEpoch"], response
        assert response["operation"] == operation and response["status"] == expected, response
        if operation == "read" and expected != "OK":
            assert response["bytesBase64"] == "" and response["returnedBytes"] == 0 and not response["eof"], response
        return response

    def raw(self, payload):
        self.process.stdin.write(payload + b"\n")
        self.process.stdin.flush()
        return self.receive()

    def close(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        assert self.process.returncode == 0, self.process.stderr.read().decode()
        self.selector.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--service", required=True)
    parser.add_argument("--probe", required=True)
    args = parser.parse_args()
    service = str(pathlib.Path(args.service).resolve())
    probe = str(pathlib.Path(args.probe).resolve())
    board = "org.deskflow.service-test." + str(uuid.uuid4())

    with tempfile.TemporaryDirectory(prefix="deskflow-mac-source-") as temporary:
        root = pathlib.Path(temporary)
        folder = root / "selected 目录"
        folder.mkdir()
        payload = bytes(index % 251 for index in range(262144 + 97))
        (folder / "first.bin").write_bytes(payload)
        later = folder / "later.txt"
        later.write_bytes(b"old offer later child")
        sparse = root / "sparse.bin"
        sparse_size = 2**32 + 124
        with sparse.open("wb") as stream:
            stream.truncate(sparse_size)
            stream.seek(2**32 + 98)
            stream.write(b"tail-after-four-gigabytes!")
        replacement = root / "replacement.txt"
        replacement.write_bytes(b"offer B")
        owners = []

        @contextlib.contextmanager
        def publish(*paths):
            log = root / (str(uuid.uuid4()) + ".jsonl")
            process = subprocess.Popen(
                [probe, "publish", "--pasteboard", board, "--log", str(log), *map(str, paths)],
                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
            )
            owners.append(process)
            deadline = time.monotonic() + 5
            while True:
                if log.exists() and '"published"' in log.read_text():
                    break
                if process.poll() is not None or time.monotonic() >= deadline:
                    raise AssertionError("clipboard publisher did not become ready")
                time.sleep(0.02)
            try:
                yield
            finally:
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=5)
                assert process.returncode == 0, process.stderr.read().decode()

        client = None
        try:
            with publish(folder, sparse):
                client = ServiceClient(service, "--pasteboard", board)
                manifest = client.ready
                generation = json.loads(subprocess.run(
                    [probe, "inspect", "--pasteboard", board], check=True, capture_output=True, timeout=5
                ).stdout)["changeCount"]
                assert manifest["sourceChangeCount"] == generation
                assert str(root) not in json.dumps(manifest), manifest
                assert not manifest["immutableVersion"] and manifest["consistency"] == "detectable"
                entries = {entry["name"]: entry for entry in manifest["entries"]}
                first = entries["first.bin"]
                late = entries["later.txt"]
                large = entries["sparse.bin"]
                directory = entries[folder.name]
                assert large["size"] == str(sparse_size)
                lease = client.call("acquire", offerId=manifest["offerId"], offerVersion=manifest["offerVersion"])["leaseId"]
                client.call("renew", leaseId=lease)
                page = client.call("list", leaseId=lease, fileId=directory["fileId"], version=directory["version"], pageSize=1)
                assert len(page["entries"]) == 1 and page["nextCursor"] != 0, page
                page2 = client.call("list", leaseId=lease, fileId=directory["fileId"], version=directory["version"],
                                    cursor=page["nextCursor"], pageSize=1)
                assert len(page2["entries"]) == 1 and page2["nextCursor"] == 0, page2
                client.call("open", expected="SOURCE_NOT_STABLE", leaseId=lease, fileId=first["fileId"],
                            version=first["version"], consistency="immutable")
                client.call("open", expected="VERSION_MISMATCH", leaseId=lease, fileId=first["fileId"], version="wrong")
                opened = client.call("open", leaseId=lease, fileId=first["fileId"], version=first["version"])
                handle = opened["handleId"]
                for offset, length in [(0, 262144), (262140, 101), (len(payload), 19), (2**64 - 1, 5)]:
                    result = client.call("read", leaseId=lease, handleId=handle, offset=str(offset), length=length)
                    expected = payload[offset:offset + length]
                    assert base64.b64decode(result["bytesBase64"]) == expected
                    assert result["returnedBytes"] == len(expected) and result["eof"] == (offset + len(expected) >= len(payload))
                    assert result["offerId"] == manifest["offerId"] and result["fileId"] == first["fileId"]
                    assert result["version"] == first["version"] and result["offset"] == str(offset)
                big_handle = client.call("open", leaseId=lease, fileId=large["fileId"], version=large["version"])["handleId"]
                result = client.call("read", leaseId=lease, handleId=big_handle, offset=str(2**32 + 90), length=64)
                assert base64.b64decode(result["bytesBase64"]) == b"\x00" * 8 + b"tail-after-four-gigabytes!"
                assert result["eof"]
                client.call("close", handleId=big_handle)
                print("PASS: named pasteboard file capture, paged directory, range reads, >4 GiB and UINT64_MAX offsets")

                for extra, status in [
                    ({"length": 0}, "INVALID_ARGUMENT"),
                    ({"length": 262145}, "INVALID_ARGUMENT"),
                    ({"offset": 0}, "INVALID_ARGUMENT"),
                    ({"offset": "18446744073709551616"}, "INVALID_ARGUMENT"),
                    ({"cancelled": True}, "CANCELLED"),
                    ({"timeoutMs": 0}, "TIMEOUT"),
                    ({"sessionEpoch": "stale-session"}, "STALE_SESSION"),
                ]:
                    fields = {"leaseId": lease, "handleId": handle, "offset": "0", "length": 1, **extra}
                    client.call("read", expected=status, **fields)
                assert client.raw(b"[]")["status"] == "INVALID_ARGUMENT"
                assert client.raw(b"{" + b" " * (1024 * 1024))["status"] == "RESOURCE_LIMIT"
                client.call("renew", leaseId=lease, requestId="duplicate")
                client.call("renew", expected="INVALID_ARGUMENT", leaseId=lease, requestId="duplicate")
                print("PASS: correlated error responses, cancellation/deadline, stale sessions, replay and JSONL limits")

                # Deterministic capture cancellation/deadline boundary checks do not
                # assume recursive filesystem traversal takes a minimum wall time.
                for fields, status in [({"timeoutMs": 0}, "TIMEOUT"), ({"cancelled": True}, "CANCELLED")]:
                    rejected = client.call("capture", expected=status, **fields)
                    assert "offerId" not in rejected and "entries" not in rejected, rejected
                client.call("read", leaseId=lease, handleId=handle, offset="0", length=1)
                print("PASS: rejected capture emits no manifest and preserves existing lease")

                with publish(replacement):
                    changed = client.call("capture")
                    assert changed["offerId"] != manifest["offerId"]
                    assert changed["sourceChangeCount"] != manifest["sourceChangeCount"]
                    later_handle = client.call("open", leaseId=lease, fileId=late["fileId"], version=late["version"])["handleId"]
                    result = client.call("read", leaseId=lease, handleId=later_handle, offset="0", length=100)
                    assert base64.b64decode(result["bytesBase64"]) == b"old offer later child"
                    client.call("close", handleId=later_handle)
                    released = client.call("release", leaseId=lease)
                    assert released["state"] == "DRAINING", released
                    client.call("open", expected="LEASE_RELEASED", leaseId=lease, fileId=late["fileId"], version=late["version"])
                    client.call("renew", leaseId=lease)
                    client.call("read", leaseId=lease, handleId=handle, offset="0", length=10)
                    client.call("close", handleId=handle)
                    client.call("close", handleId=handle)
                    client.call("read", expected="HANDLE_CLOSED", leaseId=lease, handleId=handle, offset="0", length=1)
                    assert client.call("release", leaseId=lease)["state"] == "CLOSED"
                    print("PASS: old directory child remains readable after clipboard replacement; draining and idempotent close")

                    lease_b = client.call("acquire", offerId=changed["offerId"], offerVersion=changed["offerVersion"])["leaseId"]
                    entry_b = next(entry for entry in changed["entries"] if not entry["directory"])
                    handle_b = client.call("open", leaseId=lease_b, fileId=entry_b["fileId"], version=entry_b["version"])["handleId"]
                    replacement.write_bytes(b"changed source content")
                    client.call("read", expected="SOURCE_CHANGED", leaseId=lease_b, handleId=handle_b, offset="0", length=7)
                    client.call("disconnect")
                    client.call("read", expected="DISCONNECTED", leaseId=lease_b, handleId=handle_b, offset="0", length=7)
                    print("PASS: local source mutation and disconnected session fail without successful bytes")
            client.close()
            client = None

            # Path capture is independent of AppKit, but exercises the same source and service.
            unusual = root / 'quoted " # % \n 名称.txt'
            unusual.write_bytes(b"filename metadata")
            client = ServiceClient(service, "--path", str(unusual))
            assert any(entry["name"] == unusual.name for entry in client.ready["entries"]), client.ready
            assert str(root) not in json.dumps(client.ready)
            short = client.call("acquire", offerId=client.ready["offerId"], offerVersion=client.ready["offerVersion"], ttlMs=10)
            time.sleep(0.03)
            entry = next(entry for entry in client.ready["entries"] if not entry["directory"])
            client.call("open", expected="LEASE_EXPIRED", leaseId=short["leaseId"], fileId=entry["fileId"], version=entry["version"])
            client.close()
            client = None
            omitted = subprocess.run([service, "--path", str(later)], capture_output=True, timeout=5)
            assert omitted.returncode == 2 and not omitted.stdout
            print("PASS: explicit detectable opt-in, quoted/Unicode/control-character filenames and lease expiration")
        finally:
            if client is not None:
                client.close()
            for owner in owners:
                if owner.poll() is None:
                    owner.terminate()
                    owner.wait(timeout=5)
            subprocess.run([probe, "release", "--pasteboard", board], check=True, capture_output=True, timeout=5)


if __name__ == "__main__":
    main()
