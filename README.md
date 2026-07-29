# dev-sys-cloud — IoT云平台服务程序

多租户设备管理、消息路由、订单处理、OTA编排的MQTT云服务。

## 架构

```
                     MQTT Broker
                 (多租户Topic隔离)
                         │
          ┌──────────────┼──────────────┐
          ▼              ▼              ▼
      设备A(咖啡机)   设备B(饮水机)   设备C(...)
    tenant_1/iot/   tenant_1/iot/   tenant_2/iot/
    coffee/deviceA  water/deviceB   coffee/deviceC
          │              │              │
          └──────────────┼──────────────┘
                         │
          通配符订阅: +/iot/+/+/...
                         │
                         ▼
          ┌──────────────────────────────┐
          │    云平台服务 (本系统)         │
          │                              │
          │  MessageRouter (核心)         │
          │  解析topic → 路由到业务模块    │
          │                              │
          │  ┌─ DeviceManager 多租户设备  │
          │  ├─ OrderManager  订单追踪    │
          │  ├─ OtaManager    OTA编排     │
          │  └─ FaultManager  故障监控    │
          └──────────────┬───────────────┘
                         │
                         ▼
                    PostgreSQL
```

## 核心设计：多租户消息路由

```
收消息（通配符订阅，收到所有设备消息）:
  MQTT订阅: +/iot/+/+/heartbeat
  收到topic: tenant_1/iot/coffee_v1/device_001/heartbeat
              ↓
  ParsedTopic::parse() → tenant_id="tenant_1", device_id="device_001"
              ↓
  MessageRouter::on_message() → DeviceManager::process_heartbeat()

发消息（精确topic，定向到具体设备）:
  MessageRouter::send_command("tenant_1", "coffee_v1", "device_001", "reboot", "{}")
              ↓
  构造topic: tenant_1/iot/coffee_v1/device_001/command/reboot
              ↓
  MQTT publish → 只有 device_001 收到
```

## 目录结构

```
dev-sys/
├── include/dev_sys/common/       # 数据结构 (types.h, status_codes.h, constants.h)
├── src/
│   ├── main.cpp                  # 云服务入口 (消息驱动主循环)
│   ├── app/
│   │   ├── message_router.h/cpp  # 【核心】通配符订阅+Topic解析+分发
│   │   ├── device/               # 多租户设备管理
│   │   ├── order/                # 订单事件处理
│   │   ├── ota/                  # OTA编排 (版本注册+灰度推送+进度追踪)
│   │   ├── fault/                # 故障告警接收
│   │   ├── recipe/               # 配方管理
│   │   └── config/               # 服务配置
│   └── middleware/
│       ├── communication/        # MQTT客户端 (特权凭证+通配符订阅)
│       ├── security/             # TLS/mTLS
│       └── storage/              # DB + 日志
├── config/                       # 配置文件
└── test/                         # 单元测试
```

## 构建

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

## MQTT消息流

| 方向 | Topic模式 | 说明 |
|------|----------|------|
| 上行 (收) | `+/iot/+/+/heartbeat` | 全设备心跳 |
| 上行 (收) | `+/iot/+/+/event/post` | 全设备事件(订单/故障) |
| 上行 (收) | `+/iot/+/+/property/post` | 全设备属性上报 |
| 上行 (收) | `+/iot/+/+/ota/progress` | 全设备OTA进度 |
| 下行 (发) | `{tenant}/iot/{product}/{device}/ota/notify` | OTA推送通知 |
| 下行 (发) | `{tenant}/iot/{product}/{device}/command/{cmd}` | 设备指令 |
| 下行 (发) | `{tenant}/iot/{product}/{device}/property/set` | 属性下发 |

## 与设备端的关系

```
本服务 (云平台)                 设备端程序
─────────────                  ──────────
订阅: +/iot/+/+/heartbeat  ←── publish: {tenant}/iot/{product}/{device}/heartbeat
订阅: +/iot/+/+/event/post ←── publish: {tenant}/iot/{product}/{device}/event/post
订阅: +/iot/+/+/ota/progress ←─ publish: {tenant}/iot/{product}/{device}/ota/progress

publish: {tenant}/iot/{product}/{device}/ota/notify ──→ 设备订阅: .../ota/notify
publish: {tenant}/iot/{product}/{device}/command/reboot ──→ 设备订阅: .../command/+
```

Broker ACL确保：设备只能访问自己租户的topic，云服务拥有全局读取权限。

## 环境变量

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `DEV_SYS_DB` | `postgresql://devsys:devsys@127.0.0.1:5432/devsys_cloud` | PostgreSQL连接串 |

## 本地运行

```bash
# 1. 确保 PostgreSQL 已运行，数据库和用户已创建
PGPASSWORD=devsys psql -h 127.0.0.1 -U devsys -d devsys_cloud -c "SELECT 1"

# 2. 编译
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=OFF
make -j$(nproc)

# 3. 启动
cd ..
export DEV_SYS_DB="postgresql://devsys:devsys@127.0.0.1:5432/devsys_cloud"
./build/bin/dev-sys-cloud

# 4. 验证
curl http://127.0.0.1:9080/api/v1/health
```

## 部署到阿里云

### 方式一：一键部署脚本

```bash
# 直接部署到阿里云 ECS (需要 SSH 免密)
bash deploy/deploy-aliyun.sh ubuntu@39.106.70.145
```

脚本会自动完成：Release编译 → 打包 → scp上传 → 远程安装 → systemd启动。

### 方式二：打包部署

```bash
# 1. 本机编译打包
make -f deploy/Makefile package
# 生成: deploy/dev-sys-cloud-1.0.0-x86_64.tar.gz

# 2. 上传到服务器
scp deploy/dev-sys-cloud-*.tar.gz root@39.106.70.145:/tmp/

# 3. 服务器上安装
ssh root@39.106.70.145
cd /tmp && tar xzf dev-sys-cloud-*.tar.gz
sudo bash install.sh
```

### 方式三：Docker 部署

```bash
# 打包 Docker 部署文件（含预编译二进制）
make -f deploy/Makefile docker-package

# 上传到服务器
scp dev-sys-cloud-docker-*.tar.gz root@39.106.70.145:/tmp/

# 服务器上构建并运行
ssh root@39.106.70.145
cd /tmp && tar xzf dev-sys-cloud-docker-*.tar.gz
docker build -t dev-sys-cloud .
docker run -d --name dev-sys-cloud --restart=always \
    -p 9080:9080 \
    -e DEV_SYS_DB='postgresql://devsys:devsys@host.docker.internal:5432/devsys_cloud' \
    -v /data/dev-sys/firmware:/app/firmware_files \
    dev-sys-cloud
```

### 方式四：阿里云 ACR (容器镜像服务)

```bash
# 1. 登录 ACR
docker login --username=your_aliyun_account registry.cn-hangzhou.aliyuncs.com

# 2. 构建并推送
export REGISTRY="registry.cn-hangzhou.aliyuncs.com/iot-platform"
bash deploy/deploy-acr.sh push

# 3. 远程部署
bash deploy/deploy-acr.sh deploy root@39.106.70.145
```

### 服务器环境要求

- Ubuntu 22.04+
- PostgreSQL 14+ (监听 5432，允许密码连接)
- 环境变量 `DEV_SYS_DB` 指向 PostgreSQL

```bash
# 快速安装 PostgreSQL
sudo apt-get install -y postgresql
sudo -u postgres psql -c "CREATE USER devsys WITH PASSWORD 'devsys';"
sudo -u postgres psql -c "CREATE DATABASE devsys_cloud OWNER devsys;"
```

### 部署目录结构

```
deploy/
├── Makefile                 # 统一部署命令 (package/deploy/docker/docker-package)
├── deploy-aliyun.sh         # 一键阿里云部署
├── deploy-acr.sh            # ACR镜像推送+部署
├── install.sh               # 服务器安装脚本
├── Dockerfile               # 多阶段构建 (编译+运行)
├── Dockerfile.runtime       # 运行时镜像 (预编译二进制)
├── start.sh / stop.sh / status.sh   # 本地进程管理
├── config/                  # 配置文件
├── production/              # 生产环境配置 (systemd/Docker)
├── log/                     # 运行日志
└── run/                     # PID文件
```
