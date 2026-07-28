#!/bin/bash
# ============================================================
# dev-sys-cloud 一键安装脚本 — 阿里云 Ubuntu 22.04
#
# 用法:
#   sudo bash install.sh
# ============================================================
set -e

RED='\033[0;31m'; GREEN='\033[0;32m'; NC='\033[0m'
echo -e "${GREEN}=== dev-sys-cloud 云平台安装 ===${NC}"

# 1. 安装二进制
echo "[1/5] 安装二进制..."
cp dev-sys-cloud /usr/local/bin/
chmod +x /usr/local/bin/dev-sys-cloud

# 2. 创建目录
echo "[2/5] 创建数据目录..."
mkdir -p /etc/dev-sys-cloud/certs
mkdir -p /data/dev-sys/firmware
mkdir -p /var/log/dev-sys-cloud

# 3. 配置文件
echo "[3/5] 安装配置..."
if [ -f mqtt_config.json ]; then
    cp mqtt_config.json /app/config/ 2>/dev/null || mkdir -p /app/config && cp mqtt_config.json /app/config/
fi

# 4. 环境变量
echo "[4/5] 配置环境变量..."
if ! grep -q "DEV_SYS_DB" /etc/environment 2>/dev/null; then
    echo 'DEV_SYS_DB="postgresql://devsys:devsys@127.0.0.1:5432/devsys_cloud"' >> /etc/environment
fi

# 5. systemd 服务
echo "[5/5] 安装 systemd 服务..."
if [ -f dev-sys-cloud.service ]; then
    cp dev-sys-cloud.service /etc/systemd/system/
    systemctl daemon-reload
    systemctl enable dev-sys-cloud
    systemctl start dev-sys-cloud
    sleep 2
    systemctl status dev-sys-cloud --no-pager
fi

echo -e "${GREEN}=== 安装完成 ===${NC}"
echo "  API: http://$(hostname -I | awk '{print $1}'):9080/api/v1/health"
echo "  systemctl status dev-sys-cloud"
echo "  journalctl -u dev-sys-cloud -f"
