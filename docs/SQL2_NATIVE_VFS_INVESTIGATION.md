# guideXOS SQL — SQL2 Native VFS Investigation

Status: **investigation complete; server-side change deferred.**

## Background

The relational storage layer talks to storage only through `IDatabaseFile`
(`database/database_io.h`), which requires positional I/O and a durability
barrier:

```cpp
virtual DbResult open(path, mode, truncateExisting) = 0;
virtual void close() = 0;
virtual uint64_t size() = 0;
virtual bool readAt(uint64_t offset, void* buffer, size_t length) = 0;
virtual bool writeAt(uint64_t offset, const void* buffer, size_t length) = 0;
virtual bool truncateTo(uint64_t size) = 0;
virtual bool flush() = 0; // OS-level durability barrier
```

The hosted build provides `HostDatabaseFile` (stdio). A native guideXOS
adapter must supply an equivalent implementation over the guideXOS VFS.

## Current guideXOS Server FS contract

`guideXOSServer/fs.h` exposes a **static, whole-file** interface:

```cpp
static std::vector<FileInfo> list(path);
static bool readAll(path, out);                    // whole file
static FSResult readAll(path, out, maxBytes);      // whole file
static bool writeAll(path, data);                  // whole file
static bool exists(path);
static bool createDirectories(path);
static bool renameFile(from, to, replaceExisting);
static bool removeFile(path);
static DiskTelemetrySnapshot telemetrySnapshot();
```

The implementation (`fs.cpp`) is backed by `std::ifstream`/`std::ofstream`
(hosted) / Win32 file APIs. There is:

* **no positional read/write** (`readAt` / `writeAt` / seek+read / seek+write);
* **no flush / durability barrier**;
* **no resize / append**;
* **no handle / open-close model** (the interface is static and whole-file).

## Exact missing contract

A native `IDatabaseFile` adapter requires the following VFS capabilities,
which the current `FS` interface does not provide:

| Capability | guideXOS VFS equivalent needed |
|------------|--------------------------------|
| Open | `open(path, mode) -> handle` (read-only / read-write / create) |
| Positional read | `readAt(handle, offset, buffer, length)` or `seek(handle, offset) + read` |
| Positional write | `writeAt(handle, offset, buffer, length)` or `seek + write` |
| Resize / append | `resize(handle, size)` or `append` |
| Durability barrier | `flush(handle)` (equivalent of `fsync` / `CommitFileBuffers`) |
| Close | `close(handle)` |

A **clearly documented durability boundary** is also required: the VFS `flush`
must guarantee that all prior writes for that handle are persisted across a
power loss before it returns. Without that guarantee, `.gxdb` durability
semantics cannot be honored natively.

## Why this is deferred

Adding these primitives cleanly requires a **handle-based redesign** of the
`FS` abstraction (the current interface is static and whole-file). `FS` is a
widely-used static interface across guideXOS Server; changing it touches many
callers and is risky to do as part of the relational storage phase. The
specification explicitly frames this as a potential *separate logically
isolated commit* and forbids contaminating the relational layer with
filesystem workarounds or a fake `readAll`/`writeAll` backend.

The relational layer is **not** blocked: it is fully backend-agnostic behind
`IDatabaseFile`, and the hosted proof is complete. When the VFS gains the
primitives above, a native adapter can be implemented against the existing
`IDatabaseFile` contract without any change to the catalog, heap, or buffer
layers.

## Recommendation

Track a guideXOS Server VFS enhancement to add a handle-based positional I/O +
flush API (table above). Once available, implement `GxosDatabaseFile :
IDatabaseFile` in a separate commit and prove it under QEMU/bare metal.
