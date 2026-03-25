## Why

MLPerf I/O 基准测试（DLIO benchmark）当前通过 FUSE 挂载方式访问 CubeFS，受限于 FUSE 协议的内核态/用户态切换和序列化开销，单节点读带宽瓶颈约 800 MB/s。libsdk_bench 已验证通过 libsdk（gosdk）直接接入 CubeFS 可达到 ~1,500 MB/s（1.8 倍提升），但 DLIO 使用 TensorFlow 的 `tf.data.TFRecordDataset` 在 C++ 层直接调用 POSIX I/O（`open/pread/close`），无法在 Python 层面替换文件系统后端。需要一种对 DLIO/TensorFlow 零侵入的方式将底层 I/O 路径从 FUSE 切换到 libsdk。

## What Changes

- **新增 `libcfs_preload.so` 共享库**：通过 `LD_PRELOAD` 机制拦截 glibc 的 POSIX 文件操作（`open/openat/read/pread/close/stat/fstat/opendir/readdir/lseek` 等），当文件路径匹配 CubeFS 挂载点前缀（如 `/mnt/cfs/`）时，将调用转发到 `libcfs.so`（CubeFS libsdk 的 C API），否则透传到原始 libc 函数
- **新增 CubeFS client 生命周期管理**：在 preload 库的 `__attribute__((constructor))` 中通过环境变量（`CFS_MASTER`、`CFS_VOL`、`CFS_AK`、`CFS_SK`、`CFS_BCACHE_DIRS` 等）初始化 `libcfs.so` 客户端，在 `__attribute__((destructor))` 中释放
- **新增 fd 空间映射**：为 CubeFS 文件分配高位 fd 号（偏移 100000），避免与系统 fd 冲突，维护 cfs_fd→offset 映射表支持 `lseek/read` 语义
- **新增 MPI 多进程部署脚本**：封装 `mpirun` 命令，自动注入 `LD_PRELOAD` 和 CubeFS 环境变量，在 3 台节点（204/205/206，各 8 个 MPI rank，每 rank 16 个 read_threads）上运行 DLIO benchmark
- **复用已有的 bcache IPC-bypass 和 extent cache pool 优化**：preload 模式下 gosdk 自动启用 IPC-bypass 本地缓存读取和元数据缓存池

## Capabilities

### New Capabilities

- `posix-preload`: LD_PRELOAD 拦截层，负责 POSIX API 拦截、路径匹配、fd 空间管理、libc 透传，以及通过环境变量驱动的 CubeFS client 初始化/销毁
- `dlio-integration`: DLIO benchmark 的 MPI 多节点部署集成，包括启动脚本、环境变量配置模板、多节点 bcache 配置

### Modified Capabilities

（无已有 spec 需要修改——libcfs.so 的 C API 和 gosdk 的功能已经完备，不需要改接口）

## Impact

- **新增代码**：`client/preload/cfs_preload.c`（~300-400 行 C）、`client/preload/Makefile`
- **编译依赖**：需要 `libcfs.so`（已有的 Go→C 共享库），preload 库通过 `dlopen` 动态加载
- **运行依赖**：3 台节点（7.32.209.204/205/206）均需部署 `libcfs_preload.so` + `libcfs.so`，每台节点需要 bcache SSD（已具备 `/bcache0`-`/bcache4`）
- **内存开销**：每个 MPI rank 进程加载 Go runtime 约增加 10-20 MB（24 个 rank × 20MB ≈ 480 MB 全局）
- **线程安全**：TensorFlow 每个 rank 使用 16 个 read_threads 并发调用拦截函数，需要 fd 映射表使用线程安全的数据结构（`pthread_mutex` 或 `pthread_rwlock`）
- **兼容性风险**：`LD_PRELOAD` 可能影响非 CubeFS 的文件操作（如 Python 模块加载、日志写入），需要严格的路径前缀匹配确保只拦截目标路径
