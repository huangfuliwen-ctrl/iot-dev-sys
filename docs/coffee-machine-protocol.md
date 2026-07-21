# 咖啡机设备通信协议文档

> **版本**: V1.0  
> **日期**: 2026-07-21  
> **用途**: 咖啡机设备与云平台之间的通信报文格式定义。设备端和后端均以此文档为消息格式依据。

---

## 1. 概述

### 1.1 通信架构

```
咖啡机设备 ──MQTT──→ MQTT Broker ──MQTT──→ dev-sys-cloud (后端)
    │                                            │
    └──────── HTTP REST API ──────────────────────┘
```

| 通道 | 用途 | 方向 |
|------|------|------|
| **MQTT** | 实时双向通信（心跳、事件、属性、指令） | 双向 |
| **HTTP** | 设备激活、大文件下载、离线补传 | 设备→云 |

### 1.2 Topic 格式

设备端使用的用户 Topic 格式（Broker 内部自动加上 `$T/{tenant}/` 前缀）：

```
{device_id}/v1/{message_type}[/{sub_type}]
```

**上行 (设备→云)**:

| Topic | QoS | 说明 |
|-------|-----|------|
| `{device_id}/v1/heartbeat` | 1 | 心跳保活 |
| `{device_id}/v1/event/post` | 1 | 事件上报（订单/故障/警告） |
| `{device_id}/v1/property/post` | 1 | 属性上报（传感器/状态） |
| `{device_id}/v1/ota/progress` | 1 | OTA升级进度 |

**下行 (云→设备)**:

| Topic | QoS | 说明 |
|-------|-----|------|
| `{device_id}/v1/property/set` | 1 | 配置下发 |
| `{device_id}/v1/ota/notify` | 1 | OTA升级通知 |
| `{device_id}/v1/command/{cmd}` | 1 | 远程指令 |

### 1.3 通用约定

- 所有消息体为 **JSON**（UTF-8 编码）
- 单条消息 ≤ 256KB
- MQTT keep-alive: 60秒
- HTTP API 基础路径: `/api/v1`
- HTTP Content-Type: `application/json`

---

## 2. 设备激活 (HTTP)

设备首次上电，向云平台注册获取 `device_id`。

**请求** `POST /api/v1/device/activate`

```json
{
    "uid": "SIM-a1b2c3d4e5f6a7b8",
    "model_key": "01KXW0CFN798QFGS86XBDG574A"
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| uid | string | 是 | 硬件唯一标识（芯片序列号） |
| model_key | string | 是 | 设备型号Key（26字符ULID，出厂烧录） |

> **安全约束**: 设备端不可指定 `tenant_id`，租户由云平台根据 `model_key` 自动分配。

**响应** `201 Created`

```json
{
    "success": true,
    "device_id": "dev_a1b2c3d4e5f6a7b8",
    "tenant_id": "coffeeusr",
    "product_id": "coffee_v1",
    "model_code": "CM-2000",
    "device_type": "coffee_machine",
    "firmware_version": "v2.0.5",
    "activation_token": "1ec5f204b3ab73e2a61054888196ad0c...",
    "mqtt_broker_uri": "tcp://127.0.0.1:1883",
    "ttl_seconds": 31536000
}
```

| 字段 | 说明 |
|------|------|
| device_id | 设备唯一ID（后续所有通信的标识） |
| tenant_id | 设备所属租户 |
| model_code | 型号编码 |
| device_type | 设备类型 |
| firmware_version | 基础固件版本 |
| activation_token | 激活凭证（用于后续认证） |
| mqtt_broker_uri | MQTT Broker 连接地址 |
| ttl_seconds | Token 有效期（1年） |

**失败响应**:

```json
{
    "success": false,
    "error_code": -404,
    "error_message": "model_key not found: xxx"
}
```

---

## 3. MQTT 消息格式

### 3.1 心跳保活 `{device_id}/v1/heartbeat`

设备定期上报，保持在线状态。

**上报频率**: 默认30秒（云端可配置 10~600秒）

**消息体**:

```json
{
    "device_id": "dev_a1b2c3d4e5f6a7b8",
    "timestamp": "2026-07-20T12:00:00Z",
    "network_status": 0,
    "work_status": 0,
    "firmware_version": "v2.0.5",
    "signal_strength": 95,
    "alarm_count": 0
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| device_id | string | 是 | 设备ID |
| timestamp | string | 是 | ISO 8601 UTC 时间 |
| network_status | int | 是 | 网络状态: 0=在线, 1=离线 |
| work_status | int | 是 | 工作状态（见下表） |
| firmware_version | string | 是 | 当前固件版本 |
| signal_strength | int | 否 | 网络信号强度 0-100 |
| alarm_count | int | 否 | 当前活跃告警数 |

**work_status 工作状态码**:

| 值 | 名称 | 说明 |
|----|------|------|
| 0 | IDLE | 空闲待机 |
| 1 | BREWING | 制作中 |
| 2 | FAULT | 故障（需人工介入） |
| 3 | UPGRADING | 固件升级中 |
| 4 | MAINTENANCE | 维护中 |

---

### 3.2 事件上报 `{device_id}/v1/event/post`

设备在发生关键业务事件时上报。消息体包含 `event_type` 标识事件类型。

#### 3.2.1 订单上报

咖啡机不涉及支付流程。制作完成后直接上报一条订单记录即可。

```json
{
    "event_type": "order_completed",
    "order_id": "ORD-a1b2c3",
    "recipe_id": "REC-AMERICANO-001",
    "recipe_name": "经典美式",
    "cup_size": "中",
    "timestamp": "2026-07-20T12:06:00Z"
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| event_type | string | 是 | 固定值 `"order_completed"` |
| order_id | string | 是 | 订单唯一编号（设备端生成） |
| recipe_id | string | 是 | 配方ID，如 `REC-AMERICANO-001` |
| recipe_name | string | 否 | 饮品名称，如 `经典美式` |
| cup_size | string | 否 | 杯型：小/中/大 |
| timestamp | string | 是 | 制作完成时间（ISO 8601 UTC） |

> 订单仅做记录用途，云端收到后入库即可，无需返回确认。

---

#### 3.2.2 故障告警

故障告警分为**警告 (warning)** 和**错误 (error)** 两大类。

**错误 (error)** — 影响正常运行，设备停止制作，需人工处理：

```json
{
    "event_type": "fault_alert",
    "fault_code": 5,
    "level": "error",
    "description": "机箱底部漏水传感器触发",
    "timestamp": "2026-07-20T12:10:00Z",
    "sensor_snapshot": {
        "leak_sensor": true,
        "zone": "bottom_tray"
    }
}
```

**警告 (warning)** — 不中断运行，仅提醒运维关注：

```json
{
    "event_type": "fault_alert",
    "fault_code": 8,
    "level": "warning",
    "description": "咖啡豆余量不足：当前120g，阈值150g",
    "timestamp": "2026-07-20T12:10:00Z",
    "sensor_snapshot": {
        "bean_remaining_g": 120,
        "threshold_g": 150
    }
}
```

**故障码一览**:

| 故障码 | 名称 | 级别 | 说明 |
|--------|------|------|------|
| E001 | HEATER_OVERTEMP | error | 加热器超温（立即断电） |
| E002 | HEATER_NOT_HEATING | error | 加热器不加热 |
| E003 | PUMP_FAILURE | error | 水泵流量/压力异常 |
| E004 | MOTOR_STALL | error | 电机堵转 |
| E005 | WATER_LEAK | error | 漏水检测触发 |
| W001 | SENSOR_OFFLINE | warning | 传感器通信异常 |
| W002 | COMM_FAIL | warning | 通信模块异常 |
| W003 | MATERIAL_LOW | warning | 原料不足（豆/水/粉） |

**级别说明**:

| 级别 | 设备行为 | 上报频率 |
|------|---------|---------|
| **error** | 停止制作，等待人工介入；L4危险项立即断电 | 实时上报 |
| **warning** | 不中断运行，屏幕提示 + 告警记录 | 5分钟合并上报 |

---

#### 3.2.3 故障恢复

```json
{
    "event_type": "fault_resolved",
    "fault_code": 3,
    "level": "error",
    "timestamp": "2026-07-20T12:15:00Z"
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| event_type | string | 是 | 固定值 `"fault_resolved"` |
| fault_code | int | 是 | 恢复的故障码（E001-E005 / W001-W003） |
| level | string | 是 | 故障级别：`"warning"` / `"error"` |
| timestamp | string | 是 | 恢复时间（ISO 8601 UTC） |

> 后端根据 `fault_code` 精确定位并清除对应告警，`level` 辅助前端展示。

#### 3.2.4 维护事件

**进入维护模式**:

```json
{
    "event_type": "maintenance_start",
    "reason": "定期维护",
    "timestamp": "2026-07-20T12:20:00Z"
}
```

**退出维护模式**:

```json
{
    "event_type": "maintenance_end",
    "timestamp": "2026-07-20T12:30:00Z"
}
```

---

### 3.3 属性上报 `{device_id}/v1/property/post`

定期上报设备运行参数，云端可用于监控和数据分析。

**上报频率**: 建议 30~120秒，可按需调整

```json
{
    "device_id": "dev_a1b2c3d4e5f6a7b8",
    "properties": {
        "cpu_temp_c": 55,
        "water_temp_c": 92,
        "boiler_temp_c": 94,
        "bean_remaining_g": 350,
        "milk_remaining_ml": 800,
        "waste_bin_pct": 25,
        "water_level_pct": 60,
        "total_brews": 1234,
        "uptime_s": 86400
    }
}
```

| 属性 | 类型 | 单位 | 说明 |
|------|------|------|------|
| cpu_temp_c | int | °C | CPU 温度 |
| water_temp_c | int | °C | 出水温度 |
| boiler_temp_c | int | °C | 锅炉温度 |
| bean_remaining_g | int | g | 咖啡豆余量 |
| milk_remaining_ml | int | ml | 牛奶余量 |
| waste_bin_pct | int | % | 废料桶填充率 |
| water_level_pct | int | % | 水位百分比 |
| total_brews | int | 次 | 累计制作次数 |
| uptime_s | int | 秒 | 设备运行时长 |

---

### 3.4 状态上报 `{device_id}/v1/status`

设备上下线和 Will Message 用。

**上线**:

```json
{
    "network_status": 0,
    "work_status": 0
}
```

**下线 (Will Message — 由 Broker 自动发布)**:

```json
{
    "network_status": 1,
    "work_status": 0
}
```

---

### 3.5 OTA 升级进度 `{device_id}/v1/ota/progress`

```json
{
    "device_id": "dev_a1b2c3d4e5f6a7b8",
    "version": "v2.1.0",
    "progress": 50,
    "stage": "downloading"
}
```

| stage | 说明 |
|-------|------|
| downloading | 下载中 |
| installing | 安装中 |
| done | 完成 |
| failed | 失败 |
| rolled_back | 已回滚 |

---

## 4. HTTP 通信

### 4.1 设备激活

见 [第2章](#2-设备激活-http)。

### 4.2 配置拉取 `GET /api/v1/device/config`

设备启动时拉取云端配置。

**响应**:

```json
{
    "code": 0,
    "data": {
        "heartbeat_interval": 30,
        "auto_clean_interval": 20,
        "idle_clean_delay": 30,
        "screen_brightness": 80,
        "volume_level": 70,
        "energy_save_mode": false,
        "order_expire_minutes": 15,
        "max_queue_depth": 10
    }
}
```

### 4.3 配方同步 `GET /api/v1/recipes`

设备拉取云端配方列表。支持增量同步。

### 4.4 OTA固件下载 `GET /api/v1/ota/firmwares/{version}/download`

设备下载固件升级包（base64编码在JSON中）。

### 4.5 批量事件补传 `POST /api/v1/device/events`

MQTT 不可用时的降级通道，批量上传事件。

```json
{
    "device_id": "dev_a1b2c3d4e5f6a7b8",
    "events": [
        {
            "event_type": "order_status",
            "order_id": "ORD-001",
            "status": 3
        },
        {
            "event_type": "fault_alert",
            "fault_code": 1,
            "fault_level": 4,
            "description": "加热器超温"
        }
    ]
}
```

---

## 5. 下行指令 (MQTT)

云端通过 MQTT 向设备下发指令。

### 5.1 配置下发 `{device_id}/v1/property/set`

```json
{
    "heartbeat_interval": 60,
    "screen_brightness": 90,
    "energy_save_mode": true
}
```

设备收到后应用配置，并通过 `{device_id}/v1/property/post` 上报实际值。

### 5.2 OTA 通知 `{device_id}/v1/ota/notify`

```json
{
    "version": "v2.1.0",
    "download_url": "https://cdn.example.com/firmware/v2.1.0.bin",
    "checksum_sha256": "a1b2c3d4...",
    "file_size": 4194304,
    "force_upgrade": false,
    "changelog": "修复加热器PID参数异常；优化待机功耗"
}
```

### 5.3 远程指令 `{device_id}/v1/command/{cmd}`

| cmd | 说明 | 参数 |
|-----|------|------|
| restart | 重启设备 | 无 |
| emergency_stop | 紧急停止 | 无 |
| maintenance | 进入/退出维护 | `{"action":"start"/"stop"}` |
| lock | 锁定设备 | `{"duration_min":30}` |
| clean | 执行清洗 | `{"type":"quick"/"deep"}` |
| reset_config | 恢复出厂配置 | 无 |

**示例 — 重启**:

```json
{
    "command": "restart",
    "timestamp": "2026-07-20T12:00:00Z"
}
```

---

## 6. 消息序列示例

### 6.1 设备正常运行流程

```
1. 设备上电
   → HTTP POST /api/v1/device/activate
   → 获得 device_id, broker_uri

2. MQTT 连接
   → CONNECT (clientId=device_id)
   → SUBSCRIBE {device_id}/v1/property/set
   → SUBSCRIBE {device_id}/v1/ota/notify
   → SUBSCRIBE {device_id}/v1/command/+
   → PUBLISH {device_id}/v1/status {"network_status":0,...}

3. 正常运行 (循环)
   → 每30s: PUBLISH {device_id}/v1/heartbeat {...}
   → 每60s: PUBLISH {device_id}/v1/property/post {...}

4. 制作流程
   → 开始制作 → 心跳 work_status=1 (BREWING)
   → 制作完成 → PUBLISH {device_id}/v1/event/post
     {"event_type":"order_completed","order_id":"ORD-xxx","recipe_id":"...","recipe_name":"经典美式","cup_size":"中"}
   → 心跳 work_status=0 (IDLE)

5. 故障处理
   → 故障触发 → PUBLISH {device_id}/v1/event/post
     {"event_type":"fault_alert","fault_code":3,...}
   → 心跳 work_status=2 (FAULT)
   → 恢复 → PUBLISH {device_id}/v1/event/post
     {"event_type":"fault_resolved","fault_code":3}
   → 心跳 work_status=0 (IDLE)

6. OTA 升级
   → 收到下行: {device_id}/v1/ota/notify
   → 下载固件 → 上报进度: {device_id}/v1/ota/progress
   → 安装 → 重启 → work_status=3 (UPGRADING)
```

### 6.2 Will Message 离线检测

```
设备异常断线
  → Broker 感知连接断开
  → Broker 自动发布 Will Message:
    {device_id}/v1/status {"network_status":1,"work_status":0}
  → dev-sys-cloud 收到，标记设备 OFFLINE
```

---

## 7. 错误处理

### 7.1 MQTT 断线重连

- 指数退避: 1s → 2s → 4s → 8s → ... → 60s (上限)
- 重连后无需重新激活
- 离线期间数据本地缓存，恢复后补传

### 7.2 HTTP 错误

| HTTP状态码 | 说明 | 设备行为 |
|-----------|------|---------|
| 200/201 | 成功 | 正常 |
| 400 | 请求参数错误 | 检查日志，不重试 |
| 401 | 未授权 | 重新激活 |
| 404 | 资源不存在 | 记录日志 |
| 500 | 服务端错误 | 指数退避重试（最多3次） |

---

> **文档维护**: 消息格式变更需同步更新本文档，并通知设备端和后端团队。
