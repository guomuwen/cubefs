#!/bin/bash
# =============================================================================
# run_dlio_libsdk.sh — 使用 LD_PRELOAD + libcfs 运行 DLIO Benchmark
#
# 用法:
#   ./run_dlio_libsdk.sh                          # 使用默认 conf 配置
#   ./run_dlio_libsdk.sh --np 1                   # 单节点单进程测试
#   ./run_dlio_libsdk.sh --np 24 --hostfile hf    # 多节点运行
#   ./run_dlio_libsdk.sh --data-folder /mnt/cfs/x # 覆盖数据目录
#   ./run_dlio_libsdk.sh -- --extra-dlio-args      # -- 后面的参数透传给 DLIO
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONF="${SCRIPT_DIR}/dlio_libsdk.conf"

# 加载默认配置
if [ -f "$CONF" ]; then
    source "$CONF"
fi

# 命令行参数覆盖
EXTRA_DLIO_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --np)          MPI_NP="$2"; shift 2 ;;
        --npernode)    MPI_NPERNODE="$2"; shift 2 ;;
        --hostfile)    MPI_HOSTFILE="$2"; shift 2 ;;
        --data-folder) DLIO_DATA_FOLDER="$2"; shift 2 ;;
        --epochs)      DLIO_EPOCHS="$2"; shift 2 ;;
        --batch-size)  DLIO_BATCH_SIZE="$2"; shift 2 ;;
        --conf)        CONF="$2"; source "$CONF"; shift 2 ;;
        --no-preload)  PRELOAD_SO=""; shift ;;
        --log-level)   CFS_LOG_LEVEL="$2"; shift 2 ;;
        --)            shift; EXTRA_DLIO_ARGS=("$@"); break ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--np N] [--hostfile FILE] [--data-folder DIR] [--epochs N] [--no-preload] [-- DLIO_ARGS...]"
            exit 1
            ;;
    esac
done

# 验证关键文件
if [ -n "$PRELOAD_SO" ] && [ ! -f "$PRELOAD_SO" ]; then
    echo "ERROR: PRELOAD_SO not found: $PRELOAD_SO"
    echo "  Run deploy_preload.sh first, or set --no-preload to use FUSE mode"
    exit 1
fi

if [ -n "$PRELOAD_SO" ] && [ ! -f "$CFS_LIBCFS_SO" ]; then
    echo "ERROR: CFS_LIBCFS_SO not found: $CFS_LIBCFS_SO"
    exit 1
fi

# 构建 LD_PRELOAD 环境变量
if [ -n "$PRELOAD_SO" ]; then
    PRELOAD_ENV="LD_PRELOAD=$PRELOAD_SO"
    MODE="libsdk-preload"
else
    PRELOAD_ENV=""
    MODE="FUSE"
fi

echo "============================================"
echo " DLIO Benchmark with CubeFS ($MODE mode)"
echo "============================================"
echo " MPI:        np=$MPI_NP npernode=$MPI_NPERNODE"
echo " Hostfile:   $MPI_HOSTFILE"
echo " Data:       $DLIO_DATA_FOLDER"
echo " Files:      $DLIO_NUM_FILES_TRAIN"
echo " Batch:      $DLIO_BATCH_SIZE"
echo " Threads:    $DLIO_READ_THREADS"
echo " Epochs:     $DLIO_EPOCHS"
echo " Format:     $DLIO_FORMAT"
echo " Preload:    ${PRELOAD_SO:-disabled}"
echo " Log level:  $CFS_LOG_LEVEL"
echo "============================================"

# 构建 MPI 环境变量传递参数
MPI_ENV_ARGS=""
if [ -n "$PRELOAD_SO" ]; then
    MPI_ENV_ARGS="$MPI_ENV_ARGS -x LD_PRELOAD=$PRELOAD_SO"
    MPI_ENV_ARGS="$MPI_ENV_ARGS -x CFS_LIBCFS_SO=$CFS_LIBCFS_SO"
    MPI_ENV_ARGS="$MPI_ENV_ARGS -x CFS_MASTER=$CFS_MASTER"
    MPI_ENV_ARGS="$MPI_ENV_ARGS -x CFS_VOL=$CFS_VOL"
    MPI_ENV_ARGS="$MPI_ENV_ARGS -x CFS_AK=$CFS_AK"
    MPI_ENV_ARGS="$MPI_ENV_ARGS -x CFS_SK=$CFS_SK"
    MPI_ENV_ARGS="$MPI_ENV_ARGS -x CFS_MOUNT_POINT=$CFS_MOUNT_POINT"
    MPI_ENV_ARGS="$MPI_ENV_ARGS -x CFS_BCACHE_DIRS=$CFS_BCACHE_DIRS"
    MPI_ENV_ARGS="$MPI_ENV_ARGS -x CFS_LOG_DIR=$CFS_LOG_DIR"
    MPI_ENV_ARGS="$MPI_ENV_ARGS -x CFS_LOG_LEVEL=$CFS_LOG_LEVEL"
fi

# 构建 DLIO 命令
DLIO_CMD="dlio_benchmark"
DLIO_ARGS=""
DLIO_ARGS="$DLIO_ARGS workload=resnet50"
DLIO_ARGS="$DLIO_ARGS ++workload.dataset.data_folder=$DLIO_DATA_FOLDER"
DLIO_ARGS="$DLIO_ARGS ++workload.dataset.num_files_train=$DLIO_NUM_FILES_TRAIN"
DLIO_ARGS="$DLIO_ARGS ++workload.reader.batch_size=$DLIO_BATCH_SIZE"
DLIO_ARGS="$DLIO_ARGS ++workload.reader.read_threads=$DLIO_READ_THREADS"
DLIO_ARGS="$DLIO_ARGS ++workload.workflow.epochs=$DLIO_EPOCHS"
DLIO_ARGS="$DLIO_ARGS ++workload.dataset.format=$DLIO_FORMAT"
DLIO_ARGS="$DLIO_ARGS ++workload.workflow.computation_time=$DLIO_COMPUTATION_TIME"

# 追加额外 DLIO 参数
if [ ${#EXTRA_DLIO_ARGS[@]} -gt 0 ]; then
    DLIO_ARGS="$DLIO_ARGS ${EXTRA_DLIO_ARGS[*]}"
fi

# 构建完整 MPI 命令
MPI_CMD="mpirun"
MPI_CMD="$MPI_CMD --np $MPI_NP"
if [ -n "$MPI_HOSTFILE" ] && [ -f "$MPI_HOSTFILE" ]; then
    MPI_CMD="$MPI_CMD --hostfile $MPI_HOSTFILE"
fi
if [ "$MPI_NP" -gt 1 ] 2>/dev/null; then
    MPI_CMD="$MPI_CMD --npernode $MPI_NPERNODE"
fi
MPI_CMD="$MPI_CMD --allow-run-as-root"
MPI_CMD="$MPI_CMD --bind-to none"
MPI_CMD="$MPI_CMD $MPI_ENV_ARGS"
MPI_CMD="$MPI_CMD $DLIO_CMD $DLIO_ARGS"

echo ""
echo "Command:"
echo "  $MPI_CMD"
echo ""

# 创建日志目录
mkdir -p "$CFS_LOG_DIR" 2>/dev/null || true

# 记录开始时间
START_TIME=$(date +%s)
echo "Started at $(date)"

# 执行
eval $MPI_CMD
RC=$?

END_TIME=$(date +%s)
ELAPSED=$((END_TIME - START_TIME))
echo ""
echo "Finished at $(date) (elapsed: ${ELAPSED}s, exit: $RC)"
exit $RC
