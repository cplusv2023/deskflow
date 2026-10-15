# Ubuntu clipboard and file prototype

This experimental C++20 prototype exercises M0–M2 and the M3/M4 components from the
[Ubuntu roadmap](../../../docs/dev/clipboard-progress.md). It exposes generated
remote files through a read-only FUSE3 mount and publishes their local file URIs
through X11/XWayland. It does not use a real Mac, send file data over the network,
or replace Deskflow's existing clipboard service. The local-file service and
receiver can now run in separate processes over a same-user Unix socket. M5 core
integration remains pending; see the [integration plan](../../../docs/dev/clipboard-m5-integration.md).

## Build

The standalone build needs CMake 3.24+, a C++20 compiler, pkg-config, libfuse3-dev,
libx11-dev, libxfixes-dev and Python 3 for tests. It does not require Qt. FUSE3 runtime tools and
access to `/dev/fuse` are needed for mount tests.

From the repository root:

```bash
cmake -S src/apps/clipboard-prototype -B build/clipboard-prototype \
  -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/clipboard-prototype --parallel
ctest --test-dir build/clipboard-prototype --output-on-failure
```

The default suite checks generated data, local files and leases, Broker state,
two-process RPC and isolated X11 formats. The X11 suite starts its own Xvfb and
returns skip code 77 if unavailable. Install `xvfb`, or set `XVFB_BINARY` to an
extracted Xvfb executable. It never uses the current desktop clipboard.

To also exercise FUSE without accessing the desktop clipboard:

```bash
cmake -S src/apps/clipboard-prototype -B build/clipboard-prototype \
  -DCLIPBOARD_PROTOTYPE_FUSE_TESTS=ON
ctest --test-dir build/clipboard-prototype --output-on-failure
```

To run the older desktop X11 roundtrip as well, explicitly enable desktop tests.
This test temporarily owns the current clipboard:

```bash
cmake -S src/apps/clipboard-prototype -B build/clipboard-prototype \
  -DCLIPBOARD_PROTOTYPE_DESKTOP_TESTS=ON
ctest --test-dir build/clipboard-prototype --output-on-failure
```

The desktop tests require an active X11/XWayland connection. They clean up their
own mounts and processes. They validate X11 conversions; native Files paste is a
separate interactive test below.

The main Deskflow project also accepts `-DBUILD_CLIPBOARD_PROTOTYPE=ON`, which is
OFF by default. Its three executables are `deskflow-file-prototype`,
`deskflow-clipboard-probe` and `deskflow-file-service`, placed in `build/bin`.

## Capture and publish formats

`publish-formats` lazily provides text, HTML, PNG and URI data. Format payloads
are bounded at 16 MiB each / 32 MiB per offer; file content does not use these
format buffers. Large payloads use X11 INCR in both directions.

```bash
build/clipboard-prototype/deskflow-clipboard-probe publish-formats \
  --format 'text/plain;charset=utf-8=/tmp/message.txt' \
  --format 'text/html=/tmp/message.html' --format 'image/png=/tmp/image.png'
build/clipboard-prototype/deskflow-clipboard-probe read --target image/png \
  --output /tmp/clipboard-image.png
build/clipboard-prototype/deskflow-clipboard-probe watch --origin receiver-test
build/clipboard-prototype/deskflow-clipboard-probe capture-files
```

The publisher stays in the foreground; run readers in another terminal.
`watch` uses XFixes, records owner/format announcements, and does not fetch file
bodies. Assign different `--origin` values when simulating separate devices;
events carrying the watcher's own origin are ignored. Reads pin owner and
selection timestamp so an intervening copy cannot silently change their source.
`capture-files` accepts local file URIs and GNOME `copy` lists, rejecting `cut`,
remote hosts, invalid escapes and path ambiguity. It outputs paths without
opening file content.

## Serve real files in another process

Create a private directory and start a server in terminal 1:

```bash
prototype_run=$(mktemp -d "$XDG_RUNTIME_DIR/deskflow-files.XXXXXX")
mkdir "$prototype_run/mount"
printf '%s\n' "$prototype_run"
build/clipboard-prototype/deskflow-file-service serve \
  --socket "$prototype_run/files.sock" --source /absolute/path/to/a/directory \
  --log "$prototype_run/source.jsonl" --source-chunk-size 4093
```

Alternatively use `serve --socket ... --from-clipboard` to capture the current
file selection once. Later copies are not automatically routed by this CLI;
the Broker/runtime connection is a separate integration step. Only the server's
selection authorizes file IDs; clients cannot request arbitrary host paths.

In terminal 2, set `prototype_run` to the printed directory, then inspect or mount:

```bash
build/clipboard-prototype/deskflow-file-service inspect \
  --socket "$prototype_run/files.sock"
build/clipboard-prototype/deskflow-file-prototype \
  --socket "$prototype_run/files.sock" --mount "$prototype_run/mount" \
  --log "$prototype_run/receiver.jsonl" --detectable --timeout-ms 5000
```

The single-client experimental server creates a fresh session on each connection;
finish `inspect` before starting the receiver. Its socket is 0600 under a 0700
directory, and both sides check peer UID. This is local test transport, not TLS
or a Deskflow extension protocol.

The receiver acquires a whole-offer lease before mounting, renews even with no
open handles, and releases on normal unmount. Reads use explicit status/EOF and
aggregate positive short responses. The connection permits one in-flight request
of at most 256 KiB, with a 1 MiB frame bound. Timeout/cancel/malformed responses
close that stream; incomplete or late bytes are never reused by another request.

Ordinary files cannot guarantee immutable versions. The explicit `--detectable`
option checks identity, permissions, size and change timestamps before/after
reads; detected changes fail with `SOURCE_CHANGED`/`ESTALE`. Without this option,
opening ordinary source files returns `SOURCE_NOT_STABLE`. This mode is not a
snapshot guarantee. Symlinks and special files are rejected, and manifests are
bounded and fixed for the offer.

Copy mounted files normally, or request one consumer range with `read --socket ...
--path OFFER_RELATIVE_PATH --offset N --length N --output NEW_PATH --detectable`.
The CLI writes the chosen destination under a temporary name, then atomically
links that same inode into place on success. It removes failed output and never
overwrites an existing destination. Neither the source service nor the FUSE
provider creates a complete intermediate file cache.

Stop the receiver before the service; verify unmount before deleting the dedicated
directory. The RPC mount suite also tests idle renewal, source mutation, server
disconnect, timeout and cancellation of a blocked read during receiver shutdown.

## Inspect the generated source

```bash
build/clipboard-prototype/deskflow-file-prototype --describe
build/clipboard-prototype/deskflow-file-prototype \
  --read large --offset 4294967305 --length 97 --output /tmp/deskflow-range.bin
```

`--describe` reports file IDs, paths, sizes and content rules. Pattern files use
`byte = (absoluteOffset % 251 + seed) % 251`. The default large file has size
4 GiB + 123 bytes, but those bytes are generated only when read. `hello.txt` and
`preview.svg` contain small fixed literals; the other nonempty files use the
pattern. `--source-chunk-size` can force short responses to test aggregation.

The shared [fixtures-v1.json](fixtures-v1.json) manifest freezes sizes, seeds,
literal contents, range bytes and hashes for both Ubuntu and macOS. The source
test checks its ten range vectors. Fault plans in the manifest are M4 acceptance
specifications, not implemented fault tests. Read results, leases, version errors
and the future log schema are defined in the [cross-platform contract draft](../../../docs/dev/clipboard-contract.md).

## Run a native Files paste

Use a new, dedicated directory for each run. In terminal 1, create the directories,
note the printed path, then leave the mount running in the foreground:

```bash
prototype_run=$(mktemp -d "$XDG_RUNTIME_DIR/deskflow-clipboard.XXXXXX")
mkdir "$prototype_run/mount" "$prototype_run/target"
printf '%s\n' "$prototype_run"
build/clipboard-prototype/deskflow-file-prototype \
  --mount "$prototype_run/mount" --log "$prototype_run/files.jsonl" \
  --large-size 268435579
```

In terminal 2, set `prototype_run` to that same printed path and publish files.
Keep this process running; it owns the clipboard and serves selection requests.
The publication itself does not open or read the named files.

```bash
build/clipboard-prototype/deskflow-clipboard-probe publish \
  --log "$prototype_run/clipboard.jsonl" \
  "$prototype_run/mount/hello.txt" \
  "$prototype_run/mount/empty.txt" \
  "$prototype_run/mount/nested folder" \
  "$prototype_run/mount/large.bin"
```

Open the dedicated `target` directory in Files and use its normal Paste action.
For the verified Nautilus 50 accessibility path, terminal 3 can invoke that same
action automatically. Set `prototype_run` to the printed path first:

```bash
/usr/bin/python3 src/apps/clipboard-prototype/paste_in_files.py \
  "$prototype_run/target" --open
```

The helper requires system PyGObject and AT-SPI bindings. It calls the dedicated
window's `view.paste` action, not `FileOperations2.CopyURIs`. `status=invoked`
means the action was submitted; check the destination and hashes separately.
It refuses ambiguous windows with the same directory basename.

After the copy, inspect `files.jsonl`, compare output with the fixture rules, and
verify that copied files are editable. The current X11 publisher limits the
encoded file list to 64 KiB; file content is read through FUSE, not stored in that
clipboard payload. Existing application-specific text/image synchronization is
outside this prototype.

## Observe reads and clean up

Each source request records its ID, file ID, offset, requested and returned byte
counts, duration and active/in-flight totals. Requests are capped at 256 KiB and
the FUSE worker limit is eight. The prototype uses direct I/O with readahead
disabled to make consumer reads easier to observe; this is not a final throughput
optimization.

Pure `stat` and directory-listing operations do not request file content. Files
may still read file headers for MIME detection even when thumbnails are disabled.
Enabled thumbnails can request more data. Those are consumer-demand reads and
must not be confused with full-file predownload.

Close the test Files windows, stop the publisher with Ctrl+C, then stop the mount
with Ctrl+C. Normal termination unmounts it. If a stopped prototype left its own
mount behind, use `fusermount3 -u "$prototype_run/mount"`. Retain logs as needed
and remove only the dedicated test directory after confirming it is unmounted.

The simulated data has no complete backing file and no intermediate full-file
cache. The user's chosen destination is the only full copy produced during native
paste. Real peer TLS transport, runtime-driven offer replacement, native four-format
Wayland acceptance and Deskflow session integration still require further work.
