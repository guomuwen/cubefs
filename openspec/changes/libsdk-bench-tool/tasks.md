## 0. 环境准备

- [x] 0.1 修改 `client/gosdk/cfs_client.go` 的 `openStream` 方法，将 `c.ec.OpenStream(f.ino, openForWrite, isCache, fullPath)` 改为 `c.ec.OpenStreamWithCache(f.ino, c.cfg.EnableBcache, openForWrite, isCache, fullPath)`
- [x] 0.2 ~~修改 `sdk/data/stream/extent_client.go`~~ **经分析无需修改**：`NewStreamer`（stream_reader.go:156）已无条件启动 `asyncBlockCache` goroutine；`disableMetaCache=true` 模式下不启动自动 evict，因此 streamer 不会被关闭后重建，第633行的条件不会被触发。原始代码在首次创建 streamer 时已正确工作。
- [ ] 0.3 验证修复：编译 gosdk 后用简单测试读取一个已缓存在 bcache 中的文件，通过 iostat 确认 SSD 有读 I/O

## 1. 项目结构搭建

- [x] 1.1 创建 `cmd/libsdk_bench/` 目录
- [x] 1.2 创建 `cmd/libsdk_bench/main.go`，定义 package main 和基本 import

## 2. 命令行参数解析

- [x] 2.1 使用 `flag` 包定义所有命令行参数（master, vol, ak, sk, dir, workers, fileSize, maxFiles, epochs, shuffle, bcache, logDir, logLevel）
- [x] 2.2 实现必填参数校验，缺少时输出用法说明并退出

## 3. 客户端初始化

- [x] 3.1 实现 gosdk.New(Config) + client.Start() 初始化流程
- [x] 3.2 输出初始化成功信息（volume, cluster）
- [x] 3.3 处理初始化失败（输出错误信息并退出）

## 4. 文件列表获取

- [x] 4.1 通过 client.OpenFile(dir, O_RDONLY, 0) 打开数据目录
- [x] 4.2 调用 dir.Readdir() 获取所有文件条目（支持大目录，分批读取）
- [x] 4.3 过滤出普通文件（DType == DT_REG），构建完整路径列表
- [x] 4.4 如果设置了 maxFiles，截取前 maxFiles 个文件
- [x] 4.5 输出文件数量和获取耗时

## 5. 性能统计模块

- [x] 5.1 定义 atomic int64 计数器：completedFiles, completedBytes, errorCount
- [x] 5.2 实现统计 goroutine：每秒读取计数器差值，计算并输出 IOPS 和 BW
- [x] 5.3 实现 epoch 汇总统计（平均 IOPS, 平均 BW, 总耗时）
- [x] 5.4 实现最终汇总输出（所有 epoch 对比表 + 跨 epoch 平均值）

## 6. 并发读取引擎

- [x] 6.1 实现文件列表分片：将文件均匀分配到 N 个 worker
- [x] 6.2 实现 worker goroutine：循环执行 OpenFile→ReadFile→CloseFile
- [x] 6.3 每个 worker 预分配 fileSize 大小的 buf 并复用
- [x] 6.4 每完成一个文件，atomic.AddInt64 更新计数器
- [x] 6.5 实现 epoch 循环：每个 epoch 开始前可选 shuffle，启动所有 worker，等待 WaitGroup 完成
- [x] 6.6 错误处理：OpenFile/ReadFile 失败时记录错误计数，不中断其他 worker

## 7. 编译验证

- [x] 7.1 执行 `go build ./cmd/libsdk_bench/` 确认编译通过
- [x] 7.2 本地小规模测试（少量文件 + 少量 worker）验证基本流程