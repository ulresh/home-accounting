---
name: sync-streaming-arch
description: home-accounting sync is async/streaming with a peer-state index; key hang-bug gotchas
metadata: 
  node_type: memory
  type: project
  originSessionId: 5778f742-bb34-4c48-81d2-da3f41687685
---

The sync layer (both desktop C++ and Android Kotlin) is **streaming + cancellable**, not buffer-everything. See also [[build-workflow]].

- **Files read in fixed blocks** fed straight into an incremental parser, never whole-file: desktop uses `boost::json::stream_parser`; **Android uses the Jackson library's streaming parser** (`Jk.forEachValue` in `model/Jsonl.kt` = `ObjectMapper.readValues(parser, JsonNode)` MappingIterator, reading the stream value-by-value; kotlinx-serialization-json was removed). `stateOf` (size+sha1) also streams blocks. Android network receive is pull-based: `Store.syncReceiveBlob(kind,month,replace,InputStream)` reads a `BoundedInputStream(socket,size)` via Jackson (AUTO_CLOSE_SOURCE off so it never closes the socket).
- **Send/recv stream block-by-block**: sender reads a file block and writes it to the socket immediately (`SyncSendItem` = plan only: path/offset/len/prepend, no data in memory). Receiver feeds each network block straight into `syncRecvBegin/Feed/Finish`.
- **Desktop sync = boost.asio C++20 coroutines** (`co_spawn`/`co_await`, CMake C++20). `cancel()` posts a socket/acceptor close to the io_context → aborts any pending await (accept/handshake/read/write) anywhere. **Android sync = blocking I/O on a thread; `cancel()` closes the stored socket** to interrupt any blocked read/write/accept. Both SyncServer and SyncClient are cancellable; the UI cancels both on dialog close.
- **`sync/<peerDn>.jsonl` stores PEER state**, not our own: lines are `[yyyymm, offset]` = how many bytes of our month file the peer already has. List manifests (people/catalog/device size+sha1) are exchanged live at session start; a list is sent only if our version differs from the peer's manifest. `syncCommit` records current month sizes as the peer's new offsets.

**Hang-bug gotcha (cost ~17 min once):** a fresh peer has no people.jsonl/catalog.jsonl, but `syncPlanOutgoing` still emits a zero-length list item. Android `sendItems` called `path.inputStream()` on the missing file → threw → client aborted before flushing → server hit EOF in `recvItems`, where `readLine` returned "" at EOF and `if (line.isEmpty()) continue` **busy-looped forever** at ~98% CPU. Fix: guard `sendItems` with `fileLen>0 && path.exists()`, and make `readLine` **throw on EOF** instead of returning "". (Desktop survived because C++ ifstream tolerates a missing file.) Diagnose hangs with `jstack <gradle test worker pid>` — RUNNABLE + huge CPU = busy loop, not deadlock.
