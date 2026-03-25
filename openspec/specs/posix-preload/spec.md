## Purpose

通过 LD_PRELOAD 机制拦截 glibc 的 POSIX 文件操作，将 CubeFS 挂载点下的 I/O 请求透明转发到 libcfs.so（CubeFS SDK），绕过 FUSE 内核态开销。

## Requirements

### Requirement: POSIX open 拦截
系统 MUST 拦截 `open`、`open64`、`openat` 系统调用。当文件路径以 `CFS_MOUNT_POINT` 环境变量指定的前缀开头时，MUST 调用 `cfs_open` 打开文件并返回 `cfs_fd + FD_OFFSET`（FD_OFFSET = 100000）。当路径不匹配时，MUST 透传到真实 libc `open` 函数。

#### Scenario: 打开 CubeFS 路径文件
- **WHEN** 调用 `open("/mnt/cfs/mlperf_data/train/file001.tfrecord", O_RDONLY, 0)`，且 `CFS_MOUNT_POINT=/mnt/cfs/`
- **THEN** 系统调用 `cfs_open(client_id, "/mlperf_data/train/file001.tfrecord", O_RDONLY, 0)` 并返回 `cfs_fd + 100000`

#### Scenario: 打开非 CubeFS 路径文件
- **WHEN** 调用 `open("/tmp/log.txt", O_RDONLY, 0)`
- **THEN** 系统透传到真实 libc `open`，返回系统原始 fd（< 100000）

#### Scenario: openat 相对路径处理
- **WHEN** 调用 `openat(AT_FDCWD, "/mnt/cfs/data/file.bin", O_RDONLY, 0)`
- **THEN** 系统识别绝对路径前缀，调用 `cfs_open` 处理

### Requirement: POSIX pread 拦截
系统 MUST 拦截 `pread`、`pread64` 系统调用。当 fd >= FD_OFFSET 时，MUST 调用 `cfs_read(client_id, cfs_fd, buf, size, offset)` 读取数据。当 fd < FD_OFFSET 时，MUST 透传到真实 libc `pread`。

#### Scenario: 从 CubeFS 文件 pread
- **WHEN** 调用 `pread(100003, buf, 102400, 0)`，且 fd 100003 对应 cfs_fd=3
- **THEN** 系统调用 `cfs_read(client_id, 3, buf, 102400, 0)` 并返回实际读取字节数

#### Scenario: 从系统文件 pread
- **WHEN** 调用 `pread(5, buf, 4096, 0)`
- **THEN** 系统透传到真实 libc `pread`

### Requirement: POSIX read 拦截
系统 MUST 拦截 `read` 系统调用。当 fd >= FD_OFFSET 时，MUST 使用 fd 映射表中维护的 offset 调用 `cfs_read`，并在读取完成后原子更新 offset。

#### Scenario: 顺序 read CubeFS 文件
- **WHEN** 连续调用两次 `read(100003, buf, 1024)`，文件初始 offset=0
- **THEN** 第一次 `cfs_read(client_id, 3, buf, 1024, 0)` 返回 1024，offset 更新为 1024；第二次 `cfs_read(client_id, 3, buf, 1024, 1024)` 返回 1024，offset 更新为 2048

### Requirement: POSIX close 拦截
系统 MUST 拦截 `close` 系统调用。当 fd >= FD_OFFSET 时，MUST 调用 `cfs_close(client_id, cfs_fd)` 关闭文件并释放 fd 映射表条目。

#### Scenario: 关闭 CubeFS 文件
- **WHEN** 调用 `close(100003)`
- **THEN** 系统调用 `cfs_close(client_id, 3)` 并将 fd_table[3].valid 设为 false

#### Scenario: 关闭系统文件
- **WHEN** 调用 `close(5)`
- **THEN** 系统透传到真实 libc `close`

### Requirement: POSIX stat/fstat 拦截
系统 MUST 拦截 `stat`、`lstat`、`fstat`、`__xstat`、`__fxstat` 系统调用。对于路径参数的 stat/lstat，当路径匹配 CFS_MOUNT_POINT 前缀时，MUST 调用 `cfs_getattr` 获取文件属性。对于 fd 参数的 fstat，当 fd >= FD_OFFSET 时，MUST 通过 fd 映射表中记录的路径调用 `cfs_getattr`。

#### Scenario: stat CubeFS 文件
- **WHEN** 调用 `stat("/mnt/cfs/mlperf_data/train/file001.tfrecord", &st)`
- **THEN** 系统调用 `cfs_getattr(client_id, "/mlperf_data/train/file001.tfrecord", &cfs_stat)` 并将 `cfs_stat_info` 转换为 `struct stat`

#### Scenario: fstat CubeFS fd
- **WHEN** 调用 `fstat(100003, &st)`，fd_table[3].path = "/mlperf_data/train/file001.tfrecord"
- **THEN** 系统通过记录的路径调用 `cfs_getattr` 并返回转换后的 `struct stat`

### Requirement: POSIX lseek 拦截
系统 MUST 拦截 `lseek`、`lseek64` 系统调用。当 fd >= FD_OFFSET 时，MUST 在本地 fd 映射表中更新 offset，不发起网络请求。支持 SEEK_SET、SEEK_CUR、SEEK_END。

#### Scenario: lseek SEEK_SET
- **WHEN** 调用 `lseek(100003, 4096, SEEK_SET)`
- **THEN** 系统将 fd_table[3].offset 设为 4096，返回 4096

#### Scenario: lseek SEEK_END
- **WHEN** 调用 `lseek(100003, 0, SEEK_END)`，文件大小 102400
- **THEN** 系统将 fd_table[3].offset 设为 102400，返回 102400

### Requirement: 目录操作拦截
系统 SHOULD 拦截 `opendir`、`readdir`、`closedir` 系统调用。当路径匹配 CFS_MOUNT_POINT 前缀时，MUST 使用 `cfs_open`（目录模式）和 `cfs_readdir` 实现目录遍历，并将 `cfs_dirent` 转换为标准 `struct dirent`。

#### Scenario: 遍历 CubeFS 目录
- **WHEN** 调用 `opendir("/mnt/cfs/mlperf_data/train/")` 后循环 `readdir`
- **THEN** 系统通过 `cfs_readdir` 返回目录中的所有文件条目，每个条目包含正确的 `d_name`、`d_type`、`d_ino`

### Requirement: CubeFS 客户端生命周期管理
系统 MUST 在进程启动时（`__attribute__((constructor))`）完成 CubeFS 客户端初始化，在进程退出时（`__attribute__((destructor))`）释放资源。初始化 MUST 通过环境变量读取配置。

#### Scenario: 进程启动初始化
- **WHEN** 进程加载 libcfs_preload.so（通过 LD_PRELOAD）
- **THEN** constructor 依次执行：保存真实 libc 函数指针 → dlopen libcfs.so → 读取 CFS_MASTER/CFS_VOL/CFS_AK/CFS_SK/CFS_BCACHE_DIRS/CFS_MOUNT_POINT 环境变量 → cfs_new_client → cfs_set_client × N → cfs_start_client → 初始化 fd 映射表

#### Scenario: 缺少必要环境变量
- **WHEN** CFS_MASTER 或 CFS_VOL 环境变量未设置
- **THEN** constructor 输出错误日志到 stderr，设置全局标志禁用拦截，所有 POSIX 调用透传到真实 libc

#### Scenario: 进程正常退出
- **WHEN** 进程退出触发 destructor
- **THEN** 系统关闭所有未关闭的 cfs fd，调用 cfs_close_client 释放资源，dlclose libcfs.so

### Requirement: fd 空间隔离
系统 MUST 使用高位偏移法隔离 CubeFS fd 与系统 fd。FD_OFFSET MUST 为 100000。fd 映射表 MUST 支持最大 MAX_CFS_FDS（默认 65536）个并发打开的 CubeFS 文件。

#### Scenario: fd 偏移正确性
- **WHEN** `cfs_open` 返回 fd=42
- **THEN** 拦截层返回给调用方的 fd 为 100042

#### Scenario: fd 空间不冲突
- **WHEN** 进程同时打开系统文件（fd=3）和 CubeFS 文件（返回 fd=100005）
- **THEN** 对 fd=3 的操作透传到 libc，对 fd=100005 的操作转发到 libcfs.so，互不干扰

### Requirement: 线程安全
系统 MUST 保证所有拦截函数在多线程环境下的安全性。fd 映射表的 open/close 操作 MUST 使用写锁，read/pread/stat 操作 MUST 使用读锁。pread 的 offset 参数自带，不需要额外的 offset 同步。

#### Scenario: 16 线程并发 pread
- **WHEN** 16 个 TensorFlow read_threads 同时对不同 CubeFS fd 执行 pread
- **THEN** 所有 pread 并发执行无阻塞（读锁不互斥），返回正确数据

#### Scenario: 并发 open 和 pread
- **WHEN** 一个线程执行 open（写锁），其他线程执行 pread（等待读锁）
- **THEN** open 持有写锁期间 pread 阻塞，open 完成后 pread 恢复并发

### Requirement: 运行时禁用开关
系统 SHOULD 支持通过 `CFS_PRELOAD_DISABLE=1` 环境变量在运行时禁用拦截功能，所有 POSIX 调用直接透传到真实 libc。

#### Scenario: 禁用拦截
- **WHEN** 设置 `CFS_PRELOAD_DISABLE=1` 后启动进程
- **THEN** constructor 跳过 CubeFS 客户端初始化，所有文件操作透传到 libc

### Requirement: 性能指标
系统 MUST 满足以下性能要求：
- 单次 pread 拦截额外延迟 MUST < 1μs（不含实际 I/O）
- fd 判断（整数比较）延迟 MUST < 10ns
- 单节点读带宽 MUST >= 1,200 MB/s（对比 FUSE 模式 ~800 MB/s，提升 >= 50%）
- 单节点 IOPS MUST >= 12,000（100KB 文件随机读）

#### Scenario: 拦截延迟验证
- **WHEN** 连续 pread 100 万次 CubeFS 文件
- **THEN** 拦截层本身的平均额外延迟（不含 cfs_read I/O 时间）< 1μs

#### Scenario: 带宽达标验证
- **WHEN** 使用 DLIO benchmark 128 batch_size × 16 read_threads 读取 150 万文件
- **THEN** 单节点总读带宽 >= 1,200 MB/s
