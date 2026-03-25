## Why

gosdk（`client/gosdk`）在 `bcache=true` 模式下读取文件时会 panic，因为两处 SDK 初始化缺陷：
1. `ExtentClient.getInodeInfo` 回调未设置（nil 函数指针），bcache 读路径调用时触发 nil pointer dereference
2. 只读 Streamer 的 `asyncFlushDone`/`asyncFlushCh` channel 未初始化（nil），Streamer 被驱逐时 `close(nil)` 触发 panic

这阻塞了 `libsdk_bench` 工具在 bcache 模式下的性能测试，无法量化 gosdk + bcache 相对于 FUSE 的性能提升。

## What Changes

- 在 gosdk `Client.Start()` 中为 `ExtentClient` 注册 `OnGetInodeInfo` 回调，使 bcache 读路径能正确查询 inode 的 StorageClass
- 在 `stream_writer.go` 的 `evict()`、`abort()`、`server()` 函数中，对 `asyncFlushDone` 和 `asyncFlushCh` 添加 nil 检查，防止只读 Streamer 被驱逐时 panic

## Capabilities

### New Capabilities
- `gosdk-bcache-read-callbacks`: gosdk 为 ExtentClient 提供完整的 bcache 读取所需回调（getInodeInfo），使 bcache 读路径可正常工作

### Modified Capabilities
- `gosdk-ai-training-readiness`: 补齐 gosdk 的 bcache 读取支持，使 libsdk_bench 在 bcache=true 模式下不再 panic

## Impact

- **代码影响**：`client/gosdk/cfs_client.go`（添加 OnGetInodeInfo 回调）、`sdk/data/stream/stream_writer.go`（3 处 nil channel 防护）
- **风险**：低。仅添加防御性检查和缺失回调，不改变现有读写逻辑
- **依赖**：无新增依赖
- **受益**：`libsdk_bench -bcache=true` 可正常运行，完成 gosdk vs FUSE 的 bcache 性能对比
