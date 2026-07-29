#!/bin/bash
# ============================================================
# 推送镜像到阿里云 ACR 并部署到云服务器
#
# 用法:
#   bash deploy/deploy-acr.sh push                          # 构建并推送镜像
#   bash deploy/deploy-acr.sh deploy <SSH_HOST>             # 推送 + 远程部署
# ============================================================
set -e

VERSION=$(git describe --tags --always 2>/dev/null || echo "1.0.0")
# 读取 .env 配置（如果存在）
if [ -f .env ]; then
    export $(grep -v '^#' .env | grep -v '^$' | xargs)
fi
REGISTRY="${REGISTRY:-registry.cn-hangzhou.aliyuncs.com/iot-platform}"
IMAGE="${REGISTRY}/dev-sys-cloud:${VERSION}"

push_image() {
    echo "=== 构建 Docker 镜像 ==="
    docker build -f deploy/Dockerfile -t "${IMAGE}" .
    echo "=== 推送到 ACR ==="
    docker push "${IMAGE}"
    echo "镜像: ${IMAGE}"
}

deploy_remote() {
    local SSH_HOST="${1:?Usage: bash deploy/deploy-acr.sh deploy root@39.106.70.145}"
    local DB_CONN="${DEV_SYS_DB:-postgresql://devsys:devsys@172.17.0.1:5432/devsys_cloud}"
    echo "=== 远程部署 ==="
    echo "目标: ${SSH_HOST}"
    echo "镜像: ${IMAGE}"
    echo "DB:   ${DB_CONN}"

    # 1. 确保远程服务器已登录 ACR（首次需手动: docker login）
    ssh "${SSH_HOST}" "docker pull ${IMAGE}" || {
        echo "ERROR: docker pull 失败。请先在远程服务器手动登录 ACR:"
        echo "  ssh ${SSH_HOST}"
        echo "  docker login --username=你的阿里云账号 \$(echo ${REGISTRY} | cut -d/ -f1)"
        exit 1
    }

    # 2. 停旧启新
    ssh "${SSH_HOST}" "
        docker stop dev-sys-cloud 2>/dev/null || true
        docker rm dev-sys-cloud 2>/dev/null || true
        docker run -d --name dev-sys-cloud \
            --restart=unless-stopped \
            -p 9080:9080 \
            -e DEV_SYS_DB='${DB_CONN}' \
            -v /data/dev-sys/firmware:/app/firmware_files \
            -v /data/dev-sys/logs:/app/logs \
            ${IMAGE}
    "

    # 3. 等待启动并验证
    echo "等待容器启动..."
    sleep 5
    echo ""
    ssh "${SSH_HOST}" "docker ps --filter name=dev-sys-cloud --format '{{.Status}}' && echo '---' && curl -s http://localhost:9080/api/v1/health || docker logs --tail 20 dev-sys-cloud"

    local IP=$(echo "${SSH_HOST}" | cut -d@ -f2)
    echo ""
    echo "部署完成! http://${IP}:9080/api/v1/health"
}

case "${1:-push}" in
    push)   push_image ;;
    deploy) deploy_remote "$2" ;;
    *)      echo "Usage: $0 push | deploy <SSH_HOST>" ;;
esac
