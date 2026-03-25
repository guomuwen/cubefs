## Context

commit `dd472b1f4` 将 Streamer 分为读写两种模式：只读 Streamer 不再分配 `asyncFlushCh`/`asyncFlushDone`/`asyncFlushSemaphore` channel，但驱逐/清理代码（`evict()`、`abort()`、`server()` done 分支）仍然无条件 `close()` 这些 channel。同时 gosdk 作为用户态 SDK，在初始化 `ExtentClient` 时从未设置 `OnGetInodeInfo` 回调，而 bcache 读路径需要通过该回调查询 inode 的 StorageClass 来判断是否使用 bcache。

当前工作区已修复 nil channel 问题（3 处 nil guard），需补齐 `getInodeInfo` 回调。

## Goals / Non-Goals

**Goals:**
- 让 gosdk 在 `bcache=true` 模式下读取文件不再 panic
- `libsdk_bench -bcache=true` 可正常跑完多 epoch 测试
- 不引入额外的性能退化

**Non-Goals:**
- 不补齐 gosdk 所有缺失的 FUSE 配置项（如 DentryCache v2、maxStreamerLimit 等）
- 不修改 bcache 读写逻辑本身
- 不优化 getInodeInfo 的性能（如缓存策略）

## Decisions

### Decision 1: getInodeInfo 回调实现方式

**选择**：在 gosdk `Client.Start()` 中通过闭包将 `metaWrapper.InodeGet()` 包装为 `OnGetInodeInfo` 回调

**理由**：
- FUSE 客户端（`client/fs/super.go`）已有相同模式：`ec.GetInodeInfo = mw.InodeGet`
- gosdk 已持有 `metaWrapper` 实例，无需新建依赖
- 替代方案（在 ExtentClient 中直接引用 metaWrapper）会破坏现有的回调解耦设计

### Decision 2: nil channel 防护位置

**选择**：在 `close()` 调用前添加 `if ch != nil` 检查

**理由**：
- 最小改动，不改变 channel 生命周期设计
- 替代方案（为只读 Streamer 也初始化 channel）会浪费内存，违背 `dd472b1f4` 的优化意图
- `dd472b1f4` 的设计意图是只读 Streamer 不需要 asyncFlush 系统，保持这个语义

## Risks / Trade-offs

| Risk | Mitigation |
|------|-----------|
| `InodeGet` RPC 在 bcache 热路径上增加延迟 | bcache 命中时直接返回缓存数据，不走 getInodeInfo；只在 bcache miss 后的填充路径调用 |
| gosdk 的 MetaWrapper 配置与 FUSE 不同，InodeGet 行为可能有差异 | InodeGet 是标准 meta RPC，无配置差异 |
| stream_writer.go 的 nil 检查已在工作区修改，需确保不重复提交 | tasks 中标记为"已完成"，apply 时验证 |
