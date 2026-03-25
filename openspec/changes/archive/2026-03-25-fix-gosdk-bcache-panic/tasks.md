## 1. 修复 nil channel panic（stream_writer.go）

- [x] 1.1 在 `evict()` 函数中为 `asyncFlushDone` 和 `asyncFlushCh` 添加 nil 检查（已在工作区完成）
- [x] 1.2 在 `abort()` 函数中为 `asyncFlushDone` 和 `asyncFlushCh` 添加 nil 检查（已在工作区完成）
- [x] 1.3 在 `server()` done 分支中为 `asyncFlushDone` 和 `asyncFlushCh` 添加 nil 检查（已在工作区完成）

## 2. 注册 OnGetInodeInfo 回调（gosdk cfs_client.go）

- [x] 2.1 在 `client/gosdk/cfs_client.go` 的 `Client.Start()` 中，查找 `ExtentConfig` 构建位置（line 258-275）
- [x] 2.2 参考 FUSE 模式（`client/fs/super.go` 中 `ec.GetInodeInfo = mw.InodeGet`），为 `ExtentConfig.OnGetInodeInfo` 设置回调 `mw.InodeGet_ll`
- [x] 2.3 确认回调签名匹配 `GetInodeInfoFunc` 类型定义：`func(ino uint64) (*proto.InodeInfo, error)`

## 3. 编译验证

- [x] 3.1 重新编译 `libsdk_bench`，确保无编译错误
- [x] 3.2 运行 `libsdk_bench -bcache=true` 完成 3 个 epoch，确认无 panic ✅
- [x] 3.3 对比 `bcache=true` 与 `bcache=false` 的 IOPS / BW 结果（见下方）

## 测试结果对比

| 模式 | Epoch 1 IOPS | Epoch 2 IOPS | Epoch 3 IOPS | Epoch 2 BW |
|------|-------------|-------------|-------------|-----------|
| bcache=false | 1,055 | 18,636 | 18,275 | 1,820 MB/s |
| bcache=true | 361 | 5,372 | 5,395 | 525 MB/s |
| FUSE baseline | — | 10,260 | — | ~1,000 MB/s |

**分析**：bcache=true 模式稳态 IOPS（5,400）远低于 bcache=false（18,600），原因是 gosdk 的 bcache encrypt=true（硬编码）导致每次读取都有 XOR 加解密开销 + bcache 写入开销。bcache=false 稳态数据来自 OS page cache，无额外开销。