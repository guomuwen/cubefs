## ADDED Requirements

### Requirement: gosdk 已知配置缺失清单
gosdk 在 AI 训练场景下存在以下已知的配置缺失，这些缺失 SHALL 被记录为已知限制，并在 benchmark 测试结果中评估其对性能的实际影响。

| 配置项 | FUSE模式值 | gosdk当前值 | 影响 |
|--------|-----------|------------|------|
| bcacheEncrypt | false（可配） | true（硬编码） | 加密开销 + 可能导致读取失败 |
| InodeCache TTL | 600s（可配） | 120s（硬编码） | 缓存过期后重新RPC |
| DentryCache | v2版（带TTL） | v1版（简单版） | Dentry缓存策略较弱 |
| maxStreamerLimit | 500000（可配） | 默认值 | Streamer 可能被频繁淘汰 |
| extentCachePoolSize | 3200000（可配） | 不支持 | ExtentCache 池化无法生效 |
| DisableMetaCache | false | true（硬编码） | asyncBlockCache 不启动 |

#### Scenario: bcacheEncrypt 不匹配导致读取失败
- **WHEN** gosdk 使用 `NewBcacheClient()`（encrypt=true）访问 bcache-server（cacheEncrypt=false）缓存的数据
- **THEN** 如果读取失败（数据解密错误），SHALL 回退到从 DataNode 读取，benchmark 结果反映的是无 bcache 加速的性能

#### Scenario: bcacheEncrypt 匹配正常工作
- **WHEN** gosdk 使用 `NewBcacheClient()`（encrypt=true）且 bcache-server 也配置了 cacheEncrypt=true
- **THEN** bcache 正常命中，benchmark 结果反映有 bcache 加速的性能

### Requirement: closeStream EvictStream 行为评估
gosdk 的 CloseFile 方法 SHALL 调用 closeStream，其中包含 `ec.EvictStream(f.ino)` 操作，每次关闭文件后驱逐 Streamer。此行为对多 epoch 场景的影响 SHALL 通过 benchmark 的 epoch 间 IOPS 差异来评估。

#### Scenario: 首个 epoch 建立 Streamer
- **WHEN** 第一个 epoch 开始，所有文件首次被 open
- **THEN** 每个文件的 openStream 创建新 Streamer + 执行 getExtents RPC

#### Scenario: epoch 间 Streamer 被驱逐
- **WHEN** 第一个 epoch 完成，所有文件已被 close（Streamer 被驱逐）
- **THEN** 第二个 epoch 重新 open 同一文件时，需要再次创建 Streamer + getExtents RPC

#### Scenario: 性能退化量化
- **WHEN** 对比 epoch 1 和 epoch 2-5 的 IOPS
- **THEN** 如果 epoch 间 IOPS 无显著差异，说明 getExtents RPC 不是当前瓶颈；如果 epoch 2-5 的 IOPS 显著低于 epoch 1 的首次缓存建立后的水平，说明 EvictStream 是需要优化的点

### Requirement: benchmark 结果可对比性
libsdk-bench 的测试参数 SHALL 与 FUSE 模式下的 DLIO 测试参数对齐，确保结果可直接对比。

#### Scenario: 参数对齐
- **WHEN** 执行 libsdk-bench 测试
- **THEN** 以下参数与 FUSE DLIO baseline 一致：并发度=128、文件大小=100KB、文件数量=100万/节点、epoch数=5

#### Scenario: 环境对齐
- **WHEN** 执行 libsdk-bench 测试
- **THEN** 在相同的客户端节点上运行，使用相同的 bcache SSD 缓存数据，访问相同的 CubeFS volume 和数据集
