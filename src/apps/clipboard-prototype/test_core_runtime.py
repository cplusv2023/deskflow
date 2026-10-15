#!/usr/bin/env python3
# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT
"""Actual core runtime on two private X11 desktops and a loopback TLS channel.

The default harness supplies a synthetic main-session binding. --core instead
runs real Deskflow TLS and protocol negotiation on private X11 desktops.
--native-gnome uses the real Wayland clipboard and GTK4/Nautilus windows with
the runtime harness. None of these modes test a real Mac or Portal interaction.
"""
import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import secrets
import select
import socket
import shutil
import subprocess
import sys
import tempfile
import time

from test_x11_formats import png_bytes, rows, stop


def wait_for(predicate, label, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(0.04)
    raise AssertionError("Timed out: " + label)


def exited(status_file):
    try:
        return "State:\tZ" in status_file.read_text()
    except FileNotFoundError:
        return True


class Workspace:
    """Retain evidence rather than traverse an unexpectedly live filesystem."""
    def __enter__(self):
        self.path = Path(tempfile.mkdtemp(prefix="deskflow-core-test-"))
        return str(self.path)

    def __exit__(self, *_):
        if any(str(self.path) + "/" in line for line in Path("/proc/self/mountinfo").read_text().splitlines()):
            print("Live test mount retained at " + str(self.path), file=sys.stderr)
        else:
            shutil.rmtree(self.path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--harness", type=Path, required=True)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--core", type=Path, help="Exercise the real deskflow-core handshake on private X11 desktops")
    parser.add_argument("--xvfb", default=os.environ.get("XVFB_BINARY", shutil.which("Xvfb")))
    parser.add_argument("--native-gnome", action="store_true",
                        help="Use the real GNOME desktop for one endpoint; REPLACES YOUR CLIPBOARD and opens test windows")
    args = parser.parse_args()
    if args.core and args.native_gnome:
        parser.error("real core mode uses isolated X11; native mode uses the runtime harness")
    if not args.xvfb or not Path("/dev/fuse").exists():
        print("SKIP: requires Xvfb and /dev/fuse", file=sys.stderr)
        return 77
    args.harness, args.probe = args.harness.resolve(), args.probe.resolve()
    if args.core:
        args.core = args.core.resolve()
    processes, streams = [], []
    with Workspace() as directory:
        work = Path(directory)
        native_publisher = None
        native_log = None
        core_environments = {}

        def status_rows(path):
            if not args.core:
                return rows(path)
            return [{"value": line.split("clipboard: ", 1)[1]} for line in path.read_text().splitlines()
                    if "clipboard: " in line]

        def launch(name, command, environment):
            log = work / (name + ".jsonl")
            errors = work / (name + ".stderr")
            out, err = log.open("wb"), errors.open("wb")
            streams.extend([out, err])
            p = subprocess.Popen(command, env=environment, stdout=out, stderr=err)
            processes.append(p)
            if args.core and Path(command[0]) == args.core:
                core_environments[p.pid] = environment
            return p, log, errors

        def stop_core(p):
            if p.poll() is not None:
                return
            with socket.socket(socket.AF_UNIX) as ipc:
                ipc.settimeout(3)
                ipc.connect(str(Path(core_environments[p.pid]["TMPDIR"]) / "deskflow-core"))
                ipc.sendall(b"stop\n")
                assert b"ok" in ipc.recv(1024)
            p.wait(timeout=10)

        def desktop(name):
            read_fd, write_fd = os.pipe()
            try:
                p = subprocess.Popen([args.xvfb, "-displayfd", str(write_fd), "-screen", "0", "1024x768x24",
                                      "-nolisten", "tcp", "-noreset", "-ac"], pass_fds=(write_fd,),
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                processes.append(p)
                os.close(write_fd)
                assert select.select([read_fd], [], [], 10)[0], "Xvfb startup"
                display = ":" + os.read(read_fd, 64).decode().strip()
                assert display != ":"
            finally:
                os.close(read_fd)
            # Select the GNOME bridge branch, but never use the real desktop.
            env = dict(os.environ, DISPLAY=display, XDG_SESSION_TYPE="wayland",
                       XDG_CURRENT_DESKTOP="ubuntu:GNOME", WAYLAND_DISPLAY="isolated-runtime-test",
                       XDG_CONFIG_HOME=str(work / name / "config"), XDG_STATE_HOME=str(work / name / "state"))
            Path(env["XDG_CONFIG_HOME"]).mkdir(parents=True)
            Path(env["XDG_STATE_HOME"]).mkdir(parents=True)
            return env

        def probe(env, *argv, success=True):
            result = subprocess.run([str(args.probe), *map(str, argv), "--timeout-ms", "1500"],
                                    env=env, capture_output=True, timeout=5)
            if success:
                assert result.returncode == 0, (argv, result.stdout, result.stderr)
            return result

        def read_matches(env, target, expected):
            result = probe(env, "read", "--target", target, success=False)
            return result.returncode == 0 and result.stdout == expected

        def publish(env, name, command):
            nonlocal native_publisher, native_log
            if args.native_gnome and env is a:
                if command[0] == "publish":
                    path = work / (name + ".uris")
                    path.write_text("".join(Path(p).as_uri() + "\r\n" for p in command[1:]))
                    entries = {"text/uri-list": str(path)}
                else:
                    entries = dict(str(command[i]).rsplit("=", 1) for i in range(2, len(command), 2))
                manifest = work / "native.manifest"
                replacement = work / "next.manifest"
                replacement.write_text(json.dumps(entries))
                previous = len(rows(native_log)) if native_log else 0
                replacement.replace(manifest)
                if native_publisher:
                    wait_for(lambda: len(rows(native_log)) > previous, "native clipboard update")
                    return native_publisher
                command = ["/usr/bin/python3", str(Path(__file__).with_name("test_gnome_clipboard.py")),
                           "--fixture-publish", str(manifest)]
                env = dict(env, GDK_BACKEND="wayland", GTK_A11Y="none")
            else:
                command = [str(args.probe), *map(str, command)]
            p, log, _ = launch(name, command, env)
            wait_for(lambda: any(row.get("event") == "ready" for row in rows(log)), name + " ready")
            if args.native_gnome and env.get("GDK_BACKEND") == "wayland":
                native_publisher, native_log = p, log
            return p

        def captured(env):
            result = probe(env, "capture-files", success=False)
            if result.returncode:
                return []
            return [Path(path) for path in json.loads(result.stdout)]

        try:
            b = desktop("b")
            if args.native_gnome:
                assert os.environ.get("XDG_SESSION_TYPE") == "wayland" and os.environ.get("WAYLAND_DISPLAY")
                a = dict(os.environ, XDG_CONFIG_HOME=str(work / "a-config"), XDG_STATE_HOME=str(work / "a-state"))
                Path(a["XDG_CONFIG_HOME"]).mkdir()
                Path(a["XDG_STATE_HOME"]).mkdir()
            else:
                a = desktop("a")
            if args.core:
                for name, env in (("a", a), ("b", b)):
                    env.pop("WAYLAND_DISPLAY", None)
                    env["XDG_SESSION_TYPE"] = "x11"
                    env["QT_QPA_PLATFORM"] = "minimal"
                    private_tmp = work / (name + "-tmp")
                    private_tmp.mkdir(mode=0o700)
                    env["TMPDIR"] = str(private_tmp)
            certs = []
            for name in ("a", "b"):
                key, cert, pem = [work / (name + suffix) for suffix in (".key", ".crt", ".pem")]
                subprocess.run(["/usr/bin/openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                                "-days", "1", "-subj", "/CN=deskflow-test-" + name,
                                "-keyout", key, "-out", cert], check=True, capture_output=True, timeout=15)
                pem.write_bytes(key.read_bytes() + cert.read_bytes())
                pem.chmod(0o600)
                certs.append(pem)
            secret = secrets.token_hex(32)
            if args.core:
                with socket.socket() as reservation:
                    reservation.bind(("127.0.0.1", 0))
                    port = reservation.getsockname()[1]
                server_config = work / "server.conf"
                server_config.write_text("section: links\n a:\n  right = b\n b:\n  left = a\nend\nsection: options\nend\n")
                configs = []
                for index, (name, env) in enumerate((("a", a), ("b", b))):
                    settings = Path(env["XDG_CONFIG_HOME"]) / "Deskflow" / "Deskflow.conf"
                    settings.parent.mkdir(exist_ok=True)
                    settings.write_text(f"[core]\ncomputerName={name}\ndisplay={env['DISPLAY']}\nport={port}\n"
                                        f"interface=127.0.0.1\nfileClipboard=true\n"
                                        f"[security]\ntlsEnabled=true\ncheckPeerFingerprints=true\ncertificate={certs[index]}\n"
                                        f"[server]\nenableClipboard=true\nclipboardSize=3\nexternalConfig=true\nexternalConfigFile={server_config}\n"
                                        "[client]\nremoteHost=127.0.0.1\n[log]\nlevel=DEBUG\n"
                                        "[computer_a]\nname=a\n[computer_b]\nname=b\n")
                    trusted = settings.parent / "tls"
                    trusted.mkdir()
                    digest = subprocess.check_output(["/usr/bin/openssl", "x509", "-in", str(certs[1-index]),
                                                      "-outform", "DER"])
                    (trusted / ("trusted-clients" if index == 0 else "trusted-servers")).write_text(
                        "v2:sha256:" + hashlib.sha256(digest).hexdigest() + "\n")
                    configs.append(settings)
                server, server_log, server_err = launch("server", [args.core, "server", "--new-instance", "--settings", configs[0]], a)
                wait_for(lambda: "started server" in server_log.read_text(), "real server listener")
                client, client_log, client_err = launch("client", [args.core, "client", "--new-instance", "--settings", configs[1]], b)
            else:
                server, server_log, server_err = launch("server", [args.harness, certs[0], certs[1], secret, "listen", "0"], a)
                port = wait_for(lambda: next((r["port"] for r in rows(server_log) if r.get("event") == "started"), 0), "server port")
                client, client_log, client_err = launch("client", [args.harness, certs[1], certs[0], secret, "connect", str(port)], b)
            for log in (server_log, client_log):
                expected = "Ready: X11" if args.core else "Ready: GNOME Wayland via XWayland"
                wait_for(lambda: any(r.get("value", "").startswith(expected) for r in status_rows(log)), "TLS ready / backend selection")
                if not args.core:
                    events = rows(log)
                    assert next(i for i, r in enumerate(events) if r.get("event") == "legacyGuardReleased") < \
                           next(i for i, r in enumerate(events) if r.get("value", "").startswith("Ready:"))

            text, html, png = "Ubuntu TLS 剪贴板\n".encode(), b"<p><b>Clipboard</b> &amp; HTML</p>", png_bytes()
            formats = [("text/plain;charset=utf-8", text), ("text/html", html), ("image/png", png)]
            command = ["publish-formats"]
            for index, (mime, data) in enumerate(formats):
                path = work / ("format-" + str(index))
                path.write_bytes(data)
                command += ["--format", mime + "=" + str(path)]
            publish(a, "formats-a", command)
            for mime, data in formats:
                wait_for(lambda m=mime, d=data: read_matches(b, m, d), "forward " + mime)

            reverse = work / "reverse.txt"
            reverse.write_text("Client → server, same connection\n")
            publish(b, "text-b", ["publish-formats", "--format", "text/plain=" + str(reverse)])
            wait_for(lambda: read_matches(a, "text/plain;charset=utf-8", reverse.read_bytes()), "reverse text")

            source = work / "source A"
            source.mkdir()
            (source / "empty directory").mkdir()
            (source / "empty.txt").touch()
            body = bytes(range(256)) * 16384 + b"range-tail"
            (source / "first.bin").write_bytes(body)
            (source / "未打开 child.txt").write_text("directory lease survives clipboard replacement\n")
            (source / "changed.bin").write_bytes(b"original version")
            publish(a, "files-a", ["publish", source])
            roots = wait_for(lambda: captured(b), "forward file references")
            remote = roots[0]
            assert remote.name == source.name
            assert "clipboard RPC read requestId=" not in (server_log if args.core else server_err).read_text(), "file body read before consumer"
            old_fd = os.open(remote / "first.bin", os.O_RDONLY)
            try:
                assert os.read(old_fd, 4096) == body[:4096]
                new_file = work / "selection B.txt"
                new_file.write_text("new clipboard B")
                publish(a, "files-b", ["publish", new_file])
                wait_for(lambda: any(p.name == new_file.name for p in captured(b)), "replacement B published")
                assert os.read(old_fd, 4096) == body[4096:8192]
            finally:
                os.close(old_fd)
            # No A handles remain; unopened children must still be available.
            destination = work / "copied A"
            shutil.copytree(remote, destination)
            for original in source.rglob("*"):
                copied = destination / original.relative_to(source)
                if original.is_file():
                    assert hashlib.sha256(copied.read_bytes()).digest() == hashlib.sha256(original.read_bytes()).digest()
                else:
                    assert copied.is_dir()
            (source / "changed.bin").write_bytes(b"a changed version, longer")
            try:
                with (remote / "changed.bin").open("rb") as f:
                    f.read()
            except OSError as error:
                assert error.errno in (errno.ESTALE, errno.EIO), error
            else:
                raise AssertionError("mutated source accepted")

            reverse_file = work / "reverse file.txt"
            reverse_file.write_text("client filesystem → server filesystem\n")
            publish(b, "files-reverse", ["publish", reverse_file])
            reverse_roots = wait_for(lambda: [p for p in captured(a)
                                             if p.name == reverse_file.name and p != reverse_file],
                                     "reverse file references")
            assert reverse_roots[0].read_bytes() == reverse_file.read_bytes()
            if args.native_gnome:
                destination = work / ("Files-paste-" + secrets.token_hex(4))
                destination.mkdir()
                paste = subprocess.run(["/usr/bin/python3", str(Path(__file__).with_name("paste_in_files.py")),
                                        str(destination), "--open"], env=a, capture_output=True, timeout=20)
                assert paste.returncode == 0, (paste.stdout, paste.stderr)
                copied = destination / reverse_file.name
                wait_for(lambda: copied.exists() and copied.read_bytes() == reverse_file.read_bytes(), "native Files paste")

            (stop_core if args.core else stop)(client)
            if not args.core:
                assert any(r.get("event") == "stopped" and r.get("legacyAvailable") for r in rows(client_log))
            wait_for(lambda: any(r.get("value") == "Disconnected; legacy clipboard restored" for r in status_rows(server_log)),
                     "disconnect invalidates peer")
            (stop_core if args.core else stop)(server)
            if not args.core:
                assert any(r.get("event") == "stopped" and r.get("legacyAvailable") for r in rows(server_log))
            assert not remote.exists(), "session-owned mountpoint not cleaned up"
            assert not reverse_roots[0].exists(), "reverse mountpoint not cleaned up"
            if args.core:
                assert server.returncode == 0 and client.returncode == 0, (server.returncode, client.returncode)
                server, server_log, server_err = launch("restart-server", [args.core, "server", "--new-instance", "--settings", configs[0]], a)
                wait_for(lambda: "started server" in server_log.read_text(), "restarted core listener")
                client, client_log, client_err = launch("restart-client", [args.core, "client", "--new-instance", "--settings", configs[1]], b)
                for log in (server_log, client_log):
                    wait_for(lambda: any(r.get("value", "").startswith("Ready: X11") for r in status_rows(log)), "reconnected real cores")
                crash_source = work / "parent-death.txt"
                crash_source.write_text("a helper must unmount after its core disappears")
                publish(a, "crash-source", ["publish", crash_source])
                crash_roots = wait_for(lambda: [p for p in captured(b) if p.name == crash_source.name], "mount before abrupt core death")
                assert crash_roots[0].read_bytes() == crash_source.read_bytes()
                helper_pids = []
                for path in Path("/proc").glob("[0-9]*/status"):
                    try:
                        if f"PPid:\t{client.pid}\n" in path.read_text():
                            helper_pids.append(path.parent.name)
                    except FileNotFoundError:
                        pass
                assert helper_pids, "real core did not start helpers"
                client.kill()
                client.wait(timeout=5)
                wait_for(lambda: not crash_roots[0].exists() and not os.path.ismount(crash_roots[0].parent), "parent death unmounts FUSE")
                for pid in helper_pids:
                    status_file = Path("/proc") / pid / "status"
                    wait_for(lambda p=status_file: exited(p), "helper exits after parent death")
                stop_core(server)
                print("PASS: actual deskflow-core mutual TLS, protocol 1.9/DCAP, exporter binding, bidirectional "
                      "formats/files, old directory retention, source mutation, normal stop/unmount, reconnect, "
                      "SIGKILL parent death stops helpers/unmounts FUSE")
                return 0

            # An otherwise valid certificate must not bind a different main
            # session. Reject it, retain the listener, then allow the valid peer.
            auth_server, auth_log, _ = launch("auth-server", [args.harness, certs[0], certs[1], secret, "listen", "0"], a)
            auth_port = wait_for(lambda: next((r["port"] for r in rows(auth_log) if r.get("event") == "started"), 0), "auth server port")
            bad, bad_log, _ = launch("wrong-binding", [args.harness, certs[1], certs[0], secrets.token_hex(32), "connect", str(auth_port)], b)
            wait_for(lambda: any("rejected an unbound connection" in r.get("value", "") for r in rows(auth_log)), "wrong binding rejected")
            assert not any(r.get("value", "").startswith("Ready:") for r in rows(bad_log))
            stop(bad)
            valid, valid_log, _ = launch("valid-binding", [args.harness, certs[1], certs[0], secret, "connect", str(auth_port)], b)
            wait_for(lambda: any(r.get("value", "").startswith("Ready:") for r in rows(valid_log)), "valid binding after rejection")
            stop(valid)
            stop(auth_server)

            rejected_env = dict(a, XDG_CURRENT_DESKTOP="KDE")
            rejected = subprocess.run([args.harness, certs[0], certs[1], secret, "listen", "0"],
                                      env=rejected_env, capture_output=True, timeout=5)
            assert rejected.returncode == 3 and b"supports GNOME" in rejected.stdout
            print("PASS: TLS bind; GNOME bridge selection; text/HTML/PNG chunks; bidirectional files; "
                  "no file prefetch; old directory lease; source mutation; disconnect/cleanup; unsupported desktop; "
                  "serialized legacy handoff; wrong binding rejected without consuming listener; "
                  + ("native GTK4 source + Nautilus paste" if args.native_gnome else "private X11 desktops"))
        except Exception:
            for path in sorted(work.glob("*.stderr")):
                print("\n" + path.name + ":\n" + path.read_text(errors="replace")[-10000:], file=sys.stderr)
            for path in sorted(work.glob("*.jsonl")):
                if path.exists():
                    print(path.name + ":\n" + path.read_text()[-6000:], file=sys.stderr)
            if native_log:
                print("native GTK provider:\n" + native_log.read_text(), file=sys.stderr)
            raise
        finally:
            for process in reversed(processes):
                if process.pid in core_environments and process.poll() is None:
                    try:
                        stop_core(process)
                    except (OSError, AssertionError, subprocess.TimeoutExpired):
                        pass
                stop(process)
            for stream in streams:
                stream.close()
            # Never let TemporaryDirectory recurse into a failed live mount.
            # Only paths under this test's private directory are eligible.
            for line in Path("/proc/self/mountinfo").read_text().splitlines():
                fields = line.split()
                mountpoint = fields[4].replace("\\040", " ").replace("\\134", "\\")
                if mountpoint.startswith(str(work) + "/") and " - fuse " in line:
                    subprocess.run(["fusermount3", "-u", mountpoint], capture_output=True, timeout=5)
                    if os.path.ismount(mountpoint):
                        raise RuntimeError("Test mount retained; manually unmount " + mountpoint)
    return 0


if __name__ == "__main__":
    sys.exit(main())
