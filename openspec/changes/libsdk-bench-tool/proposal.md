## Why

当前 CubeFS 在 AI 训练场景（MLPerf DLIO benchmark, 300万小文件）的数据读取通过 FUSE 路径，经过多轮软件调优后（元数据缓存、ExtentCache池化、Bcache优化），IOPS 从 6200 提升到 10260，单块 SATA SSD 已打满（408MB/s, %util=100%）。但 FUSE 通道存在架构性天花板（/dev/fuse 串行化 + 4次内核/用户态上下文切换），导致 5 块 SSD 的聚合带宽无法线性扩展到理论值 2000MB/s。CubeFS 已有 `client/gosdk` 用户态 SDK，可绕过 FUSE 直连存储，理论上能消除 FUSE 通道瓶颈。**需要一个benchmark工具来快速验证 libsdk 绕过 FUSE 后的实际 I/O 上限，为后续 AI 训练场景的生产接入提供数据支撑。**

## What Changes

- **新增 `cmd/libsdk_bench/` 目录**：一个独立的 Go benchmark 程序，通过 `client/gosdk` 直接读取 CubeFS volume 中的文件，模拟 DLIO 的多线程并发读取模式
- **模拟 DLIO 工作负载**：128 goroutine 并发读取、100KB 文件大小、多 epoch 循环、随机 shuffle，与 FUSE 模式下的 DLIO 参数对齐
- **实时性能统计**：每秒输出 IOPS、吞吐量(MB/s)、平均延迟，每 epoch 输出汇总，便于与 FUSE baseline 直接对比
- **支持 Bcache 加速**：通过 gosdk 的 `EnableBcache` 配置项复用现有 bcache-server 的 SSD 缓存，确保与 FUSE 模式下的数据路径一致
- **gosdk 配置补齐（可选后续）**：探索中发现 gosdk 缺少 `maxStreamerLimit`、`extentCachePoolSize`、`icacheTimeout`、`bcacheEncrypt` 等配置项，当前版本先用默认值跑 baseline，根据测试结果决定是否需要补齐

## Capabilities

### New Capabilities
- `libsdk-bench`: libsdk benchmark 工具的功能规格，包括命令行参数、工作负载模拟、性能统计输出格式、与 FUSE 模式的对比基准
- `gosdk-ai-training-readiness`: gosdk 在 AI 训练场景下的就绪状态评估，包括已知的配置缺失（bcacheEncrypt硬编码、InodeCache TTL、closeStream EvictStream行为）和对性能的影响分析

### Modified Capabilities
<!-- 无现有 spec，不需要修改 -->

## Impact

- **新增代码**：`cmd/libsdk_bench/main.go`，约 300-500 行 Go 代码
- **依赖**：`client/gosdk` 包（已有）、`client/blockcache/bcache`（已有）
- **不影响现有代码**：纯新增工具，不修改任何现有模块
- **运行依赖**：需要 bcache-server 进程运行中（提供 cachePath IPC 查询）、CubeFS 集群正常（Master/Meta/DataNode）
- **已知风险**：
  - gosdk 的 `NewBcacheClient()` 默认 `encrypt=true`，而当前 bcache-server 配置 `cacheEncrypt=false`，可能导致 bcache 读取失败，需要在测试中验证
  - gosdk 的 `closeStream` 每次 close 执行 `EvictStream`，300万文件场景下 Streamer 频繁创建销毁，第二轮 epoch 可能退化为全量 getExtents RPC
  - gosdk 的 `InodeCache` TTL 硬编码为 120s（FUSE 模式用 600s），长时间测试可能出现缓存过期
