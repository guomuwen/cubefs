## Context

当前 DLIO benchmark 通过 FUSE 挂载（`/mnt/cfs/`）访问 CubeFS，I/O 路径为：

```
TensorFlow C++ → glibc open/pread/close → syscall → VFS → FUSE → cfs-client → MetaNode/DataNode
```

FUSE 协议引入两次内核态/用户态切换和数据拷贝，导致单节点读带宽上限约 800 MB/s。

libsdk_bench 已验证 gosdk 直接接入可达 ~15,000 IOPS / ~1,500 MB/s，但 DLIO 的数据读取发生在 TensorFlow C++ 层（`tf.data.TFRecordDataset` 内部通过 `PosixFileSystem` 调用 `pread`），无法在 Python 或 TF API 层面替换。

CubeFS 已有完整的 C API（`libcfs.so`），导出了 `cfs_open`、`cfs_read`、`cfs_close`、`cfs_getattr`、`cfs_readdir` 等函数，可作为 LD_PRELOAD 的转发目标。

### 部署环境

- 3 台计算节点：7.32.209.204 / 205 / 206，每台 8 个 MPI rank
- MPI 总进程数：24（`-np 24`），每个 rank 内 16 个 TF read_threads
- 最大并发读线程数：24 × 16 = 384
- 每台节点 5 块 bcache SSD（`/bcache0` ~ `/bcache4`）
- 数据集：300 万文件 × 100 KB = ~300 GB（TFRecord 格式）

## Goals / Non-Goals

**Goals:**

- 通过 LD_PRELOAD 透明拦截 POSIX I/O，对 DLIO/TensorFlow 零代码修改
- 将 DLIO 的文件读取路径从 FUSE 切换到 libcfs.so（gosdk），消除内核态开销
- 支持 MPI 多进程部署，每个进程独立初始化 CubeFS client
- 复用已有的 bcache IPC-bypass 和 extent cache pool 优化
- 提供开箱即用的部署脚本和配置模板

**Non-Goals:**

- 不修改 TensorFlow 或 DLIO 源码
- 不实现写操作拦截（DLIO 场景为纯读）
- 不支持 mmap（TFRecordDataset 使用 pread，不使用 mmap）
- 不做通用的 POSIX 完整兼容（只覆盖 DLIO 实际使用的系统调用子集）
- 不做跨卷访问（单进程单卷）

## Decisions

### 决策 1：LD_PRELOAD 拦截 vs TensorFlow FileSystem 插件

**选择**：LD_PRELOAD

| 维度 | LD_PRELOAD | TF FileSystem 插件 |
|------|-----------|-------------------|
| 对上层侵入 | 零（不改代码，不改路径） | 低（需改 URI 前缀为 `cfs:///`） |
| 实现语言 | C（~300 行） | C++（需 TF 编译环境） |
| 通用性 | 适用于任何 POSIX 程序 | 仅 TensorFlow |
| 编译依赖 | 仅 gcc + libdl | TF 源码 + bazel |
| 风险 | 路径匹配需精确 | TF 版本兼容性 |

**理由**：DLIO 使用 MPI + TF 的组合，LD_PRELOAD 是唯一不需要修改上层代码的方案，且实现简单、编译无额外依赖。

### 决策 2：fd 空间隔离策略

**选择**：高位偏移法（CFS fd = cfs_open 返回值 + FD_OFFSET，FD_OFFSET = 100000）

```
系统 fd 空间：  0 ~ 99,999     → 透传到真实 libc
CubeFS fd 空间：100,000+       → 转发到 libcfs.so
```

**替代方案**：哈希表映射（fd→cfs_fd），但引入额外的查找开销和锁竞争。
**理由**：偏移法判断只需一次整数比较（`fd >= FD_OFFSET`），零额外开销，且 Linux 默认 fd 上限远低于 100,000。

### 决策 3：libcfs.so 加载方式

**选择**：`dlopen` 动态加载（非编译时链接）

```c
// constructor 中加载
void *libcfs_handle = dlopen("libcfs.so", RTLD_NOW);
cfs_new_client = dlsym(libcfs_handle, "cfs_new_client");
cfs_open       = dlsym(libcfs_handle, "cfs_open");
// ...
```

**理由**：
- 避免 preload 库和 libcfs.so 的编译时耦合
- libcfs.so 包含 Go runtime，体积大（~50 MB），动态加载更灵活
- 部署时只需确保 `LD_LIBRARY_PATH` 包含 libcfs.so 路径

### 决策 4：线程安全设计

**选择**：fd 映射表使用 `pthread_rwlock`（读写锁）

```
┌─────────────────────────────────────────────────┐
│  fd 映射表 (全局数组)                             │
│                                                 │
│  fd_table[MAX_CFS_FDS]:                         │
│    [0] = { cfs_fd=3,  offset=0,    valid=true } │
│    [1] = { cfs_fd=7,  offset=4096, valid=true } │
│    [2] = { valid=false }                        │
│    ...                                          │
│                                                 │
│  open/close: pthread_rwlock_wrlock (写锁)        │
│  read/pread/stat: pthread_rwlock_rdlock (读锁)   │
│                                                 │
│  pread 不需要锁 offset（参数自带 offset）          │
│  read 需要 atomic offset 更新                    │
└─────────────────────────────────────────────────┘
```

**理由**：DLIO 场景读远多于写（每个文件 open 一次、pread 多次），读写锁允许多个 pread 并发无阻塞。TFRecordDataset 使用 `pread`（自带 offset），不需要维护 fd 的 current offset，进一步减少锁竞争。

### 决策 5：路径匹配策略

**选择**：环境变量 `CFS_MOUNT_POINT` 配置前缀（如 `/mnt/cfs/`）

```c
int hooked_open(const char *path, int flags, mode_t mode) {
    if (strncmp(path, cfs_mount_point, cfs_mount_point_len) == 0) {
        // 截取 CubeFS 内部路径：/mnt/cfs/mlperf_data/... → /mlperf_data/...
        const char *cfs_path = path + cfs_mount_point_len - 1;
        int cfs_fd = real_cfs_open(client_id, cfs_path, flags, mode);
        return cfs_fd + FD_OFFSET;
    }
    return real_open(path, flags, mode);  // 透传
}
```

**理由**：严格前缀匹配确保只拦截 CubeFS 路径，Python 模块加载、TF 模型文件、日志写入等非 CubeFS I/O 不受影响。

## 整体架构

```
┌─────────────────────────────────────────────────────────────┐
│  Node 204/205/206 (每台 8 个 MPI Rank)                       │
│                                                             │
│  ┌───────────────────────────────────────────────────────┐  │
│  │  MPI Rank (Python 进程)                                │  │
│  │  ┌─────────────────────────────────────────────────┐  │  │
│  │  │  DLIO / TensorFlow C++                          │  │  │
│  │  │  tf.data.TFRecordDataset                        │  │  │
│  │  │  16 × read_threads (pread 并发)                  │  │  │
│  │  └──────────────┬──────────────────────────────────┘  │  │
│  │                 │ open/pread/close/stat                │  │
│  │  ┌──────────────▼──────────────────────────────────┐  │  │
│  │  │  libcfs_preload.so (LD_PRELOAD)                 │  │  │
│  │  │                                                 │  │  │
│  │  │  path.startsWith(CFS_MOUNT_POINT) ?             │  │  │
│  │  │  ┌─────────┐           ┌──────────────────┐    │  │  │
│  │  │  │  YES    │           │  NO (透传)         │    │  │  │
│  │  │  │  ↓      │           │  ↓                │    │  │  │
│  │  │  │ libcfs  │           │ real libc         │    │  │  │
│  │  │  │ .so     │           │ (dlsym RTLD_NEXT) │    │  │  │
│  │  │  └────┬────┘           └──────────────────┘    │  │  │
│  │  └───────┼─────────────────────────────────────────┘  │  │
│  │          │                                            │  │
│  │  ┌───────▼──────────────────────────────────────────┐ │  │
│  │  │  gosdk (Go runtime in-process)                   │ │  │
│  │  │  ┌────────────┐  ┌──────────┐  ┌─────────────┐  │ │  │
│  │  │  │ MetaWrapper│  │ Extent   │  │ bcache      │  │ │  │
│  │  │  │ + IC + DC  │  │ Client   │  │ IPC-bypass  │  │ │  │
│  │  │  └─────┬──────┘  └────┬─────┘  └──────┬──────┘  │ │  │
│  │  └────────┼──────────────┼───────────────┼──────────┘ │  │
│  └───────────┼──────────────┼───────────────┼────────────┘  │
│              │TCP           │TCP            │Local SSD      │
│  ┌───────────▼──┐  ┌───────▼────┐  ┌───────▼──────────┐   │
│  │  MetaNode    │  │  DataNode  │  │ /bcache0~/bcache4 │   │
│  └──────────────┘  └────────────┘  └──────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

## POSIX API 拦截映射

```
┌──────────────┬────────────────────┬──────────────────────────┐
│ POSIX API    │ libcfs.so C API    │ 说明                      │
├──────────────┼────────────────────┼──────────────────────────┤
│ open/openat  │ cfs_open           │ 返回 cfs_fd + FD_OFFSET   │
│ open64       │ cfs_open           │ 同上（大文件支持）          │
│ close        │ cfs_close          │ 释放 fd 映射表条目          │
│ read         │ cfs_read           │ 使用维护的 offset          │
│ pread/pread64│ cfs_read           │ 参数自带 offset，最高频     │
│ lseek/lseek64│ (本地 offset 表)    │ 仅更新本地 offset          │
│ fstat/__fxstat│cfs_getattr(by fd) │ 需要 fd→path 反查          │
│ stat/lstat   │ cfs_getattr        │ 直接传路径                 │
│ opendir      │ cfs_open (目录)     │ 返回包装的 DIR*            │
│ readdir      │ cfs_readdir        │ 遍历目录条目               │
│ closedir     │ cfs_close          │ 释放目录 fd                │
└──────────────┴────────────────────┴──────────────────────────┘
```

## 进程生命周期

```
进程启动
    │
    ▼
__attribute__((constructor)) cfs_preload_init()
    │
    ├── 1. dlsym(RTLD_NEXT, "open") → 保存真实 libc 函数指针
    ├── 2. dlopen("libcfs.so") → 加载 CubeFS SDK
    ├── 3. 读取环境变量（CFS_MASTER/VOL/AK/SK/BCACHE_DIRS/MOUNT_POINT）
    ├── 4. cfs_new_client() → 获取 client_id
    ├── 5. cfs_set_client() × N → 配置各项参数
    ├── 6. cfs_start_client() → 启动 gosdk（连接集群、初始化 bcache）
    └── 7. 初始化 fd 映射表 + rwlock
    │
    ▼
正常运行（拦截 POSIX 调用）
    │
    ▼
__attribute__((destructor)) cfs_preload_fini()
    │
    ├── 关闭所有未关闭的 cfs fd
    ├── cfs_close_client() → 释放 gosdk 资源
    └── dlclose(libcfs_handle)
```

## Risks / Trade-offs

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| LD_PRELOAD 拦截到非目标 I/O | Python 模块加载、日志写入异常 | 严格路径前缀匹配，只拦截 `CFS_MOUNT_POINT` 下的路径 |
| Go runtime 内存开销 | 每 MPI rank +20 MB | 24 rank × 20 MB = 480 MB，相对 GPU 内存可接受 |
| Go runtime 与 fork 不兼容 | MPI fork 新进程后 Go runtime 状态不一致 | MPI 使用 `exec` 模式（`plm_rsh`），每个 rank 是独立 exec 的新进程 |
| fstat 需要 fd→path 反查 | 需要额外维护 fd→path 映射 | fd 映射表增加 path 字段，open 时记录 |
| TF 内部使用非标准 I/O | 可能调用未拦截的系统调用 | 已确认 TFRecordDataset 只使用 open/pread/close/fstat |
| 多线程 fd 表锁竞争 | pread 高并发下性能下降 | pread 本身不需要锁 offset（参数自带），fd 查找用数组直接索引（O(1)）|

### 降级方案

如果 LD_PRELOAD 模式遇到兼容性问题：
1. **降级到 FUSE**：去掉 `LD_PRELOAD` 环境变量即可回退到原始 FUSE 挂载，零成本切换
2. **降级到部分拦截**：通过 `CFS_PRELOAD_DISABLE=1` 环境变量可在运行时禁用拦截

## Open Questions

1. **fstat 的实现方式**：`cfs_getattr` 需要路径参数，但 `fstat` 只有 fd。需要在 fd 映射表中记录 path，或者在 libcfs.so 中新增 `cfs_fstat(client_id, fd)` 接口——待确认是否需要修改 libcfs.so
2. **opendir/readdir 的 DIR* 封装**：glibc 的 `DIR*` 是不透明结构体，需要自定义包装。DLIO 是否使用 `opendir/readdir`（还是只通过 Python `os.listdir`）需要验证
3. **TFRecordDataset 的 buffer_size 参数**：是否会导致一次 `pread` 请求大于单个文件大小，需要确认 cfs_read 对超长请求的处理
