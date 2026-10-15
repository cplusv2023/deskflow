# Clipboard and file prototype

The `macos` branch also builds the generated file source with macFUSE3 and an
AppKit clipboard probe. See the [macOS progress and commands](../../../docs/dev/clipboard-macos-progress.md)
for actual verification status, FSKit setup, and Finder acceptance. Linux keeps
the X11 implementation described below. Neither platform prototype replaces the
Deskflow core clipboard protocol.

The first usable release will build on the existing macFUSE/FSKit backend,
accepting its installation requirement for early users. Native File Provider
evaluation is deferred while real peer transport and lifecycle integration take
priority. The current tools remain local prototypes, not a cross-host release.
The macOS progress document records the scope and the File Promises comparison.

On macOS, the default tests use isolated named pasteboards and never mount a
filesystem. The additional `deskflow-mac-file-service` connects a selected local
file list to the [shared file service](../../lib/clipboard/README.md) through a
bounded local JSONL test channel; it requires explicit `--consistency detectable`.

```sh
cmake -S src/apps/clipboard-prototype -B build/macos-clipboard -G Ninja
cmake --build build/macos-clipboard
ctest --test-dir build/macos-clipboard --output-on-failure
```

macFUSE SDK files are discovered in `/usr/local/lib/pkgconfig`. macOS mounting
explicitly selects FSKit and requires its extension to be available and enabled.
There is no implicit kernel-backend fallback. Mount tests remain opt-in through
`CLIPBOARD_PROTOTYPE_DESKTOP_TESTS`; the X11 clipboard test is Linux-only.

## Ubuntu workflow

This experimental C++20 prototype exercises milestones M0–M2 from the
[Ubuntu roadmap](../../../docs/dev/clipboard-progress.md). It exposes generated
remote files through a read-only FUSE3 mount and publishes their local file URIs
through X11/XWayland. It does not use a real Mac, send file data over the network,
or replace Deskflow's existing clipboard service.

## Build

The standalone build needs CMake 3.24+, a C++20 compiler, pkg-config, libfuse3-dev,
libx11-dev and Python 3 for tests. It does not require Qt. FUSE3 runtime tools and
access to `/dev/fuse` are needed for mount tests.

From the repository root:

```bash
cmake -S src/apps/clipboard-prototype -B build/clipboard-prototype \
  -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/clipboard-prototype --parallel
ctest --test-dir build/clipboard-prototype --output-on-failure
```

Generated-source and shared-service tests run by default. To also mount a filesystem and
temporarily own the current desktop clipboard, explicitly enable the integration
tests:

```bash
cmake -S src/apps/clipboard-prototype -B build/clipboard-prototype \
  -DCLIPBOARD_PROTOTYPE_DESKTOP_TESTS=ON
ctest --test-dir build/clipboard-prototype --output-on-failure
```

The desktop tests require an active X11/XWayland connection. They clean up their
own mounts and processes. They validate X11 conversions; native Files paste is a
separate interactive test below.

The main Deskflow project also accepts `-DBUILD_CLIPBOARD_PROTOTYPE=ON`, which is
OFF by default. In a main-project build, build targets `deskflow-file-prototype`
and `deskflow-clipboard-probe` explicitly; executables are placed in `build/bin`.

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
paste. Real peer transport, offer replacement, cancellation and Deskflow session
integration are M3–M5 work.
