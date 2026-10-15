# Shared clipboard service prototype

This directory implements the in-process parts of `docs/dev/clipboard-contract.md`.
It is independent of AppKit, X11, Qt, FUSE and the Deskflow wire protocol.

```sh
cmake -S src/lib/clipboard -B build/clipboard-service
cmake --build build/clipboard-service
ctest --test-dir build/clipboard-service --output-on-failure
```

`deskflow_clipboard_service` exports headers as `clipboard/FileService.h`,
`clipboard/LocalFileSource.h` and `clipboard/ClipboardBroker.h`.

## Integration

1. Create a `FileService` for an already-authenticated peer and unique session epoch.
   The caller owns peer authentication; never share a service between principals.
2. Publish an `Offer` containing a fixed tree, opaque file IDs and a `FileSource`.
3. Acquire a whole-offer lease **before** publishing native clipboard file URLs.
   Renew all published offers conservatively, including replaced directory offers.
4. Open with the advertised file version. `Immutable` is the default. Local files
   require the explicit `Detectable` choice; they never silently claim snapshots.
5. Request ranges with fresh request IDs. A positive short read is not EOF. A
   filesystem adapter must aggregate until full/EOF and discard its accumulated
   bytes if any subrequest fails. `statusErrno()` provides POSIX error mapping.
6. Release enters `Draining` while handles remain; it blocks new opens and
   directory access. Close is idempotent. Neither action proves paste completion.
7. Disconnect revokes the whole session and propagates stop to pending source
   reads. Reconnect creates a new service/epoch. Join all outstanding calls before
   destroying the service; it does not own the caller's worker threads.

`LocalFileSource::capture()` pins file descriptors, captures fixed directory
membership, and checks device/inode/size/mode/mtime/ctime before and after `pread`.
It rejects symlinks and nonregular files. Capture defaults to at most 512 entries,
including the virtual root; regular-file descriptors remain pinned for the offer.
Replacing a pathname never retargets an old ID. The original descriptor either
still supplies its old data or returns `SOURCE_CHANGED` if its metadata changed.
These checks detect changes; they cannot prove immutable content under arbitrary
concurrent writes. Ordinary directory trees do not become filesystem snapshots.

## Boundaries

This is a synchronous source service. Sources must cooperate with deadlines and
cancellation. The service rejects late data after timeout/cancel/disconnect, but
cannot forcibly interrupt an arbitrary blocked source call. The POSIX source
checks before/after syscalls; its syscall can still block on an underlying device.

Budgets reject excess active requests, bytes, leases, handles, offers and metadata.
Identifiers/versions are limited to 256 bytes and entry names to 1024 bytes.
Retained replay/object state has a separate byte budget (32 MiB by default).
Replay IDs and object tombstones have explicit session-lifetime limits and are not
silently evicted; exhausting them requires establishing a new session. No existing
lease is revoked to accept a new offer. Replaced offers may be reclaimed only
after the acquisition window and after their leases/execution references end.

`validateReadResult()` checks complete transport responses, correlation fields,
fixed version, bounds and explicit EOF. Network frame parsing, authentication,
wire encoding, data-connection isolation, retry scheduling, periodic renewal,
JSONL wire logging, and native-filesystem short-read aggregation belong to adapters
and are not implemented by this library. Transport partial frames must never be
passed as complete successful responses. This code alone is not M5 integration.

Tests cover >4 GiB generated and local sparse-file offsets, UINT64_MAX EOF,
invalid source responses, old directory children after replacement, lease
draining/expiry, replay/session rejection, bounded concurrent reads, cancellation,
timeout/disconnect late responses, local mutations, replacement, permissions,
symlink/FIFO rejection, and bounded capture. Broker tests separately cover local
echo suppression, ordering and stale asynchronous publication tickets.
