# Shared clipboard service and core runtime

The canonical cross-platform service is the Ubuntu implementation: `FileTypes.h`,
`LocalFileSource.h`, `LocalFileRpc.h`, and `ClipboardBroker.h`, in namespace
`deskflow::clipboard`. `deskflow_clipboard` is the platform-independent service target.

`LocalFileRpc` exports the shared binary codec, fixed offer serialization and file
request dispatcher. It supports both same-user AF_UNIX adapters on macOS/Linux and
the authenticated core data channel. The core owns `PeerLink` in `CoreClipboard.*`;
that target adds Qt, TLS binding, native helpers and FUSE lifecycle management.

See [the integrated v1 guide](../../../docs/dev/clipboard-core-v1.md) for building,
configuration, the frozen wire profile, budgets, dependencies and validation limits.
`BUILD_CLIPBOARD_FILES=ON` builds the integrated feature and both native helpers.
It is disabled at runtime until enabled in Settings on both peers.

The old Mac generic service and JSONL harness remain under `experimental/` and
namespace `deskflow::clipboard::experimental`, target `deskflow_clipboard_service`.
They are historical tooling, not a second production API.

Standalone canonical service build (does not run tests):

```sh
cmake -S src/lib/clipboard -B build/clipboard-service
cmake --build build/clipboard-service
```

Source metadata checks detect ordinary file changes; they do not create immutable
snapshots. A local file read can still block in its underlying filesystem. Runtime
integration was compiled on Mac; the new cross-machine flow has not been exercised.
