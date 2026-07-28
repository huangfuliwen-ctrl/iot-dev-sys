#!/bin/bash
# ============================================================
# 一键部署 dev-sys-cloud 到阿里云 Ubuntu 22.04
#
# 用法:
#   bash deploy/deploy-aliyun.sh ubuntu@39.106.70.145
#
# 前置条件:
#   本机: 已安装 Docker
#   服务器: Ubuntu 22.04, 已安装 Docker + Docker Compose, PostgreSQL
# ============================================================
set -e

SSH_HOST="${1:?Usage: bash deploy/deploy-aliyun.sh ubuntu@39.106.70.145}"
VERSION=$(git describe --tags --always 2>/dev/null || echo "1.0.0")
PROJECT="dev-sys-cloud"

echo "=== 1/5: 编译 Release 二进制 ==="
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF -DUSE_SYSTEM_PAHO=OFF
cmake --build build -j$(nproc)
strip build/bin/dev-sys-cloud
echo "Binary: $(ls -lh build/bin/dev-sys-cloud | awk '{print $5}')"

echo "=== 2/5: 打包部署文件 ==="
DIST="deploy/dist/${VERSION}"
rm -rf "$DIST" && mkdir -p "$DIST"
cp build/bin/dev-sys-cloud "$DIST/"
cp config/mqtt_config.json "$DIST/"
cp deploy/production/dev-sys-cloud.service "$DIST/"
cp deploy/install.sh "$DIST/"
chmod +x "$DIST/install.sh"
cd "$DIST" && tar czf "../../${PROJECT}-${VERSION}.tar.gz" .
echo "Package: deploy/${PROJECT}-${VERSION}.tar.gz"

echo "=== 3/5: 上传到服务器 ==="
scp "deploy/${PROJECT}-${VERSION}.tar.gz" "${SSH_HOST}:/tmp/"

echo "=== 4/5: 远程安装 ==="
ssh "${SSH_HOST}" "cd /tmp && tar xzf ${PROJECT}-${VERSION}.tar.gz && sudo bash install.sh"

echo "=== 5/5: 验证 ==="
sleep 3
ssh "${SSH_HOST}" "systemctl status dev-sys-cloud --no-pager" || true
echo ""
echo "部署完成! http://${SSH_HOST#*@}:9080/api/v1/health"
