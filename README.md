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
