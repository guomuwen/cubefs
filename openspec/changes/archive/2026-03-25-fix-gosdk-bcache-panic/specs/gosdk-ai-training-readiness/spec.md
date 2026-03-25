## MODIFIED Requirements

### Requirement: gosdk 已知配置缺失清单
gosdk 在 AI 训练场景下存在以下已知的配置缺失，这些缺失 SHALL 被记录为已知限制，并在 benchmark 测试结果中评估其对性能的实际影响。

| 配置项 | FUSE模式值 | gosdk当前值 | 影响 | 状态 |
|--------|-----------|------------|------|------|
| OnGetInodeInfo | mw.InodeGet | **未设置（nil）** → **已修复** | bcache 读路径 panic | ✅ 已修复 |
| bcacheEncrypt | false（可配） | true（硬编码） | 加密开销 + 可能导致读取失败 | 已知限制 |
| InodeCache TTL | 600s（可配） | 120s（硬编码） | 缓存过期后重新RPC | 已知限制 |
| DentryCache | v2版（带TTL） | v1版（简单版） | Dentry缓存策略较弱 | 已知限制 |
| maxStreamerLimit | 500000（可配） | 默认值 | Streamer 可能被频繁淘汰 | 已知限制 |
| extentCachePoolSize | 3200000（可配） | 不支持 | ExtentCache 池化无法生效 | 已知限制 |
| DisableMetaCache | false | true（硬编码） | asyncBlockCache 不启动 | 已知限制 |

#### Scenario: bcacheEncrypt 不匹配导致读取失败
- **WHEN** gosdk 使用 `NewBcacheClient()`（encrypt=true）访问 bcache-server（cacheEncrypt=false）缓存的数据
- **THEN** 如果读取失败（数据解密错误），SHALL 回退到从 DataNode 读取，benchmark 结果反映的是无 bcache 加速的性能

#### Scenario: bcacheEncrypt 匹配正常工作
- **WHEN** gosdk 使用 `NewBcacheClient()`（encrypt=true）且 bcache-server 也配置了 cacheEncrypt=true
- **THEN** bcache 正常命中，benchmark 结果反映有 bcache 加速的性能

#### Scenario: OnGetInodeInfo 回调已注册
- **WHEN** gosdk client 初始化完成且 `EnableBcache=true`
- **THEN** `ExtentClient.getInodeInfo` SHALL 为非 nil 函数，bcache 填充路径可正常查询 inode StorageClass
