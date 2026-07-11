#!/bin/bash
# PostgreSQL 外部访问配置脚本
# 运行: bash scripts/setup_postgresql.sh

set -e
PG_VER=14
PG_CONF=/etc/postgresql/${PG_VER}/main/postgresql.conf
PG_HBA=/etc/postgresql/${PG_VER}/main/pg_hba.conf

echo "=== 1. 修改监听地址为 * (所有网卡) ==="
sudo sed -i "s/#listen_addresses = 'localhost'/listen_addresses = '*'/" $PG_CONF
grep "^listen_addresses" $PG_CONF

echo ""
echo "=== 2. 允许外部 IP 通过密码连接 ==="
# 确保本地连接使用 md5
sudo sed -i 's|host    all             all             127.0.0.1/32            scram-sha-256|host    all             all             127.0.0.1/32            md5|' $PG_HBA 2>/dev/null || true
# 添加外部访问规则（仅当不存在时）
if ! grep -q "0.0.0.0/0" $PG_HBA; then
    echo "" | sudo tee -a $PG_HBA
    echo "# External access (password required)" | sudo tee -a $PG_HBA
    echo "host    all             all             0.0.0.0/0               md5" | sudo tee -a $PG_HBA
fi
echo "pg_hba.conf 最后5行:"
tail -5 $PG_HBA

echo ""
echo "=== 3. 重启 PostgreSQL ==="
sudo systemctl restart postgresql
sudo systemctl status postgresql --no-pager | head -5

echo ""
echo "=== 4. 创建数据库和用户 ==="
sudo -u postgres psql <<SQL
-- 如果用户不存在就创建
DO \$\$
BEGIN
  IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'devsys') THEN
    CREATE USER devsys WITH PASSWORD 'devsys';
  ELSE
    ALTER USER devsys WITH PASSWORD 'devsys';
  END IF;
END
\$\$;

-- 如果数据库不存在就创建
SELECT 'CREATE DATABASE devsys_cloud OWNER devsys'
WHERE NOT EXISTS (SELECT FROM pg_database WHERE datname = 'devsys_cloud')\gexec

GRANT ALL PRIVILEGES ON DATABASE devsys_cloud TO devsys;
SQL

echo ""
echo "=== 5. 验证连接 ==="
PGPASSWORD=devsys psql -h 127.0.0.1 -U devsys -d devsys_cloud -c "\conninfo"
PGPASSWORD=devsys psql -h 127.0.0.1 -U devsys -d devsys_cloud -c "SELECT 'PostgreSQL OK' as status, version()"

echo ""
echo "=== 6. 防火墙放行 (如有 ufw) ==="
sudo ufw allow 5432/tcp 2>/dev/null || echo "ufw 未启用或无权限"

echo ""
echo "══════════════════════════════════════════════════"
echo "  PostgreSQL 配置完成！"
echo ""
echo "  监听地址: 0.0.0.0:5432 (所有网卡)"
echo "  数据库:   devsys_cloud"
echo "  用户:     devsys / devsys"
echo ""
echo "  外部连接: psql -h <服务器IP> -U devsys -d devsys_cloud"
echo ""
echo "  设置环境变量启动 dev-sys-cloud:"
echo "  export DEV_SYS_DB='postgresql://devsys:devsys@127.0.0.1:5432/devsys_cloud'"
echo "  ./build/bin/dev-sys-cloud"
echo "══════════════════════════════════════════════════"
