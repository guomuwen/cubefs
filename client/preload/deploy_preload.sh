#!/bin/bash
# =============================================================================
# deploy_preload.sh — 分发 libcfs_preload.so 和 libcfs.so 到多台节点
#
# 用法:
#   ./deploy_preload.sh                               # 使用默认 hostfile
#   ./deploy_preload.sh --hostfile /path/to/hfile64    # 指定 hostfile
#   ./deploy_preload.sh --nodes "1.2.3.4,5.6.7.8"     # 指定节点列表
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 默认配置
SSH_PORT=32200
DEPLOY_DIR="/usr/local/lib/cubefs"
HOSTFILE="/home/guomuwen/hfile64"
NODES=""

# 本地文件路径
LOCAL_PRELOAD_SO="${SCRIPT_DIR}/libcfs_preload.so"
LOCAL_LIBCFS_SO="/home/guomuwen/cubefs/build/bin/libcfs.so"

# 命令行参数
while [[ $# -gt 0 ]]; do
    case "$1" in
        --hostfile) HOSTFILE="$2"; shift 2 ;;
        --nodes)    NODES="$2"; shift 2 ;;
        --port)     SSH_PORT="$2"; shift 2 ;;
        --deploy-dir) DEPLOY_DIR="$2"; shift 2 ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--hostfile FILE] [--nodes 'ip1,ip2'] [--port PORT] [--deploy-dir DIR]"
            exit 1
            ;;
    esac
done

# 解析节点列表
HOSTS=()
if [ -n "$NODES" ]; then
    IFS=',' read -ra HOSTS <<< "$NODES"
elif [ -f "$HOSTFILE" ]; then
    while IFS= read -r line; do
        # 解析 hostfile 格式: hostname slots=N 或者纯 IP
        host=$(echo "$line" | awk '{print $1}' | sed 's/slots=.*//')
        [ -n "$host" ] && HOSTS+=("$host")
    done < "$HOSTFILE"
else
    echo "ERROR: No hostfile found at $HOSTFILE and --nodes not specified"
    exit 1
fi

# 去重
HOSTS=($(printf '%s\n' "${HOSTS[@]}" | sort -u))

echo "============================================"
echo " CubeFS Preload Deployment"
echo "============================================"
echo " Nodes:      ${HOSTS[*]}"
echo " SSH Port:   $SSH_PORT"
echo " Deploy Dir: $DEPLOY_DIR"
echo " Preload SO: $LOCAL_PRELOAD_SO"
echo " Libcfs SO:  $LOCAL_LIBCFS_SO"
echo "============================================"
echo ""

# 验证本地文件
if [ ! -f "$LOCAL_PRELOAD_SO" ]; then
    echo "ERROR: libcfs_preload.so not found at $LOCAL_PRELOAD_SO"
    echo "  Run 'cd client/preload && make' first"
    exit 1
fi

if [ ! -f "$LOCAL_LIBCFS_SO" ]; then
    echo "ERROR: libcfs.so not found at $LOCAL_LIBCFS_SO"
    echo "  Run 'make libsdk' first"
    exit 1
fi

echo "Local files:"
ls -lh "$LOCAL_PRELOAD_SO" "$LOCAL_LIBCFS_SO"
echo ""

# 分发到每台节点
FAILED=0
for host in "${HOSTS[@]}"; do
    echo "--- Deploying to $host ---"

    # 创建目标目录
    ssh -p "$SSH_PORT" -o ConnectTimeout=10 -o StrictHostKeyChecking=no \
        "$host" "mkdir -p $DEPLOY_DIR" 2>/dev/null
    if [ $? -ne 0 ]; then
        echo "  ERROR: Failed to connect to $host"
        FAILED=$((FAILED + 1))
        continue
    fi

    # 分发 libcfs_preload.so
    scp -P "$SSH_PORT" -o ConnectTimeout=10 -o StrictHostKeyChecking=no \
        "$LOCAL_PRELOAD_SO" "$host:$DEPLOY_DIR/" 2>/dev/null
    if [ $? -ne 0 ]; then
        echo "  ERROR: Failed to scp libcfs_preload.so to $host"
        FAILED=$((FAILED + 1))
        continue
    fi

    # 分发 libcfs.so
    scp -P "$SSH_PORT" -o ConnectTimeout=10 -o StrictHostKeyChecking=no \
        "$LOCAL_LIBCFS_SO" "$host:$DEPLOY_DIR/" 2>/dev/null
    if [ $? -ne 0 ]; then
        echo "  ERROR: Failed to scp libcfs.so to $host"
        FAILED=$((FAILED + 1))
        continue
    fi

    # 验证
    ssh -p "$SSH_PORT" -o ConnectTimeout=10 -o StrictHostKeyChecking=no \
        "$host" "ls -lh $DEPLOY_DIR/libcfs_preload.so $DEPLOY_DIR/libcfs.so && ldd $DEPLOY_DIR/libcfs_preload.so 2>/dev/null | head -5" 2>/dev/null
    echo "  OK: $host deployed successfully"
    echo ""
done

if [ $FAILED -gt 0 ]; then
    echo "WARNING: $FAILED node(s) failed deployment"
    exit 1
else
    echo "SUCCESS: All ${#HOSTS[@]} node(s) deployed successfully"
fi

# 也部署到本机
echo ""
echo "--- Deploying to localhost ---"
mkdir -p "$DEPLOY_DIR"
cp -f "$LOCAL_PRELOAD_SO" "$DEPLOY_DIR/"
cp -f "$LOCAL_LIBCFS_SO" "$DEPLOY_DIR/"
ls -lh "$DEPLOY_DIR/libcfs_preload.so" "$DEPLOY_DIR/libcfs.so"
echo "  OK: localhost deployed"
echo ""
echo "Done. Libraries at: $DEPLOY_DIR/"
