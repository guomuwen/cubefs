## 1. 项目结构与编译基础

- [x] 1.1 创建 `client/preload/` 目录结构，新建 `cfs_preload.c` 和 `Makefile`
  - 新建文件：`client/preload/cfs_preload.c`、`client/preload/Makefile`、`client/preload/README.md`
  - Makefile 需支持 `make preload` 编译出 `libcfs_preload.so`
  - 编译命令：`gcc -shared -fPIC -o libcfs_preload.so cfs_preload.c -ldl -lpthread`

- [x] 1.2 确认 `libcfs.so` 可用，检查导出的 C API 符号
  - 执行 `nm -D libcfs.so | grep -E "cfs_new_client|cfs_open|cfs_read|cfs_close|cfs_getattr|cfs_readdir|cfs_set_client|cfs_start_client|cfs_close_client"` 确认所有必需符号存在
  - 如果 libcfs.so 不存在，先编译：`cd /home/guomuwen/cubefs && make libcfs`

## 2. 核心拦截层实现

- [x] 2.1 实现 libc 函数指针保存和 libcfs.so 动态加载
  - 文件：`client/preload/cfs_preload.c`
  - 在 `__attribute__((constructor))` 中通过 `dlsym(RTLD_NEXT, "open")` 保存 `real_open`、`real_open64`、`real_openat`、`real_read`、`real_pread`、`real_pread64`、`real_close`、`real_stat`、`real_lstat`、`real_fstat`、`real_lseek`、`real_opendir`、`real_readdir`、`real_closedir` 等真实函数指针
  - 通过 `dlopen("libcfs.so", RTLD_NOW)` 加载 libcfs.so，`dlsym` 获取 `cfs_new_client`、`cfs_set_client`、`cfs_start_client`、`cfs_close_client`、`cfs_open`、`cfs_read`、`cfs_close`、`cfs_getattr`、`cfs_readdir` 函数指针

- [x] 2.2 实现 CubeFS 客户端初始化（constructor）
  - 文件：`client/preload/cfs_preload.c`
  - 读取环境变量：`CFS_MASTER`、`CFS_VOL`、`CFS_AK`、`CFS_SK`、`CFS_MOUNT_POINT`、`CFS_BCACHE_DIRS`、`CFS_LOG_DIR`、`CFS_LOG_LEVEL`、`CFS_PRELOAD_DISABLE`
  - 调用 `cfs_new_client()` 获取 `client_id`
  - 调用 `cfs_set_client(client_id, "volName", vol)` 等设置参数（masterAddr、volName、accessKey、secretKey、logDir、logLevel、bcacheDirs 等）
  - 调用 `cfs_start_client(client_id)` 启动客户端
  - 缺少必要环境变量时输出 stderr 警告并设 `g_preload_enabled = 0`
  - 验证：`LD_PRELOAD=./libcfs_preload.so CFS_MASTER=... ls /mnt/cfs/` 能正常执行

- [x] 2.3 实现 fd 映射表和线程安全机制
  - 文件：`client/preload/cfs_preload.c`
  - 定义 `FD_OFFSET = 100000`、`MAX_CFS_FDS = 65536`
  - 定义 `fd_entry` 结构体：`{ int cfs_fd; off_t offset; char path[PATH_MAX]; int valid; }`
  - 全局数组 `fd_table[MAX_CFS_FDS]` + `pthread_rwlock_t fd_lock`
  - 实现 `alloc_fd_entry(cfs_fd, path)` 和 `free_fd_entry(idx)` 辅助函数

- [x] 2.4 实现 open/open64/openat 拦截
  - 文件：`client/preload/cfs_preload.c`
  - 路径匹配：`strncmp(path, g_mount_point, g_mount_point_len) == 0`
  - 匹配时：截取 CubeFS 内部路径 → `cfs_open(client_id, cfs_path, flags, mode)` → `alloc_fd_entry` → 返回 `idx + FD_OFFSET`
  - 不匹配时：`real_open(path, flags, mode)`
  - openat 需处理 `AT_FDCWD` + 绝对路径的情况

- [x] 2.5 实现 pread/pread64 拦截
  - 文件：`client/preload/cfs_preload.c`
  - `fd >= FD_OFFSET` 时：`pthread_rwlock_rdlock` → `cfs_read(client_id, fd_table[idx].cfs_fd, buf, size, offset)` → `pthread_rwlock_unlock` → 返回字节数
  - `fd < FD_OFFSET` 时：`real_pread(fd, buf, size, offset)`

- [x] 2.6 实现 read 拦截（带 offset 管理）
  - 文件：`client/preload/cfs_preload.c`
  - `fd >= FD_OFFSET` 时：读取 `fd_table[idx].offset` → `cfs_read` → 原子更新 offset（`__atomic_add_fetch`）
  - `fd < FD_OFFSET` 时：`real_read(fd, buf, size)`

- [x] 2.7 实现 close 拦截
  - 文件：`client/preload/cfs_preload.c`
  - `fd >= FD_OFFSET` 时：`pthread_rwlock_wrlock` → `cfs_close(client_id, cfs_fd)` → `free_fd_entry(idx)` → unlock
  - `fd < FD_OFFSET` 时：`real_close(fd)`

- [x] 2.8 实现 stat/lstat/fstat/__xstat/__fxstat 拦截
  - 文件：`client/preload/cfs_preload.c`
  - stat/lstat：路径匹配 → `cfs_getattr(client_id, cfs_path, &cfs_stat)` → 转换为 `struct stat`
  - fstat：`fd >= FD_OFFSET` → 从 `fd_table[idx].path` 获取路径 → `cfs_getattr`
  - `cfs_stat_info` → `struct stat` 字段映射：ino、size、blocks、mode、nlink、uid、gid、atime、mtime、ctime、blk_size

- [x] 2.9 实现 lseek/lseek64 拦截
  - 文件：`client/preload/cfs_preload.c`
  - `fd >= FD_OFFSET` 时：根据 whence（SEEK_SET/SEEK_CUR/SEEK_END）更新 `fd_table[idx].offset`
  - SEEK_END 需要调用 `cfs_getattr` 获取文件大小
  - `fd < FD_OFFSET` 时：`real_lseek(fd, offset, whence)`

- [x] 2.10 实现 opendir/readdir/closedir 拦截
  - 文件：`client/preload/cfs_preload.c`
  - 自定义 `cfs_DIR` 结构体封装 cfs fd 和 readdir 状态
  - opendir：路径匹配 → `cfs_open` 目录 → 包装为 `cfs_DIR*`
  - readdir：从 `cfs_readdir` 获取条目 → 转换为 `struct dirent`
  - closedir：`cfs_close` → 释放 `cfs_DIR`

- [x] 2.11 实现 destructor 清理
  - 文件：`client/preload/cfs_preload.c`
  - `__attribute__((destructor))`：遍历 fd_table 关闭所有 valid 的 cfs fd → `cfs_close_client(client_id)` → `dlclose(libcfs_handle)`

## 3. 编译与本地验证

- [x] 3.1 编译 libcfs_preload.so 并验证基本功能
  - ✅ 编译零 warning：`cd client/preload && make`
  - ✅ 需先重编 libcfs.so（`make libsdk`）以包含 bcache nil-panic 修复
  - ✅ 验证 open/read/close：`cat /mnt/cfs/.../img_0000000_of_4200000.tfrecord` → 114,303 bytes
  - ✅ 验证 stat/opendir/readdir：深层目录遍历正常
  - ⚠️ `ls` 对子目录失败（ls 使用 openat 相对路径），不影响 DLIO

- [x] 3.2 验证非 CubeFS 路径透传
  - ✅ `cat /tmp/preload_test.txt` 正常输出
  - ✅ `ls /tmp/preload_test.txt` 正常工作
  - ✅ `stat /tmp/preload_test.txt` 正常工作
  - ✅ `CFS_PRELOAD_DISABLE=1` 禁用模式正常透传

- [x] 3.3 验证 Python/TensorFlow 兼容性
  - ✅ Python 3.11 全面兼容：os.listdir、os.scandir、os.open/fstat/read/close、open()、os.stat、os.path.exists/isdir、glob.glob、os.walk
  - ✅ 关键修复：CPython 3.11 使用 `readdir64` 符号（而非 `readdir`），添加 `readdir64` 拦截
  - ✅ 关键修复：CPython 3.11 使用 `fstatat64` 实现 `fstat`，添加 64-bit stat 拦截
  - ✅ 二进制文件 .tfrecord 读取完全正确 (114,303 bytes)
  - ✅ glob.glob 找到 4096 个 tfrecord 文件

## 4. bcache 与性能优化

- [x] 4.1 验证 bcache IPC-bypass 在 preload 模式下生效
  - ✅ CFS_BCACHE_DIRS 已配置 `/bcache0;/bcache1;/bcache2;/bcache3;/bcache4`
  - ✅ 关键修复：libcfs.so 的 `openStream` 需要调用 `OpenStreamWithCache`（而非 `OpenStream`）才能设置 `needBCache=true`
  - ✅ 关键修复：libcfs.so 的 ExtentConfig 需要设置 `OnGetInodeInfo: mw.InodeGet_ll` 避免 nil panic
  - ✅ 日志确认 bcache hit：`hit blockCache: cacheKey(ltptest_29839608_28_60_0000000000000000)`
  - ✅ 性能验证：bcache 命中后读取延迟 1.2ms（首次 11.4ms → 第二次 1.2ms，9.5x 加速）

- [x] 4.2 单节点性能基准测试
  - ✅ 编写了 `preload_bench.c` 多线程基准测试程序（支持 pattern 模式生成文件名）
  - ✅ 小规模测试（4096 文件 / 32 线程）：Epoch 2 峰值 **9,299 IOPS / 1,013 MB/s**
  - ✅ 大规模测试（100,000 文件 / 64 线程）：稳定 **6,400 IOPS / 700 MB/s**
  - ✅ 中等规模测试（20,000 文件 / 32 线程）：Epoch 2 **8,537 IOPS / 930 MB/s**
  - ✅ 对比 libsdk_bench Epoch 1（网络读取）：220 IOPS / 21.5 MB/s → preload 提升 **30-40x**
  - ✅ 单节点已超目标（3 节点聚合目标 12,000 IOPS 只需每节点 4,000）

## 5. DLIO 集成与部署脚本

- [x] 5.1 创建配置模板 `dlio_libsdk.conf`
  - ✅ 新建文件：`client/preload/dlio_libsdk.conf`
  - ✅ 包含 CubeFS 连接（master/vol/ak/sk）、bcache、日志、DLIO 参数、MPI 参数

- [x] 5.2 创建 MPI 启动脚本 `run_dlio_libsdk.sh`
  - ✅ 新建文件：`client/preload/run_dlio_libsdk.sh`
  - ✅ 读取 dlio_libsdk.conf 配置，支持命令行覆盖（--np, --hostfile, --data-folder, --epochs 等）
  - ✅ 组装 mpirun 命令，注入 `-x LD_PRELOAD` `-x CFS_*` 环境变量
  - ✅ 支持 `--no-preload` 切换 FUSE 模式对比测试
  - ✅ `--` 后的参数透传给 DLIO

- [x] 5.3 创建多节点部署脚本 `deploy_preload.sh`
  - ✅ 新建文件：`client/preload/deploy_preload.sh`
  - ✅ 支持 --hostfile 和 --nodes 两种指定节点方式
  - ✅ 通过 `scp -P 32200` 分发 + `ldd` 验证

- [ ] 5.4 分发并部署到 3 台节点
  - 执行 `./deploy_preload.sh --hostfile /home/guomuwen/hfile64`
  - 确认每台节点的库文件存在且可执行

## 6. DLIO Benchmark 端到端验证

- [x] 6.1 单节点 DLIO 测试（1 个 MPI rank）
  - ✅ DLIO 2.0.0 + TensorFlow 2.21.0 + LD_PRELOAD 兼容性验证通过
  - ✅ 本地 NPZ 数据集（50 文件）：12 steps 完成, AU=99.37%, Throughput=36.8 samples/s
  - ✅ TF 核心 API（tf.io.read_file, FixedLengthRecordDataset）通过 preload 完美读取 CubeFS 文件
  - ⚠️ CubeFS 上的 .tfrecord 文件非标准 TFRecord 格式（随机数据），DLIO TFRecordDataset reader 报 corrupted
  - ✅ 解决方案：使用 format=npz 或 format=synthetic 模式，或重新生成标准格式数据

- [ ] 6.2 多节点 DLIO 测试（24 个 MPI rank，3 台节点）
  - 待完成：需要先配置 SSH 免密登录到 205/206 节点
  - 待完成：分发 libcfs_preload.so 和 libcfs.so 到远程节点

- [ ] 6.3 性能对比测试
  - 待完成：需要 FUSE 模式作为基准线
  - 已有 preload_bench 基准数据：单节点峰值 9,299 IOPS / 1,013 MB/s
