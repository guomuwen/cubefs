## ADDED Requirements

### Requirement: 命令行参数配置
libsdk-bench 工具 SHALL 支持以下命令行参数，允许用户配置测试工作负载：

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `-master` | string | 必填 | Master节点地址，逗号分隔 |
| `-vol` | string | 必填 | Volume 名称 |
| `-ak` | string | 必填 | AccessKey |
| `-sk` | string | 必填 | SecretKey |
| `-dir` | string | 必填 | 数据目录路径（相对volume根） |
| `-workers` | int | 128 | 并发 goroutine 数量 |
| `-fileSize` | int | 102400 | 每个文件读取的字节数 |
| `-maxFiles` | int | 0(全部) | 最大读取文件数，0表示读取目录下所有文件 |
| `-epochs` | int | 5 | 读取轮数 |
| `-shuffle` | bool | true | 是否随机打乱文件顺序 |
| `-bcache` | bool | true | 是否启用 Bcache 加速 |
| `-logDir` | string | "/cfs/libsdk/log" | 日志目录 |
| `-logLevel` | string | "error" | 日志级别 |

#### Scenario: 最小必填参数启动
- **WHEN** 用户提供 `-master`, `-vol`, `-ak`, `-sk`, `-dir` 五个必填参数
- **THEN** 工具使用默认值启动，128 goroutine 并发读取目录下所有文件，执行5个epoch

#### Scenario: 缺少必填参数
- **WHEN** 用户缺少任一必填参数
- **THEN** 工具输出用法说明并退出，返回非零退出码

### Requirement: 客户端初始化
工具启动时 SHALL 通过 gosdk.New() 创建 CubeFS 客户端并调用 client.Start() 完成初始化，包括连接 Master、MetaNode、DataNode，以及权限校验（AK/SK）。

#### Scenario: 成功初始化
- **WHEN** 提供正确的 master 地址和 AK/SK
- **THEN** 客户端成功连接集群，输出 "Client initialized, volume: <vol>, cluster: <cluster>"

#### Scenario: 初始化失败
- **WHEN** Master 地址不可达或 AK/SK 错误
- **THEN** 输出错误信息并退出，返回非零退出码

### Requirement: 文件列表获取
工具 SHALL 通过 gosdk 的 OpenFile + Readdir 接口读取指定数据目录下的所有文件名，构建完整的文件路径列表。

#### Scenario: 扁平目录读取
- **WHEN** 指定的数据目录下包含 N 个文件（无子目录）
- **THEN** 工具获取全部 N 个文件路径，如果设置了 `-maxFiles` 则取前 maxFiles 个

#### Scenario: 大目录读取
- **WHEN** 目录下有 300 万个文件
- **THEN** Readdir 能成功返回所有文件条目，输出 "Found <N> files in <dir>, took <duration>"

### Requirement: 并发读取执行
工具 SHALL 将文件列表均匀分配到 N 个 goroutine，每个 goroutine 独立执行 open→read→close 循环。每个 epoch 读取所有分配的文件一遍。

#### Scenario: 单 epoch 完整读取
- **WHEN** 启动 128 个 worker，共 100 万个文件
- **THEN** 每个 worker 负责 100万/128 ≈ 7812 个文件，全部读取完成后 epoch 结束

#### Scenario: 多 epoch 循环
- **WHEN** 设置 epochs=5
- **THEN** 工具执行 5 轮完整读取，每轮开始前如果 shuffle=true 则重新打乱文件顺序

#### Scenario: 文件读取流程
- **WHEN** worker 读取一个文件
- **THEN** 执行 client.OpenFile(path, O_RDONLY, 0) → f.ReadFile(buf, 0) → f.CloseFile()，buf 大小为 fileSize 参数指定值

### Requirement: 实时性能统计
工具 SHALL 在测试执行期间每秒输出性能指标，每个 epoch 结束后输出该 epoch 的汇总统计。

#### Scenario: 每秒统计输出
- **WHEN** 测试正在运行
- **THEN** 每秒输出一行格式为: `[Epoch X] <elapsed>s: IOPS=<n> BW=<n>MB/s Files=<completed>/<total>`

#### Scenario: Epoch 汇总输出
- **WHEN** 一个 epoch 完成
- **THEN** 输出汇总: `[Epoch X] Summary: IOPS=<avg> BW=<avg>MB/s Duration=<sec>s Files=<total>`

#### Scenario: 最终汇总输出
- **WHEN** 所有 epoch 完成
- **THEN** 输出所有 epoch 的 IOPS 和 BW 对比表，以及跨 epoch 的平均值

### Requirement: buf 复用
每个 goroutine SHALL 预分配一个 fileSize 大小的 byte buffer 并在所有文件读取中复用，避免每次读取分配新内存导致 GC 压力。

#### Scenario: buffer 复用
- **WHEN** 一个 worker 读取 7812 个文件
- **THEN** 使用同一个 buf 读取所有文件，整个过程不额外分配 read buffer
