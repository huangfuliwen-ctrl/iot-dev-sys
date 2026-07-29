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
    echo "=== 远程部署 ==="
    ssh "${SSH_HOST}" "docker pull ${IMAGE} && \
        docker stop dev-sys-cloud 2>/dev/null || true && \
        docker rm dev-sys-cloud 2>/dev/null || true && \
        docker run -d --name dev-sys-cloud \
            --restart=always \
            -p 9080:9080 \
            -e DEV_SYS_DB='postgresql://devsys:devsys@host.docker.internal:5432/devsys_cloud' \
            -v /data/dev-sys/firmware:/app/firmware_files \
            -v /data/dev-sys/logs:/app/logs \
            ${IMAGE}"
    echo "部署完成! http://${SSH_HOST#*@}:9080/api/v1/health"
}

case "${1:-push}" in
    push)   push_image ;;
    deploy) deploy_remote "$2" ;;
    *)      echo "Usage: $0 push | deploy <SSH_HOST>" ;;
esac
