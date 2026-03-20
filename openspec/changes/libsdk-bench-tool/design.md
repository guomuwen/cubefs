## Context

CubeFS 在 AI 训练场景下的数据读取路径经过多轮优化后，FUSE 模式已达到单块 SATA SSD 的硬件极限（408MB/s, 10260 IOPS）。当前集群每个客户端节点配置了 5 块 SSD 做 Bcache 缓存，但 FUSE 通道的架构性天花板（/dev/fuse 串行化）使得多盘带宽无法线性扩展。

CubeFS 已有 `client/gosdk` 用户态 SDK，提供了完整的文件操作接口（OpenFile/ReadFile/CloseFile/Readdir 等），可绕过 FUSE 内核模块直接读取数据。需要一个 benchmark 工具来量化 gosdk 直连路径相对 FUSE 路径的性能差距。

当前环境：
- 3 个客户端节点（207/208/209），每节点 5 块 SATA SSD（/bcache0~/bcache4）
- 数据集：300万文件 × 100KB = ~300GB，存储在 CubeFS volume "ltptest"
- Bcache-server 已运行，缓存数据已热
- FUSE baseline：10260 IOPS（单SSD打满）

## Goals / Non-Goals

**Goals:**
- 量化 gosdk 直连路径在 300 万文件场景下的读取 IOPS 和吞吐量
- 与 FUSE 模式 DLIO baseline（10260 IOPS）直接对比
- 验证 gosdk 路径能否突破 FUSE 通道的吞吐天花板（~700MB/s）
- 识别 gosdk 路径的新瓶颈（如果存在）
- 评估 gosdk 已知配置缺失对性能的实际影响

**Non-Goals:**
- 不修改 gosdk 代码（第一版先用现有 API 跑 baseline）
- 不修改 bcache-server 代码
- 不接入 DLIO（先用独立 Go 程序验证 I/O 上限）
- 不做生产化（这是一个内部 benchmark 工具，不需要完善的错误处理和文档）

## Decisions

### Decision 1: 使用 gosdk 而非 libsdk (CGO)

**选择**: 直接 import `client/gosdk` Go 包

**替代方案**: 编译 `client/libsdk` 为 `libcfs.so` 并通过 C 接口调用

**理由**:
- Go 原生调用，无 CGO 开销
- 不需要编译 c-shared 库
- 可以直接访问 gosdk 的所有内部类型
- benchmark 场景不需要跨语言调用
- gosdk 和 libsdk 底层逻辑基本相同

### Decision 2: 单进程多 goroutine 模型

**选择**: 单个 Go 进程，128 个 goroutine 并发读取

**替代方案**: 多个 Go 进程（模拟 DLIO 的 8 MPI 进程 × 16 线程）

**理由**:
- Go 的 goroutine 调度器天然高效，无需多进程
- 单进程共享同一个 gosdk Client 实例，元数据缓存效率最高
- 简化实现，减少进程间协调开销
- 128 goroutine 在 Go runtime 中可以轻松调度到所有 CPU 核心

### Decision 3: 文件列表通过 Readdir 获取而非硬编码

**选择**: 运行时通过 `client.OpenFile(dir) + dir.Readdir()` 获取文件列表

**替代方案**: 根据 DLIO 文件命名规则硬编码生成文件路径

**理由**:
- 更通用，不依赖特定的文件命名规则
- 验证 gosdk Readdir 在大目录（300万文件）下的性能
- 文件列表构建是一次性开销，不影响稳态读取性能

### Decision 4: 统计收集使用 atomic 计数器

**选择**: 使用 `sync/atomic` 的 int64 计数器收集 IOPS 和字节数

**替代方案**: 使用 channel 汇总或 sync.Mutex 保护的计数器

**理由**:
- atomic 操作无锁，对热路径性能影响最小
- 每个 goroutine 完成一个文件读取后 atomic.AddInt64
- 独立的统计 goroutine 每秒读取计数器并计算差值

### Decision 5: 代码位置在 cmd/libsdk_bench/

**选择**: 放在 `cmd/libsdk_bench/main.go`

**理由**:
- 遵循 CubeFS 项目结构（cmd/ 目录放可执行程序）
- 可以直接 `go build ./cmd/libsdk_bench/` 编译
- 自动使用项目的 go.mod 管理依赖

## Risks / Trade-offs

### [Risk] bcacheEncrypt 不匹配
gosdk 的 `NewBcacheClient()` 默认 encrypt=true，当前 bcache-server 配置 cacheEncrypt=false。
→ **Mitigation**: 先运行测试观察。如果 bcache 读取失败，gosdk 会回退到从 DataNode 读取。可以通过 iostat 观察 SSD 是否有读 I/O 来判断 bcache 是否工作。如果不工作，需要改 bcache-server 配置为 cacheEncrypt=true 并重新预热缓存，或修改 gosdk 代码。

### [Risk] closeStream EvictStream 导致 epoch 间性能退化
每次 CloseFile 都驱逐 Streamer，下个 epoch 重新 open 需要 getExtents RPC。
→ **Mitigation**: 通过 epoch 间 IOPS 对比量化影响。如果退化显著，后续可修改 gosdk 的 closeStream 行为，不执行 EvictStream（只读场景安全）。

### [Risk] 300 万文件 Readdir 可能超时或 OOM
一次 ReadDir_ll RPC 返回 300 万条目录项，MetaNode 端可能响应缓慢。
→ **Mitigation**: 使用 `-maxFiles` 参数限制读取数量（如 100 万），或分批 Readdir（如果 API 支持分页）。

### [Risk] 单 gosdk Client 的锁争用
128 goroutine 共享一个 Client 实例，fdmap/fdlock 可能成为热点。
→ **Mitigation**: gosdk 的 fdlock 是 RWMutex，读多写少场景下争用应该有限。如果成为瓶颈，可考虑多 Client 实例（每组 goroutine 一个 Client）。

### [Risk] 测试结果不可直接对比 DLIO
Go 程序和 Python DLIO 的行为不完全相同（无 batch 概念、无 DataLoader 开销）。
→ **Mitigation**: 这是已知限制。benchmark 的目标是验证 gosdk I/O 路径的上限，而非精确复刻 DLIO 行为。后续如需精确对比，可实现 DLIO 的 CubefsReader 插件。

## Open Questions

1. **bcacheEncrypt 不匹配是否真的会导致读取失败？** 需要实测验证。如果加密只是在存储路径上加了一层hash，可能不影响读取。
2. **Readdir 300 万文件的实际耗时？** 需要实测。如果超过分钟级，可能需要优化文件列表获取方式。
3. **gosdk 的 fdset 容量是否足够？** `maxFdNum = 10240000`，300万文件顺序读取（同时只打开128个）应该够用。
