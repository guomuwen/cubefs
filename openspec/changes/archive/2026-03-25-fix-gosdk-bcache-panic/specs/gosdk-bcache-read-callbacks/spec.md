## ADDED Requirements

### Requirement: gosdk SHALL register OnGetInodeInfo callback during client initialization
gosdk `Client.Start()` SHALL set `ExtentConfig.OnGetInodeInfo` to a function that calls `metaWrapper.InodeGet_ll(inode)` before creating the `ExtentClient`. This ensures the bcache read path in `stream_reader.go` can query inode metadata (StorageClass) without nil pointer dereference.

#### Scenario: bcache read path calls getInodeInfo successfully
- **WHEN** gosdk client is initialized with `EnableBcache=true` and a file is read from a DataNode (bcache miss)
- **THEN** `getInodeInfo(inode)` SHALL return a valid `*proto.InodeInfo` with correct `StorageClass`, and the bcache population logic SHALL proceed without panic

#### Scenario: gosdk client works without bcache
- **WHEN** gosdk client is initialized with `EnableBcache=false`
- **THEN** the `OnGetInodeInfo` callback SHALL still be registered but SHALL NOT be invoked during normal reads (bcache code path is skipped)

### Requirement: Read-only Streamer cleanup SHALL NOT panic on nil async flush channels
The `evict()`, `abort()`, and `server()` done-branch functions in `stream_writer.go` SHALL check that `asyncFlushDone` and `asyncFlushCh` are non-nil before calling `close()`. Read-only Streamers (created with `openForWrite=false`) do not allocate these channels.

#### Scenario: Read-only Streamer is evicted without panic
- **WHEN** a read-only Streamer is evicted (LRU gc, idle timeout, or explicit close) and `asyncFlushDone` is nil
- **THEN** the `evict()` function SHALL skip the `close(s.asyncFlushDone)` call and return without panic

#### Scenario: Read-only Streamer abort without panic
- **WHEN** a read-only Streamer receives a done signal and `abort()` is called with nil `asyncFlushCh`
- **THEN** the `abort()` function SHALL skip the `close(s.asyncFlushCh)` call and the `server()` done-branch SHALL also skip its cleanup close calls, all without panic
