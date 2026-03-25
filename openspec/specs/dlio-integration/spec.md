## Purpose

DLIO benchmark 的 MPI 多节点部署集成，包括启动脚本、环境变量配置模板、多节点 bcache 配置，实现 LD_PRELOAD 模式下的 DLIO 一键运行。

## Requirements

### Requirement: MPI 启动脚本
系统 MUST 提供 `run_dlio_libsdk.sh` 脚本，封装 `mpirun` 命令并自动注入 LD_PRELOAD 和 CubeFS 环境变量。脚本 MUST 支持通过命令行参数或配置文件指定 CubeFS 连接信息、节点列表、MPI 进程数等参数。

#### Scenario: 使用脚本启动 DLIO benchmark
- **WHEN** 执行 `./run_dlio_libsdk.sh --hostfile hfile64 --np 24 --data-folder /mnt/cfs/mlperf_data/resnet_copy_400g`
- **THEN** 脚本生成并执行包含 `-x LD_PRELOAD=libcfs_preload.so -x CFS_MASTER=... -x CFS_VOL=... -x CFS_AK=... -x CFS_SK=... -x CFS_MOUNT_POINT=/mnt/cfs/ -x CFS_BCACHE_DIRS=...` 的 mpirun 命令

#### Scenario: 默认参数启动
- **WHEN** 执行 `./run_dlio_libsdk.sh`（无额外参数）
- **THEN** 脚本从同目录的 `dlio_libsdk.conf` 配置文件读取默认值

### Requirement: 环境变量配置模板
系统 MUST 提供 `dlio_libsdk.conf` 配置模板，包含所有必要的 CubeFS 连接参数和 DLIO 运行参数。

#### Scenario: 配置模板完整性
- **WHEN** 用户查看 `dlio_libsdk.conf`
- **THEN** 文件包含以下配置项（均有注释说明）：CFS_MASTER、CFS_VOL、CFS_AK、CFS_SK、CFS_MOUNT_POINT、CFS_BCACHE_DIRS、CFS_LOG_DIR、CFS_LOG_LEVEL、LIBCFS_SO_PATH、LIBCFS_PRELOAD_SO_PATH、HOSTFILE、NP、MPI_PORT

### Requirement: 多节点部署
系统 MUST 支持将 `libcfs_preload.so` 和 `libcfs.so` 分发到所有 MPI 计算节点。SHOULD 提供一键部署脚本 `deploy_preload.sh`。

#### Scenario: 分发到 3 台节点
- **WHEN** 执行 `./deploy_preload.sh --hostfile hfile64`
- **THEN** 脚本通过 scp（端口 32200）将 libcfs_preload.so 和 libcfs.so 拷贝到每台节点的指定目录（如 `/usr/local/lib/cubefs/`），并验证文件是否正确部署

#### Scenario: 验证部署完整性
- **WHEN** 部署完成后
- **THEN** 脚本在每台节点上执行 `ldd libcfs_preload.so` 验证依赖是否满足

### Requirement: bcache 多节点配置
系统 MUST 支持每台节点独立配置 bcache SSD 路径。环境变量 `CFS_BCACHE_DIRS` MUST 在每台节点上指向该节点本地的 SSD 路径（`/bcache0;/bcache1;/bcache2;/bcache3;/bcache4`）。

#### Scenario: 每台节点使用本地 bcache
- **WHEN** MPI 在节点 204 上启动 rank 0-7，节点 205 上启动 rank 8-15
- **THEN** 每个 rank 的 gosdk 初始化时使用该节点本地的 bcache SSD 路径，不跨节点访问 bcache

### Requirement: DLIO 命令行兼容
启动脚本 MUST 支持透传所有 DLIO benchmark 的原生参数（workload、dataset、reader 等），仅注入 LD_PRELOAD 和 CubeFS 环境变量，不修改 DLIO 的任何行为参数。

#### Scenario: 与原始 DLIO 命令等价
- **WHEN** 使用 libsdk 模式运行 `workload=resnet50_v100 ++workload.dataset.num_files_train=3000000`
- **THEN** DLIO 的行为（数据加载、batch 组织、shuffle、epoch 管理）与 FUSE 模式完全一致，仅 I/O 路径不同

#### Scenario: data_folder 路径不变
- **WHEN** DLIO 配置 `data_folder=/mnt/cfs/mlperf_data/resnet_copy_400g`
- **THEN** 该路径保持不变，由 LD_PRELOAD 层透明转发到 libcfs.so，DLIO 无感知

### Requirement: 运行日志收集
系统 SHOULD 将每个 MPI rank 的 CubeFS 客户端日志输出到独立目录，便于问题排查。日志目录 SHOULD 通过 `CFS_LOG_DIR` 环境变量配置，默认为 `/tmp/cfs_preload_rank_<rank_id>/`。

#### Scenario: 多 rank 日志隔离
- **WHEN** 24 个 MPI rank 同时运行
- **THEN** 每个 rank 的 gosdk 日志写入 `/tmp/cfs_preload_rank_0/` ~ `/tmp/cfs_preload_rank_23/`，互不干扰
