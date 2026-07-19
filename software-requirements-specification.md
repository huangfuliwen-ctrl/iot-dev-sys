# 软件需求规格说明书 (SRS)

## IoT云平台服务程序——多租户设备管理与消息路由

---

## 文档信息

| 项目 | 内容 |
|------|------|
| 文档名称 | IoT云平台服务程序软件需求规格说明书 |
| 版本号 | V2.2 |
| 创建日期 | 2026-06-26 |
| 修改日期 | 2026-06-27 |
| 状态 | 修订中 |
| 变更说明 | V2.2: 在4.1节明确划分MQTT与HTTP的通信职责边界，新增协议分工原则、MQTT/HTTP通信内容一览表、职责边界总结图 |

---

## 1. 引言

### 1.1 编写目的

本文档定义IoT云平台服务程序的软件需求。本服务作为云端核心服务，通过MQTT Broker连接所有终端设备（咖啡机、饮水机等），负责**多层级组织管理、账号权限控制、设备管理、订单处理、OTA升级编排、故障监控**等核心功能。

### 1.2 产品范围

- **多租户支撑**：通过MQTT Topic前缀 `{tenant_id}` 隔离不同租户
- **组织架构管理**：支持公司→部门→子部门多级树形组织架构，每级可独立管理下属设备和账号
- **账号权限体系**：基于RBAC的账号管理和权限控制，支持超级管理员、组织管理员、部门管理员、运营人员、只读用户等多级角色
- **数据隔离**：账号登录后仅可见其所属组织范围内的设备、订单、故障等数据
- **全设备接入**：通过MQTT通配符订阅，消费所有租户下所有设备的上行消息
- **设备类型**：现磨咖啡机、速溶饮料机、饮水机、其他自动售卖终端

### 1.3 术语与缩略语

| 术语 | 说明 |
|------|------|
| IoT | 物联网 |
| MQTT | 消息队列遥测传输协议（本系统核心通信协议） |
| OTA | 空中下载升级 |
| Tenant | 租户，多租户隔离的基本单元 |
| Org（组织） | 公司、部门或子部门的统称，组织架构中的节点 |
| Account（账号） | 登录本系统的用户账号，归属于某个组织 |
| Role（角色） | 权限的集合，一个账号绑定一个角色 |
| RBAC | 基于角色的访问控制（Role-Based Access Control） |

### 1.4 运行环境

| 项目 | 要求 |
|------|------|
| 操作系统 | Linux (Ubuntu 22.04+ / CentOS 8+) |
| 硬件平台 | x86_64 服务器 |
| 内存 | ≥ 4GB RAM |
| 存储 | ≥ 50GB SSD |
| 网络 | 千兆以太网 |

---

## 2. 总体描述

### 2.1 系统架构

```
┌──────────────────────────────────────────────────┐
│                 MQTT Broker                       │
│                                                  │
│   +/v1/...  +/v1/...    │
│         ▲                       ▲                │
│         │  (设备上报)            │                │
│    ┌────┴───┐             ┌────┴───┐             │
│    │ 设备A   │             │ 设备B   │   ...      │
│    │(咖啡机) │             │(饮水机) │             │
│    └────────┘             └────────┘             │
└──────────────────────────────────────────────────┘
         │                       │
         │  通配符订阅:           │
         │  +/v1/event/post │
         │  +/v1/heartbeat  │
         │  +/v1/ota/progress
         ▼                       ▼
┌──────────────────────────────────────────────────┐
│           云平台服务程序 (本系统)                   │
│                                                  │
│  ┌─────────────────────────────────────────────┐ │
│  │         消息路由层 (Topic Parser)            │ │
│  │   tenant_id + product_id + device_id 解析    │ │
│  └──────────────┬──────────────────────────────┘ │
│     ┌───────────┼───────────┬──────────┐         │
│     ▼           ▼           ▼          ▼         │
│ ┌───────┐ ┌───────┐ ┌───────┐ ┌──────────┐      │
│ │设备   │ │订单   │ │OTA    │ │故障      │      │
│ │管理   │ │处理   │ │编排   │ │监控      │      │
│ └───┬───┘ └───┬───┘ └───┬───┘ └────┬─────┘      │
│     │         │         │          │             │
│     ▼         ▼         ▼          ▼             │
│  ┌──────────────────────────────────────────┐    │
│  │         PostgreSQL / MySQL               │    │
│  │   设备表/订单表/配方表/租户表/故障记录     │    │
│  └──────────────────────────────────────────┘    │
│                                                  │
│  下行指令:                                        │
│  publish → {device}/v1/command/{cmd}   │
└──────────────────────────────────────────────────┘
```

### 2.2 核心设计：多租户消息路由

云平台服务通过**MQTT通配符订阅**接收所有设备消息，通过**Topic解析**路由到对应业务模块：

```
设备上行Topic: {device_id}/v1/event/post
                                  │
                    MQTT订阅: +/v1/event/post
                                  │
                    收到消息后解析topic各段:
                      tenant_id  = "tenant_1"
                      product_id = "coffee_v1"
                      device_id  = "device_001"
                                  │
                         ┌───────┼───────┐
                         ▼       ▼       ▼
                      Tenant   Device   Message
                      Context  Lookup   Dispatch
```

**下行指令**则反向构造精确topic直发目标设备：

```
发送OTA升级通知给 tenant_1/device_001:
publish → dev_001/v1/ota/notify {payload}
```

### 2.2 核心功能模块

**功能模块（从设备端变为云服务端视角）：**

1. **组织管理** — 公司/部门多级树形架构、账号生命周期管理、基于RBAC的权限控制
2. **设备管理** — 多租户设备注册激活、心跳监控、在线状态追踪
3. **消息路由** — MQTT通配符订阅→Topic解析→业务分发
4. **订单处理** — 接收设备订单事件、状态追踪
5. **OTA编排** — 升级包管理、灰度推送、升级进度追踪
6. **故障监控** — 全设备故障告警接收、分级通知

**MQTT消息流（云平台服务视角）：**

```
# 上行消费 (通配符订阅，接收所有设备消息)
订阅: +/v1/event/post       ← 所有设备事件
订阅: +/v1/property/post    ← 所有设备属性
订阅: +/v1/heartbeat        ← 所有设备心跳
订阅: +/v1/ota/progress     ← 所有OTA进度

# 下行发送 (精确topic，定向到具体设备)
发布: {device}/v1/property/set
发布: {device}/v1/ota/notify
发布: {device}/v1/command/{cmd}
```

---

## 3. 功能性需求

### 3.1 设备管理

#### 3.1.1 设备类型与型号定义

| 需求编号 | REQ-DM-001 |
|----------|------------|
| 名称 | 设备类型与型号管理 |
| 优先级 | 高 |

**设备类型**为一组设备的大类划分（如咖啡机、饮水机）；**设备型号**为某类型下的具体产品型号（如某品牌某代咖啡机），每个型号归属一个设备类型。

**设备类型数据模型：**

| 字段 | 类型 | 说明 |
|------|------|------|
| type_id | int | 类型唯一ID，自增主键 |
| type_code | string | 类型编码，如 COFFEE_MACHINE |
| type_name | string | 类型名称，如 现磨咖啡机 |
| description | string | 类型说明 |
| is_active | bool | 是否启用 |
| created_at | datetime | 创建时间 |
| updated_at | datetime | 更新时间 |

系统预置默认设备类型：

| type_id | type_code | type_name | 说明 |
|---------|-----------|-----------|------|
| 1 | COFFEE_MACHINE | 现磨咖啡机 | 默认预置 |
| 2 | INSTANT_MACHINE | 速溶饮料机 | 默认预置 |
| 3 | WATER_DISPENSER | 饮水机 | 默认预置 |
| 4 | OTHER | 其他终端 | 默认预置 |

**设备型号数据模型：**

| 字段 | 类型 | 说明 |
|------|------|------|
| model_id | int | 型号唯一ID，自增主键 |
| type_id | int | 所属设备类型ID（外键） |
| model_code | string | 型号编码，如 CM-2000 |
| model_name | string | 型号名称，如 云魔方CM2000 |
| manufacturer | string | 生产厂商 |
| spec_info | json | 规格信息（功率、容量、尺寸等） |
| firmware_base | string | 基础固件版本 |
| is_active | bool | 是否启用 |
| created_at | datetime | 创建时间 |
| updated_at | datetime | 更新时间 |

**需求描述：**

1. 设备类型和设备型号由云平台统一管理，支持动态增删改查（详见 [3.1.2 设备类型与型号管理API](#312-设备类型与型号管理api)）
2. 设备出厂时预设设备型号（model_code），存储在设备配置中
3. 设备激活时上报设备型号至云平台，云平台自动关联对应类型
4. 云平台可根据设备类型/型号下发对应的配方、配置和升级包
5. 设备类型和型号信息缓存在设备本地，云端变更后同步更新
6. 已关联设备的型号不可删除，类型下有型号时不可删除

---

#### 3.1.2 设备类型与型号管理API

| 需求编号 | REQ-DM-002 |
|----------|------------|
| 名称 | 设备类型与型号管理API |
| 优先级 | 高 |

本接口集供云平台前端（管理后台）调用，实现设备类型和型号的动态管理。所有接口遵循 RESTful 风格，通信协议 HTTPS，数据格式 JSON（UTF-8）。

**通用约定：**

| 项目 | 规格 |
|------|------|
| 基础路径 | `/api/v1` |
| 认证方式 | Bearer Token（JWT） |
| 请求头 | `Content-Type: application/json` |
| 响应格式 | `{"code": 0, "message": "success", "data": {...}}` |
| 分页参数 | `?page=1&page_size=20`，响应含 `total`, `page`, `page_size`, `list` |

**通用响应码：**

| code | 说明 |
|------|------|
| 0 | 成功 |
| 1001 | 参数校验失败 |
| 1002 | 资源不存在 |
| 1003 | 资源冲突（重复/有关联数据） |
| 1004 | 未授权/Token过期 |
| 2000 | 服务端内部错误 |

---

**3.1.2.1 设备类型API**

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | /api/v1/device-types | 查询设备类型列表（支持分页、模糊搜索） |
| GET | /api/v1/device-types/:type_id | 查询单个设备类型详情 |
| POST | /api/v1/device-types | 新增设备类型 |
| PUT | /api/v1/device-types/:type_id | 修改设备类型 |
| DELETE | /api/v1/device-types/:type_id | 删除设备类型（无关联型号时） |

**GET /api/v1/device-types** —— 查询参数：

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| page | int | 否 | 页码，默认1 |
| page_size | int | 否 | 每页条数，默认20，最大100 |
| keyword | string | 否 | 模糊搜索 type_code / type_name |
| is_active | bool | 否 | 按启用状态筛选 |

响应 `data.list` 中每项为设备类型对象（字段见 [3.1.1 数据模型](#311-设备类型与型号定义)）。

**POST /api/v1/device-types** —— 请求体：

```json
{
  "type_code": "SMART_CABINET",
  "type_name": "智能货柜",
  "description": "智能零售货柜终端",
  "is_active": true
}
```

校验规则：
- `type_code`：必填，字母+下划线，2-64字符，全局唯一
- `type_name`：必填，1-64字符
- `description`：可选，最大256字符

**PUT /api/v1/device-types/:type_id** —— 请求体：

```json
{
  "type_name": "智能零售货柜",
  "description": "支持制冷制热的智能货柜",
  "is_active": true
}
```

注：`type_code` 不可修改；其余字段均可更新。

**DELETE /api/v1/device-types/:type_id**：

仅当该类型下无关联设备型号时允许删除。存在关联型号时返回 `code: 1003`。

---

**3.1.2.2 设备型号API**

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | /api/v1/device-models | 查询设备型号列表（支持分页、按类型筛选） |
| GET | /api/v1/device-models/:model_id | 查询单个设备型号详情 |
| POST | /api/v1/device-models | 新增设备型号 |
| PUT | /api/v1/device-models/:model_id | 修改设备型号 |
| DELETE | /api/v1/device-models/:model_id | 删除设备型号（无关联设备时） |

**GET /api/v1/device-models** —— 查询参数：

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| page | int | 否 | 页码，默认1 |
| page_size | int | 否 | 每页条数，默认20，最大100 |
| type_id | int | 否 | 按设备类型筛选 |
| keyword | string | 否 | 模糊搜索 model_code / model_name / manufacturer |
| is_active | bool | 否 | 按启用状态筛选 |

响应 `data.list` 中每项为设备型号对象（字段见 [3.1.1 数据模型](#311-设备类型与型号定义)），并附带关联的 `type_code` 和 `type_name`。

**POST /api/v1/device-models** —— 请求体：

```json
{
  "type_id": 1,
  "model_code": "CM-2000",
  "model_name": "云魔方CM2000",
  "manufacturer": "云魔方科技",
  "spec_info": {
    "power_w": 2200,
    "capacity_l": 15,
    "dimensions_mm": "450x500x650",
    "weight_kg": 28
  },
  "firmware_base": "v2.1.0",
  "is_active": true
}
```

校验规则：
- `type_id`：必填，必须为已存在的设备类型
- `model_code`：必填，字母+数字+连字符，2-64字符，全局唯一
- `model_name`：必填，1-128字符
- `manufacturer`：可选，最大128字符
- `spec_info`：可选，JSON对象，存储规格参数
- `firmware_base`：可选，语义版本号格式

**PUT /api/v1/device-models/:model_id** —— 请求体：

```json
{
  "model_name": "云魔方CM2000 Pro",
  "manufacturer": "云魔方科技有限公司",
  "spec_info": {
    "power_w": 2400,
    "capacity_l": 18,
    "dimensions_mm": "480x520x680",
    "weight_kg": 30
  },
  "firmware_base": "v2.2.0",
  "is_active": true
}
```

注：`type_id` 和 `model_code` 不可修改。

**DELETE /api/v1/device-models/:model_id**：

仅当该型号下无已注册设备时允许删除。存在关联设备时返回 `code: 1003`。

---

**验收标准：**
- API 平均响应时间 ≤ 200ms
- 单页查询最大支持 100 条/页
- 并发写入无数据冲突

---

#### 3.1.3 设备注册与激活

| 需求编号 | REQ-DM-003 |
|----------|------------|
| 名称 | 设备注册激活 |
| 优先级 | 高 |

**请求参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| uid | string | 硬件唯一标识（芯片序列号/Secure Element ID） |
| model_key | string | 设备型号Key（26字符ULID，出厂烧录） |

**流程：**

1. 设备首次上电，读取 `uid`（芯片序列号）和 `model_key`（出厂烧录的26字符ULID）
2. 向云平台发起激活请求，上传 `uid` + `model_key`（**设备端不可指定租户**）
3. 云平台根据 `model_key` 查找设备型号，自动解析：设备类型（COFFEE_MACHINE等）、产品ID、基础固件版本
4. 云平台从数据库 `mqtt_tenant_config` 表读取当前配置的租户（tenant_id），分配给设备
5. 云平台分配 `device_id` 并生成 `activation_token`（256位随机hex）
6. 返回：device_id, tenant_id, model_code, device_type, product_id, firmware_version, token, broker_uri
7. 设备将凭证安全存储
8. 激活成功进入正常运行模式
9. 激活失败：首次间隔30秒重试，之后指数退避，最大间隔5分钟

**租户分配规则：**
- 租户由云平台统一配置（通过 `mqtt_tenant_config` 表或管理后台）
- **设备端不可指定租户**，防止越权接入
- 设备激活后归属于云平台当前配置的租户，如需变更需通过管理后台迁移

**验收标准：**
- 激活完成时间 ≤ 10秒（正常网络）
- 凭证存储不可被非授权读取
- 设备端仅需2个参数（uid + model_key），其余由云端解析

---

#### 3.1.4 设备心跳

| 需求编号 | REQ-DM-004 |
|----------|------------|
| 名称 | 心跳保活 |
| 优先级 | 高 |

**需求描述：**

1. 心跳间隔默认60秒，可云端配置（30~600秒）
2. 心跳包内容：

| 字段 | 说明 |
|------|------|
| device_id | 设备ID |
| timestamp | 当前时间戳 |
| network_status | 网络状态（0=在线, 1=离线） |
| work_status | 工作状态（0=空闲, 1=制作中, 2=故障, 3=升级中, 4=维护） |
| firmware_version | 固件版本 |
| signal_strength | 网络信号强度 |
| alarm_count | 当前告警计数 |

3. 连续3次心跳无响应判定离线，触发重连
4. 离线期间数据本地缓存，恢复后补传
5. 使用MQTT Will Message机制，异常断线时通知平台

**验收标准：**
- 心跳偏差 ≤ ±2秒
- 断线后15秒内完成重连

---

### 3.2 配方管理

#### 3.2.1 配方数据结构

| 需求编号 | REQ-RC-001 |
|----------|------------|
| 名称 | 配方数据模型 |
| 优先级 | 高 |

| 字段 | 类型 | 说明 |
|------|------|------|
| recipe_id | string | 配方唯一ID |
| recipe_name | string | 配方名称 |
| device_type | int | 适用设备类型 |
| category | string | 品类：咖啡/茶饮/热水/冰水/其他 |
| steps | array | 制作步骤列表 |
| step.action | string | 动作：grind/brew/dispense/mix/heat/water/wait |
| step.duration_ms | int | 步骤持续时长（毫秒） |
| step.params | object | 步骤参数（temperature/volume/speed等） |
| cup_sizes | array | 可选杯型及对应价格 [{size: "小", price: 1500, volume_ml: 200}, ...] |
| is_active | bool | 是否启用 |
| version | int | 配方版本号 |

**需求描述：**

1. 设备本地缓存全部在线配方
2. 配方由云端统一下发，支持增量同步（仅下发变更的配方）
3. 配方变更不影响正在进行的制作任务
4. 离线时使用本地缓存配方
5. 配方参数支持云端远程调整（如温度、浓度）

---

### 3.3 订单处理

#### 3.3.1 订单数据结构

| 需求编号 | REQ-OR-001 |
|----------|------------|
| 名称 | 订单数据模型 |
| 优先级 | 高 |

| 字段 | 类型 | 说明 |
|------|------|------|
| order_id | string | 订单唯一编号 |
| device_id | string | 设备ID |
| recipe_id | string | 配方ID |
| cup_size | string | 杯型：小/中/大 |
| quantity | int | 数量，默认1 |
| total_amount | int | 总金额（分） |
| payment_method | string | 支付方式：wechat/alipay/member |
| order_status | string | 见订单状态码（附录7.3） |
| created_at | string | 下单时间（ISO 8601 UTC） |
| expired_at | string | 订单过期时间 |
| notify_url | string | 支付回调URL（可选） |

---

#### 3.3.2 订单处理流程

| 需求编号 | REQ-OR-002 |
|----------|------------|
| 名称 | 订单处理流程 |
| 优先级 | 高 |

**订单来源：**
1. 用户扫码下单（设备屏幕展示二维码，用户小程序下单）
2. 设备端现场选购（屏幕直接选择商品并支付）

**处理流程：**

```
下单 → 支付确认 → 制作前自检 → 执行制作 → 完成通知 → 订单完成
                        ↓
                    自检失败 → 取消订单/退款
```

**详细需求：**

1. 订单接收后本地缓存，有效期15分钟（可配置），过期自动取消
2. 多订单按FIFO排队，队列深度最大10单
3. 支付确认后进入制作队列，通知用户预计等待时间
4. 制作前自检：
   - 检查原料是否充足
   - 检查设备状态是否正常
   - 任一条件不满足则拒绝制作，取消订单
5. 制作过程：
   - 按配方步骤依次执行
   - 实时监控关键传感器（温度、流量）
   - 超时（配方理论时长 × 1.5）强制中止
   - 支持紧急停止
6. 制作完成通知用户取饮品
7. 制作失败则自动取消订单并退款
8. 订单数据本地保留7天

**制作状态机：**

```
IDLE → PREPARING → BREWING → DISPENSING → DONE → IDLE
  │                    ↓
  └──── ERROR（任意环节）──→ IDLE
```

**验收标准：**
- 订单接收延迟 ≤ 1秒
- 单杯制作成功率 ≥ 99.5%
- 紧急停止响应 ≤ 200ms

#### 3.3.3 订单状态同步

| 需求编号 | REQ-OR-003 |
|----------|------------|
| 名称 | 订单状态上报 |
| 优先级 | 高 |

**需求描述：**

1. 订单状态变更时实时上报云端（通过`event/post` topic）
2. 上报时机：
   - 订单创建（待支付）
   - 支付确认（已支付）
   - 开始制作（制作中）
   - 制作完成（已完成）
   - 订单取消/失败
3. 上报内容：order_id + 新状态 + 时间戳 + 附加信息（失败原因等）
4. 网络中断时状态变更本地记录，恢复连接后按时间顺序补传
5. 云端可查询设备端订单列表和详情（通过HTTPS API）

---

### 3.4 设备配置同步

| 需求编号 | REQ-CF-001 |
|----------|------------|
| 名称 | 设备配置云端同步 |
| 优先级 | 高 |

**可同步的配置项：**

| 配置项 | 说明 | 默认值 |
|--------|------|--------|
| heartbeat_interval | 心跳间隔（秒） | 60 |
| auto_clean_interval | 自动冲洗杯数间隔 | 20 |
| idle_clean_delay | 空闲超时冲洗（分钟） | 30 |
| screen_brightness | 屏幕亮度（%） | 80 |
| volume_level | 音量（%） | 70 |
| energy_save_mode | 节能模式开关 | false |
| order_expire_minutes | 订单过期时间（分钟） | 15 |
| max_queue_depth | 最大排队订单数 | 10 |

**同步机制：**

1. 云端通过MQTT下发期望配置（`property/set` topic）
2. 设备收到后应用配置，并通过`property/post`上报实际值
3. 设备启动时主动向云端拉取最新配置
4. 配置项支持热更新，无需重启设备
5. 配置变更记录本地日志，包含变更前后值和来源

---

### 3.5 OTA固件升级

#### 3.5.1 升级流程

| 需求编号 | REQ-OT-001 |
|----------|------------|
| 名称 | OTA固件远程升级 |
| 优先级 | 高 |

**升级流程：**

```
云端推送升级通知 → 设备收到通知
                      ↓
              设备下载升级包（HTTPS，断点续传）
                      ↓
              下载完成 → 完整性校验(MD5/SHA256) + 签名验证
                      ↓
              等待设备空闲（无订单、非制作中）
                      ↓
              备份当前固件 → 安装新固件
                      ↓
              安装成功 → 重启设备 → 功能验证
                      ↓
              上报升级结果至云端

         安装失败 → 自动回滚至上一版本 → 上报失败原因
```

**详细需求：**

1. **触发方式：**
   - 云端推送升级通知，设备收到后自动下载
   - 设备定时检查更新（默认每日凌晨3:00）
2. **下载：**
   - 支持HTTP/HTTPS断点续传
   - 下载过程不影响设备正常制作
   - 下载完成校验MD5/SHA256和数字签名
3. **安装：**
   - 仅在设备空闲时执行
   - 支持A/B分区升级
   - 安装前自动备份当前固件
   - 安装过程显示进度，拒绝新请求
   - 安装完成后自动重启验证
4. **异常处理：**
   - 下载中断：恢复后从断点继续
   - 安装失败：自动回滚至上一版本
   - 升级后连续3次启动失败：回滚
   - 回滚后上报平台等待人工介入
5. **升级策略：**
   - 支持按设备类型/批次灰度推送
   - 支持强制升级（安全漏洞修复）

**验收标准：**
- 下载成功率 ≥ 99%
- 安装完成时间 ≤ 5分钟（不含下载）
- 失败自动回滚成功率 100%
- 升级期间设备不可用 ≤ 5分钟

---

### 3.6 故障检测

| 需求编号 | REQ-FD-001 |
|----------|------------|
| 名称 | 基础故障检测与告警 |
| 优先级 | 高 |

**故障分级：**

| 级别 | 名称 | 说明 | 设备行为 |
|------|------|------|---------|
| L1 | 轻微 | 部分功能受限 | 限制关联功能，屏幕提示 |
| L2 | 一般 | 影响使用 | 暂停部分功能，告警上报 |
| L3 | 严重 | 影响正常运行 | 暂停制作，等待维修 |
| L4 | 危险 | 安全隐患 | 立即停机，切断电源 |

**检测项目：**

| 检测项 | 检测方式 | 级别 | 周期 |
|--------|---------|------|------|
| 加热器异常 | 温度传感器 | L3 | 1秒 |
| 水泵异常 | 流量传感器 | L3 | 1秒 |
| 电机异常 | 电流检测 | L3 | 500ms |
| 漏水检测 | 漏水传感器 | L4 | 500ms |
| 通信模块异常 | 心跳超时 | L2 | 5秒 |
| 传感器离线 | 通信超时 | L2 | 5秒 |
| 原料不足 | 传感器阈值 | L1 | 10秒 |

**需求描述：**

1. L3/L4级故障即时告警上报，L1/L2级合并周期上报（5分钟）
2. L4级故障设备自动进入安全锁定状态，需人工现场复位
3. 告警信息包含：设备ID、故障码、故障描述、发生时间
4. 故障恢复后发送恢复通知

**验收标准：**
- L4级故障检测到保护动作 ≤ 200ms
- 告警上报延迟（L3/L4） ≤ 3秒

---

### 3.7 组织架构与权限管理

#### 3.7.1 组织架构模型

| 需求编号 | REQ-ORG-001 |
|----------|-------------|
| 名称 | 多层级组织架构 |
| 优先级 | 高 |

**组织架构**为树形层级结构，根节点为**公司**，公司下可建**部门**，部门下可建**子部门**，支持无限层级嵌套。每个组织节点在MQTT Topic中映射为一个 `tenant_id`，实现数据隔离。

**组织数据模型：**

| 字段 | 类型 | 说明 |
|------|------|------|
| org_id | int | 组织唯一ID，自增主键 |
| parent_id | int | 父组织ID，0表示根节点（公司） |
| tenant_id | string | 唯一租户标识，用于MQTT Topic隔离（如 company_a / dept_rnd） |
| org_name | string | 组织名称，如 张三咖啡有限公司、研发部 |
| org_type | string | 组织类型：company / department / sub_department |
| contact_name | string | 联系人姓名 |
| contact_phone | string | 联系人电话 |
| contact_email | string | 联系人邮箱 |
| address | string | 地址 |
| is_active | bool | 是否启用 |
| level | int | 层级深度（0=公司, 1=一级部门, 2=二级部门...） |
| path | string | 组织路径，如 /1/5/12（根到当前节点的ID路径） |
| created_at | datetime | 创建时间 |
| updated_at | datetime | 更新时间 |

**层级约束：**

| 约束项 | 规则 |
|--------|------|
| 根节点（公司） | `parent_id = 0`，`org_type = "company"`，唯一或少量存在 |
| 子节点（部门） | `parent_id` 指向已存在的父组织，`org_type = "department"` |
| 租户隔离 | 每个组织有唯一的 `tenant_id`，设备注册时绑定组织的 `tenant_id` |
| 删除保护 | 有子节点或有设备/账号关联时不可删除 |
| 路径 | `path` 自动维护，从根节点到当前节点的ID路径 |

**示例组织树：**

```
张三咖啡有限公司 (org_id=1, tenant_id=company_zhangsan, type=company)
├── 华东区 (org_id=2, tenant_id=dept_huadong, type=department)
│   ├── 上海运营中心 (org_id=5, tenant_id=dept_shanghai, type=department)
│   └── 杭州运营中心 (org_id=6, tenant_id=dept_hangzhou, type=department)
├── 华南区 (org_id=3, tenant_id=dept_huanan, type=department)
└── 研发部 (org_id=4, tenant_id=dept_rnd, type=department)
```

---

#### 3.7.2 账号与权限模型

| 需求编号 | REQ-ORG-002 |
|----------|-------------|
| 名称 | 账号管理与RBAC权限 |
| 优先级 | 高 |

**账号数据模型：**

| 字段 | 类型 | 说明 |
|------|------|------|
| account_id | int | 账号唯一ID，自增主键 |
| username | string | 登录用户名，全局唯一，4-64字符 |
| password_hash | string | 密码哈希（bcrypt/argon2） |
| display_name | string | 显示名称 |
| org_id | int | 所属组织ID（外键） |
| role_code | string | 角色编码（见角色码附录7.5） |
| email | string | 邮箱 |
| phone | string | 手机号 |
| is_active | bool | 是否启用 |
| last_login_at | datetime | 最后登录时间 |
| created_at | datetime | 创建时间 |
| updated_at | datetime | 更新时间 |

**角色定义（附录7.5）：**

| 角色编码 | 名称 | 权限范围 | 说明 |
|----------|------|---------|------|
| super_admin | 超级管理员 | 全局 | 管理所有组织和账号，系统最高权限 |
| org_admin | 组织管理员 | 本组织及子组织 | 管理部门、账号、设备；不可跨上级组织 |
| dept_admin | 部门管理员 | 本部门及子部门 | 管理设备配置、查看数据报表 |
| operator | 运营人员 | 本部门 | 日常运营操作（配方调整、订单查询） |
| viewer | 只读用户 | 本部门 | 仅查看数据，不可修改 |

**权限码定义（附录7.6）：**

| 权限码 | 资源 | 操作 | 说明 |
|--------|------|------|------|
| org:read | 组织 | 查看 | 查看组织信息 |
| org:write | 组织 | 增/改/删 | 创建/修改/删除组织 |
| account:read | 账号 | 查看 | 查看账号信息 |
| account:write | 账号 | 增/改/删 | 创建/修改/删除账号 |
| device:read | 设备 | 查看 | 查看设备列表和详情 |
| device:write | 设备 | 配置 | 修改设备配置、远程指令 |
| order:read | 订单 | 查看 | 查看订单列表和详情 |
| recipe:read | 配方 | 查看 | 查看配方 |
| recipe:write | 配方 | 增/改/删 | 管理配方 |
| ota:read | OTA | 查看 | 查看固件和升级状态 |
| ota:write | OTA | 推送 | 注册固件、推送升级 |
| fault:read | 故障 | 查看 | 查看故障列表 |
| fault:write | 故障 | 处理 | 确认/解决故障 |
| config:read | 配置 | 查看 | 查看服务配置 |
| config:write | 配置 | 修改 | 修改服务配置 |

**角色权限映射：**

| 权限码 | super_admin | org_admin | dept_admin | operator | viewer |
|--------|:--:|:--:|:--:|:--:|:--:|
| org:read | ✓ | ✓ | ✓ | ✗ | ✗ |
| org:write | ✓ | ✓ | ✗ | ✗ | ✗ |
| account:read | ✓ | ✓ | ✓ | ✗ | ✗ |
| account:write | ✓ | ✓ | ✗ | ✗ | ✗ |
| device:read | ✓ | ✓ | ✓ | ✓ | ✓ |
| device:write | ✓ | ✓ | ✓ | ✗ | ✗ |
| order:read | ✓ | ✓ | ✓ | ✓ | ✓ |
| recipe:read | ✓ | ✓ | ✓ | ✓ | ✓ |
| recipe:write | ✓ | ✓ | ✗ | ✓ | ✗ |
| ota:read | ✓ | ✓ | ✓ | ✓ | ✓ |
| ota:write | ✓ | ✓ | ✗ | ✗ | ✗ |
| fault:read | ✓ | ✓ | ✓ | ✓ | ✓ |
| fault:write | ✓ | ✓ | ✓ | ✓ | ✗ |
| config:read | ✓ | ✓ | ✗ | ✗ | ✗ |
| config:write | ✓ | ✗ | ✗ | ✗ | ✗ |

**数据隔离规则：**

1. **按组织范围**：账号登录后，只能看到所属组织（`org_id`）及其所有子组织下的设备、订单、故障等数据
2. **组织范围计算**：根据 `org.path` 字段，查询所有 `path LIKE '{当前path}%'` 的组织节点
3. **超级管理员**：不设限制，可查看全局所有数据
4. **API鉴权**：每个请求携带JWT Token，中间件解析后注入 `account_id`、`org_id`、`org_scope`（可见组织ID列表）、`role_code`
5. **跨组织操作**：仅超级管理员和组织管理员可操作下属组织数据

---

#### 3.7.3 组织架构管理API

| 需求编号 | REQ-ORG-003 |
|----------|-------------|
| 名称 | 组织架构CRUD API |
| 优先级 | 高 |

| 方法 | 路径 | 说明 | 权限 |
|------|------|------|------|
| GET | /api/v1/orgs | 查询组织列表（树形结构） | org:read |
| GET | /api/v1/orgs/tree | 获取完整组织树 | org:read |
| GET | /api/v1/orgs/:org_id | 查询单个组织详情 | org:read |
| POST | /api/v1/orgs | 新建组织（公司或部门） | org:write |
| PUT | /api/v1/orgs/:org_id | 修改组织信息 | org:write |
| DELETE | /api/v1/orgs/:org_id | 删除组织（无子节点/关联时） | org:write |

**GET /api/v1/orgs** —— 查询参数：

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| parent_id | int | 否 | 按父组织ID筛选（0=仅公司） |
| org_type | string | 否 | 按类型筛选：company / department |
| keyword | string | 否 | 模糊搜索 org_name / tenant_id |
| is_active | bool | 否 | 按启用状态筛选 |

响应 `data.list` 中每项为组织对象，含 `children_count`（下级组织数）。

**GET /api/v1/orgs/tree** —— 返回完整树形JSON：

```json
{
  "code": 0,
  "data": {
    "tree": [{
      "org_id": 1,
      "org_name": "张三咖啡有限公司",
      "org_type": "company",
      "tenant_id": "company_zhangsan",
      "children": [{
        "org_id": 2,
        "org_name": "华东区",
        "org_type": "department",
        "tenant_id": "dept_huadong",
        "children": [...]
      }]
    }]
  }
}
```

**POST /api/v1/orgs** —— 请求体：

```json
{
  "parent_id": 0,
  "tenant_id": "company_newcoffee",
  "org_name": "新咖啡科技有限公司",
  "org_type": "company",
  "contact_name": "李四",
  "contact_phone": "13900001111",
  "contact_email": "lisi@newcoffee.com",
  "address": "北京市朝阳区xxx号"
}
```

校验规则：
- `parent_id`：必填，0=公司，非0则父组织必须存在
- `tenant_id`：必填，字母+数字+下划线+连字符，2-64字符，全局唯一
- `org_name`：必填，1-128字符
- 父组织为公司（`org_type=company`）时，`org_type` 只能为 `department`

**PUT /api/v1/orgs/:org_id** —— 请求体：

```json
{
  "org_name": "华东大区（更新）",
  "contact_name": "王五",
  "contact_phone": "13911112222",
  "is_active": true
}
```

注：`parent_id`、`tenant_id`、`org_type` 不可修改；只能停用/启用。

**DELETE /api/v1/orgs/:org_id**：
- 存在子组织时返回 `code: 1003`，提示"请先删除所有子组织"
- 存在关联账号时返回 `code: 1003`，提示"请先删除/转移关联账号"
- 存在关联设备时返回 `code: 1003`，提示"请先转移关联设备"

---

#### 3.7.4 账号管理API

| 需求编号 | REQ-ORG-004 |
|----------|-------------|
| 名称 | 账号CRUD与认证API |
| 优先级 | 高 |

| 方法 | 路径 | 说明 | 权限 |
|------|------|------|------|
| POST | /api/v1/auth/login | 账号登录（获取JWT Token） | 公开 |
| POST | /api/v1/auth/logout | 账号登出（Token失效） | 登录 |
| GET | /api/v1/auth/profile | 获取当前登录账号信息 | 登录 |
| GET | /api/v1/accounts | 查询账号列表（分页） | account:read |
| GET | /api/v1/accounts/:account_id | 查询单个账号详情 | account:read |
| POST | /api/v1/accounts | 创建账号 | account:write |
| PUT | /api/v1/accounts/:account_id | 修改账号信息 | account:write |
| PUT | /api/v1/accounts/:account_id/password | 修改密码 | account:write / 本人 |
| DELETE | /api/v1/accounts/:account_id | 删除/停用账号 | account:write |

**POST /api/v1/auth/login** —— 请求体：

```json
{
  "username": "zhangsan",
  "password": "xxxxxx"
}
```

响应：

```json
{
  "code": 0,
  "data": {
    "token": "eyJhbGciOiJIUzI1NiIs...",
    "expires_at": "2026-06-28T12:00:00Z",
    "account": {
      "account_id": 1,
      "username": "zhangsan",
      "display_name": "张三",
      "org_id": 1,
      "org_name": "张三咖啡有限公司",
      "role_code": "super_admin",
      "permissions": ["org:read", "org:write", ...]
    }
  }
}
```

**JWT Token Payload：**

```json
{
  "sub": "1",
  "username": "zhangsan",
  "org_id": 1,
  "org_scope": [1, 2, 3, 4, 5, 6],
  "role_code": "super_admin",
  "permissions": ["org:read", "org:write", "device:read", ...],
  "iat": 1719456000,
  "exp": 1719542400
}
```

**GET /api/v1/accounts** —— 查询参数：

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| page | int | 否 | 页码，默认1 |
| page_size | int | 否 | 每页条数，默认20 |
| org_id | int | 否 | 按所属组织筛选 |
| role_code | string | 否 | 按角色筛选 |
| keyword | string | 否 | 模糊搜索 username / display_name / email |
| is_active | bool | 否 | 按启用状态筛选 |

**POST /api/v1/accounts** —— 请求体：

```json
{
  "username": "wangwu",
  "password": "secureP@ss123",
  "display_name": "王五",
  "org_id": 2,
  "role_code": "dept_admin",
  "email": "wangwu@example.com",
  "phone": "13800001111"
}
```

校验规则：
- `username`：必填，字母+数字+下划线，4-64字符，全局唯一
- `password`：必填，8-128字符，须含大小写字母+数字
- `org_id`：必填，组织必须存在
- `role_code`：必填，必须为有效角色码
- 非超级管理员不可创建 `super_admin` 角色账号

**PUT /api/v1/accounts/:account_id/password** —— 请求体：

```json
{
  "old_password": "oldP@ss123",
  "new_password": "newSecureP@ss456"
}
```

---

#### 3.7.5 权限校验与数据隔离

| 需求编号 | REQ-ORG-005 |
|----------|-------------|
| 名称 | API鉴权中间件与数据隔离 |
| 优先级 | 高 |

**鉴权流程：**

```
HTTP Request
    │
    ▼
┌─────────────────────┐
│ 1. 提取 Authorization│   Bearer Token
│    Header           │
└─────────┬───────────┘
          ▼
┌─────────────────────┐
│ 2. 验证JWT签名       │   失败 → 401 Unauthorized
│    + 过期检查         │
└─────────┬───────────┘
          ▼
┌─────────────────────┐
│ 3. 解析Payload      │
│    account_id       │
│    org_id            │
│    org_scope[]       │   组织ID列表（含子组织）
│    role_code         │
│    permissions[]     │
└─────────┬───────────┘
          ▼
┌─────────────────────┐
│ 4. 权限校验          │   检查 permissions[]
│                     │   含所需权限码？          否→ 403 Forbidden
└─────────┬───────────┘
          ▼
┌─────────────────────┐
│ 5. 数据过滤          │   查询时自动附加
│                     │   WHERE org_id IN (org_scope)
└─────────┬───────────┘
          ▼
    执行业务逻辑
```

**数据隔离实现规则：**

1. **设备查询**：`WHERE org_id IN ({account.org_scope})` — 仅返回可见组织范围内的设备
2. **订单查询**：通过设备关联 → `WHERE device.org_id IN ({account.org_scope})`
3. **故障查询**：通过设备关联 → `WHERE device.org_id IN ({account.org_scope})`
4. **OTA/配方**：全局可用（技术资源，非业务数据）
5. **组织/账号管理**：根据 `org:read`/`org:write` 权限控制
6. **MQTT消息**：Broker ACL层根据设备所属 `tenant_id` 控制订阅权限

**验收标准：**
- 登录响应时间 ≤ 1秒
- JWT Token有效期：默认24小时，可配置
- 鉴权失败的请求一律返回 401/403，不得泄露数据
- 数据隔离100%生效：跨组织数据泄漏为最高级别安全缺陷

---

## 4. 外部接口

### 4.1 云平台通信协议选型

| 需求编号 | REQ-IF-001 |
|----------|------------|
| 名称 | 设备-云平台通信协议 |
| 优先级 | 高 |

#### 4.1.1 协议分工原则

设备与云平台之间采用 **MQTT + HTTP 双协议**，各司其职：

| 维度 | MQTT | HTTP (HTTPS) |
|------|------|--------------|
| 通信模式 | 发布/订阅（Pub/Sub），支持推拉 | 请求/响应（Request/Response） |
| 连接方向 | 长连接，双向推送 | 短连接，设备主动发起 |
| 适用场景 | 实时数据流、事件推送、指令下发 | 大文件下载、一次性请求、批量同步 |
| 消息大小 | 单条 ≤ 256KB | 支持大文件（固件包可达数百MB） |
| QoS | QoS 1（至少一次送达） | 应用层重试保证 |
| 协议版本 | MQTT v3.1.1 / v5.0 | HTTP/1.1 over TLS 1.2+ |
| 数据格式 | JSON（UTF-8） | JSON（UTF-8） |
| Keep Alive | 60秒 | 不适用 |

#### 4.1.2 MQTT 通信（实时双向通道）

MQTT 是设备与云平台之间的**主要通信通道**，承载所有实时、高频、双向的数据交互。

**MQTT 通信内容一览：**

| 序号 | 数据类别 | 方向 | Topic | 说明 |
|------|---------|------|-------|------|
| 1 | 心跳保活 | 设备→云 | `{device}/v1/heartbeat` | 定期上报设备在线状态、当前状态码、固件版本、信号强度、告警计数 |
| 2 | 事件上报 | 设备→云 | `{device}/v1/event/post` | 订单状态变更（创建/支付/制作中/完成/取消/失败）、故障告警（L1-L4） |
| 3 | 属性上报 | 设备→云 | `{device}/v1/property/post` | 设备运行属性（温度、流量、液位、功率等传感器数据）、设备状态变更 |
| 4 | OTA进度上报 | 设备→云 | `{device}/v1/ota/progress` | 固件下载进度百分比、升级状态（下载中/安装中/成功/失败） |
| 5 | 配置下发 | 云→设备 | `{device}/v1/property/set` | 云端主动推送配置变更（心跳间隔、清洗参数、亮度音量等） |
| 6 | OTA升级通知 | 云→设备 | `{device}/v1/ota/notify` | 通知设备有新固件可用，包含下载URL、版本号、MD5/SHA256校验值 |
| 7 | 远程指令 | 云→设备 | `{device}/v1/command/{cmd}` | 远程控制指令（重启、紧急停止、维护模式切换、锁定/解锁等） |
| 8 | 遗嘱消息 | 设备→云 | MQTT Will Message | 设备异常断线时Broker自动发布离线通知，平台即时感知设备离线 |

**MQTT Topic 设计（多租户）：**

```
# 格式: {device_id}/v1/{message_type}

# === 上行 (设备 → 云平台) ===
{device_id}/v1/heartbeat       # 心跳保活
{device_id}/v1/event/post      # 事件上报（订单状态/故障告警）
{device_id}/v1/property/post   # 属性上报（传感器/设备状态）
{device_id}/v1/ota/progress    # OTA升级进度

# === 下行 (云平台 → 设备) ===
{device_id}/v1/property/set    # 配置下发
{device_id}/v1/ota/notify      # OTA升级通知
{device_id}/v1/command/{cmd}   # 远程指令

# === 云平台服务端消费（通配符订阅） ===
# 云端服务拥有全局权限，订阅通配符消费所有设备消息：
+/v1/event/post       # 所有租户所有设备事件
+/v1/heartbeat        # 所有租户所有设备心跳
+/v1/property/post    # 所有租户所有设备属性
+/v1/ota/progress     # 所有租户所有设备OTA进度
```

**MQTT 通信要求：**

1. 断线自动重连，指数退避（1s→2s→4s→...→60s上限）
2. 消息体JSON格式，单条最大256KB
3. 通信启用TLS双向认证（mTLS）
4. 租户隔离在Broker ACL层完成，设备仅能访问所属租户的Topic
5. 设备离线期间MQTT消息本地缓存，恢复连接后按时间顺序补传

#### 4.1.3 HTTP 通信（请求-响应通道）

HTTP 用于**不适合MQTT的场景**：大文件传输、设备首次激活、数据批量拉取、以及MQTT不可用时的降级通道。

**HTTP 通信内容一览：**

| 序号 | 数据类别 | 方法 | 路径 | 说明 |
|------|---------|------|------|------|
| 1 | 设备激活注册 | POST | `/api/v1/device/activate` | 设备首次上电，上报设备型号、固件版本、MAC地址，获取device_id和激活凭证 |
| 2 | 固件包下载 | GET | `/api/v1/device/ota/download` | 下载固件升级包（文件可达数百MB），支持断点续传（Range请求） |
| 3 | 固件更新检查 | GET | `/api/v1/device/ota/check` | 设备定时或手动检查是否有新固件 |
| 4 | OTA结果回调 | POST | `/api/v1/device/ota/callback` | OTA升级完成后回调通知云端（HTTP兜底，正常走MQTT ota/progress） |
| 5 | 配方同步 | GET | `/api/v1/recipes/sync` | 设备主动拉取配方增量更新（全量或增量，通过版本号/时间戳） |
| 6 | 配置拉取 | GET | `/api/v1/device/config` | 设备启动时主动拉取最新配置 |
| 7 | 批量事件补传 | POST | `/api/v1/device/events` | MQTT不可用或离线恢复后，批量补传事件数据 |
| 8 | 批量属性补传 | POST | `/api/v1/device/properties` | MQTT不可用或离线恢复后，批量补传属性数据 |
| 9 | 订单状态同步（降级） | POST | `/api/v1/orders/sync` | MQTT不可用时HTTP降级同步订单状态 |

**管理后台API（前端/管理端调用，非设备端）：**

以下API均为前端管理界面调用，全部走 HTTPS RESTful 接口：

**组织架构管理：**

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | /api/v1/orgs | 查询组织列表（树形结构） |
| GET | /api/v1/orgs/tree | 获取完整组织树 |
| GET | /api/v1/orgs/:org_id | 查询单个组织详情 |
| POST | /api/v1/orgs | 新建组织（公司或部门） |
| PUT | /api/v1/orgs/:org_id | 修改组织信息 |
| DELETE | /api/v1/orgs/:org_id | 删除组织 |

**账号与认证管理：**

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | /api/v1/auth/login | 账号登录（获取JWT） |
| POST | /api/v1/auth/logout | 账号登出 |
| GET | /api/v1/auth/profile | 获取当前登录账号信息 |
| GET | /api/v1/accounts | 查询账号列表 |
| GET | /api/v1/accounts/:account_id | 查询账号详情 |
| POST | /api/v1/accounts | 创建账号 |
| PUT | /api/v1/accounts/:account_id | 修改账号信息 |
| PUT | /api/v1/accounts/:account_id/password | 修改密码 |
| DELETE | /api/v1/accounts/:account_id | 删除/停用账号 |

**设备类型与型号管理：**

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | /api/v1/device-types | 查询设备类型列表 |
| GET | /api/v1/device-types/:type_id | 查询设备类型详情 |
| POST | /api/v1/device-types | 新增设备类型 |
| PUT | /api/v1/device-types/:type_id | 修改设备类型 |
| DELETE | /api/v1/device-types/:type_id | 删除设备类型 |
| GET | /api/v1/device-models | 查询设备型号列表 |
| GET | /api/v1/device-models/:model_id | 查询设备型号详情 |
| POST | /api/v1/device-models | 新增设备型号 |
| PUT | /api/v1/device-models/:model_id | 修改设备型号 |
| DELETE | /api/v1/device-models/:model_id | 删除设备型号 |

#### 4.1.4 MQTT 与 HTTP 职责边界总结

```
┌─────────────────────────────────────────────────────────────────┐
│                    设备 ↔ 云平台 通信分工                          │
│                                                                 │
│  ┌───────────────────────────┐  ┌─────────────────────────────┐ │
│  │     MQTT（长连接/实时）     │  │    HTTP（短连接/按需）        │ │
│  │                           │  │                             │ │
│  │  ✓ 心跳保活（60s周期）      │  │  ✓ 设备激活注册（一次性）      │ │
│  │  ✓ 事件上报（订单/故障）    │  │  ✓ 固件包下载（大文件/断点续传）│ │
│  │  ✓ 属性上报（传感器数据）    │  │  ✓ 配方同步（设备主动拉取）    │ │
│  │  ✓ OTA进度上报             │  │  ✓ 配置拉取（启动时获取）      │ │
│  │  ✓ 配置下发（云端主动推送）  │  │  ✓ 固件更新检查              │ │
│  │  ✓ OTA升级通知             │  │  ✓ MQTT降级通道（心跳/事件    │ │
│  │  ✓ 远程指令下发             │  │    补传/属性补传/订单同步）    │ │
│  │  ✓ 遗嘱消息（离线感知）      │  │                             │ │
│  │                           │  │  ─────────────────────       │ │
│  │  特点：                    │  │  管理后台API（前端调用）：      │ │
│  │  · 高频、实时、双向推送     │  │  · 组织架构CRUD              │ │
│  │  · 消息体小（≤256KB）      │  │  · 账号认证与CRUD            │ │
│  │  · 需要服务端主动推送       │  │  · 设备类型/型号管理          │ │
│  └───────────────────────────┘  └─────────────────────────────┘ │
│                                                                 │
│  协议降级策略：                                                   │
│  · 正常运行：MQTT为主通道，HTTP用于大文件下载和按需拉取              │
│  · MQTT不可用：心跳/事件/属性/订单同步 降级为HTTP POST上报          │
│  · 恢复连接：优先恢复MQTT通道，补传离线期间缓存数据                  │
└─────────────────────────────────────────────────────────────────┘
```

**验收标准：**
- MQTT消息端到端延迟 ≤ 500ms
- HTTP API平均响应时间 ≤ 200ms（管理后台API）/ ≤ 2s（固件下载建立连接）
- TLS安全链路建立 ≤ 5秒
- MQTT→HTTP降级切换时间 ≤ 10秒

---

### 4.2 硬件接口（摘要）

| 类型 | 接口 | 说明 |
|------|------|------|
| 温度传感器 | 1-Wire / ADC | 水温/锅炉温度监测 |
| 流量传感器 | GPIO中断 | 水量计量 |
| 液位传感器 | ADC / I²C | 水位监测 |
| 加热器 | GPIO+SSR | PID+PWM控制 |
| 水泵/电机 | GPIO+PWM | PWM调速 |
| 电磁阀 | GPIO | 开关控制 |
| 显示屏 | UART / SPI / HDMI | 触摸屏交互 |
| 蓝牙 | UART | BLE近场配置 |

**需求描述：**

1. 传感器/执行器驱动提供统一抽象接口（open/read/write/ioctl/close）
2. 加热器PID控制参数可云端调整
3. 执行器超时保护

---

## 5. 非功能性需求

### 5.1 性能

| 指标 | 目标值 |
|------|--------|
| 系统冷启动时间 | ≤ 30秒 |
| MQTT消息延迟 | ≤ 500ms |
| 触摸响应延迟 | ≤ 100ms |
| 紧急停止响应 | ≤ 200ms |
| CPU使用率（空闲） | ≤ 10% |
| CPU使用率（制作中） | ≤ 50% |
| 内存使用峰值 | ≤ 70% |

### 5.2 可靠性

| 指标 | 目标值 |
|------|--------|
| 系统可用性 | ≥ 99.9% |
| 单杯制作成功率 | ≥ 99.5% |
| OTA升级成功率 | ≥ 99% |
| 离线运行能力 | ≥ 7天 |
| 数据上报完整率 | ≥ 99.9% |

**额外需求：**

1. 软件Watchdog，死锁时自动复位
2. 关键数据（订单/配置）掉电保护
3. 软件异常崩溃后自动重启
4. 支持A/B系统分区，主分区损坏自动切换

### 5.3 安全

1. 设备与云平台mTLS双向认证
2. 设备凭证（证书/Token）存储在安全存储区，不可导出
3. 所有网络通信强制TLS 1.2+加密
4. 固件OTA包数字签名验证
5. 安全启动（Secure Boot），校验固件签名
6. 本地操作权限分级（消费者/运营人员/运维工程师）
7. 远程指令执行需设备端授权确认
8. 机箱开门检测告警
9. 账号密码强度策略：8字符以上，含大小写字母+数字
10. 登录失败锁定：连续5次失败锁定30分钟
11. JWT Token有效期默认24小时（可配置），支持主动登出失效
12. API鉴权中间件：每个请求校验Token和权限码
13. 跨组织数据隔离：API层自动过滤非授权数据
14. 敏感操作审计日志（账号创建/删除、权限变更、组织变更）

### 5.4 可维护性

1. 模块低耦合，可独立编译测试
2. 日志级别：DEBUG/INFO/WARN/ERROR
3. 日志本地滚动存储（单文件 ≤ 50MB，保留10个）
4. 配置支持云端下发，无需重启生效
5. 支持远程日志查看和基础诊断

---

## 6. 数据存储

| 数据类别 | 存储周期 | 容量上限 |
|---------|---------|---------|
| 组织架构 | 永久 | 1MB |
| 账号信息 | 永久 | 1MB |
| 审计日志 | 365天 | 50MB |
| 设备配置 | 永久 | 1MB |
| 配方数据 | 跟随云端更新 | 10MB |
| 订单记录 | 30天 | 50MB |
| 运行日志 | 7天滚动 | 100MB |
| 故障日志 | 90天 | 50MB |
| 固件升级包 | 升级后删除旧版 | 200MB |

- 采用SQLite3嵌入式数据库
- 存储空间不足时优先清理过期数据
- 关键配置（设备ID、证书、配方）加密备份至独立分区

---

## 7. 附录

### 7.1 设备状态码

设备状态分为**网络状态**和**工作状态**两个独立维度。

**网络状态 (network_status)** — 由 MQTT 连接决定：

| 状态码 | 名称 | 说明 |
|--------|------|------|
| 0 | ONLINE | MQTT已连接，设备在线 |
| 1 | OFFLINE | MQTT断开，设备离线 |

**工作状态 (work_status)** — 由设备自身业务决定：

| 状态码 | 名称 | 说明 |
|--------|------|------|
| 0 | IDLE | 空闲待机 |
| 1 | BREWING | 制作中 |
| 2 | FAULT | 故障 |
| 3 | UPGRADING | 固件升级中 |
| 4 | MAINTENANCE | 维护模式 |

**心跳包字段更新：**

| 字段 | 说明 |
|------|------|
| device_id | 设备ID |
| timestamp | 当前时间戳 |
| network_status | 网络状态：0=在线, 1=离线 |
| work_status | 工作状态：0=空闲, 1=制作中, 2=故障, 3=升级中, 4=维护 |
| firmware_version | 固件版本 |
| signal_strength | 网络信号强度 |
| alarm_count | 当前告警计数 |

**状态判定规则：**
- MQTT Will Message 触发 → network_status = OFFLINE
- 设备主动上报心跳 → network_status = ONLINE
- work_status 仅由设备端业务逻辑变更，不受网络状态影响

### 7.2 故障码（部分）

| 故障码 | 名称 | 级别 | 说明 |
|--------|------|------|------|
| F001 | HEATER_OVERTEMP | L4 | 加热器超温 |
| F002 | HEATER_NOT_HEATING | L3 | 加热器不加热 |
| F003 | PUMP_FAILURE | L3 | 水泵故障 |
| F004 | MOTOR_STALL | L3 | 电机堵转 |
| F005 | WATER_LEAK | L4 | 漏水检测 |
| F006 | SENSOR_OFFLINE | L2 | 传感器离线 |
| F007 | COMM_FAIL | L2 | 通信模块异常 |
| F008 | MATERIAL_LOW | L1 | 原料不足 |

### 7.3 订单状态码

| 状态码 | 名称 | 说明 |
|--------|------|------|
| 0 | PENDING | 待支付 |
| 1 | PAID | 已支付，待制作 |
| 2 | BREWING | 制作中 |
| 3 | COMPLETED | 已完成 |
| 4 | CANCELLED | 已取消 |
| 5 | FAILED | 制作失败 |

### 7.4 开发技术栈建议

| 层次 | 推荐 |
|------|------|
| 操作系统 | Yocto Linux / FreeRTOS |
| 编程语言 | C++17 / C |
| 构建系统 | CMake |
| MQTT客户端 | Eclipse Paho |
| HTTP客户端 | libcurl |
| 数据库 | SQLite3 |
| 序列化 | JSON + Protobuf |
| 加密库 | mbedTLS / OpenSSL |
| 密码哈希 | bcrypt / argon2 |
| JWT | cpp-jwt / nlohmann-json |

---

### 7.5 角色码

| 角色编码 | 名称 | 权限范围 | 可创建角色 | 说明 |
|----------|------|---------|-----------|------|
| super_admin | 超级管理员 | 全局 | 所有角色 | 系统初始化时创建，通常1-2个 |
| org_admin | 组织管理员 | 本组织及子组织 | org_admin/dept_admin/operator/viewer | 可管理下属部门和账号 |
| dept_admin | 部门管理员 | 本部门及子部门 | operator/viewer | 日常设备和数据管理 |
| operator | 运营人员 | 本部门 | viewer | 日常操作，可调整配方 |
| viewer | 只读用户 | 本部门 | 无 | 仅查看，不可修改 |

### 7.6 权限码

| 权限码 | 资源 | 操作 | 依赖 |
|--------|------|------|------|
| org:read | 组织 | 查看组织树和详情 | 无 |
| org:write | 组织 | 创建/修改/删除组织 | org:read |
| account:read | 账号 | 查看账号列表和详情 | org:read |
| account:write | 账号 | 创建/修改/删除账号 | account:read |
| device:read | 设备 | 查看设备列表和详情 | org:read |
| device:write | 设备 | 修改配置/远程指令 | device:read |
| order:read | 订单 | 查看订单列表和详情 | device:read |
| recipe:read | 配方 | 查看配方 | 无 |
| recipe:write | 配方 | 创建/修改/删除配方 | recipe:read |
| ota:read | OTA | 查看固件和升级状态 | 无 |
| ota:write | OTA | 注册固件/推送升级 | ota:read |
| fault:read | 故障 | 查看故障列表 | device:read |
| fault:write | 故障 | 确认/解决故障 | fault:read |
| config:read | 配置 | 查看服务配置 | 无 |
| config:write | 配置 | 修改服务配置 | config:read |

### 7.7 通用响应码（补充）

| code | 说明 |
|------|------|
| 0 | 成功 |
| 1001 | 参数校验失败 |
| 1002 | 资源不存在 |
| 1003 | 资源冲突（重复/有关联数据） |
| 1004 | 未授权/Token过期 |
| 1005 | 权限不足（403 Forbidden） |
| 1006 | 账号已锁定 |
| 1007 | 密码错误 |
| 2000 | 服务端内部错误 |

---

---

## 8. 附录：新设备上线完整流程

### 8.1 流程概览

一台新设备从出厂到正常运行，经历以下阶段：

```
出厂预置 → 首次上电 → HTTP激活 → MQTT连接 → 正常运行 → OTA升级
  │            │          │           │           │           │
  │ model_code │ 生成     │ 获取      │ 订阅      │ 心跳      │ 检查更新
  │ HW UID     │ device   │ device_id │ downlink  │ 属性上报  │ HTTP下载
  │ 证书       │ _id      │ token     │ topics    │ 事件上报  │ 校验安装
  └────────────┴──────────┴───────────┴───────────┴───────────┴──────────┘
```

### 8.2 阶段0：出厂预置

设备出厂时已在安全存储区烧录：

| 数据 | 说明 | 示例 |
|------|------|------|
| `hardware_uid` | 芯片唯一序列号（Secure Element / eFuse） | `STM32F4-0x1FFF7A10-07123456` |
| `model_key` | 设备型号Key（26字符ULID，云端创建型号时自动生成） | `01KX5M2KM8EBW9G1CWVMJ94TSK` |

### 8.3 阶段1：HTTP设备激活

设备首次上电，只需 2 个参数（uid + model_key）。云端根据 model_key 自动解析设备型号、类型、固件版本。

```
POST /api/v1/device/activate
Content-Type: application/json

{
  "uid":       "STM32F4-0x1FFF7A10-07123456",
  "model_key": "01KX5M2KM8EBW9G1CWVMJ94TSK"
}
```

响应：
```json
{
  "success": true,
  "device_id":         "dev_a1b2c3d4e5f6",
  "model_key":         "01KX5M2KM8EBW9G1CWVMJ94TSK",
  "model_code":        "ACM-200",
  "tenant_id":         "default",
  "product_id":        "coffee_v1",
  "device_type":       "coffee_machine",
  "firmware_version":  "v2.0.0",
  "activation_token":  "a1b2c3d4...（256位随机hex）",
  "mqtt_broker_uri":   "ssl://mqtt.example.com:8883",
  "ttl_seconds":       31536000
}
```

**激活流程：** `uid` 唯一标识设备 → `model_key` 查型号表 → 解析 type_code → 查类型表 → 得 product_id + firmware_base → 分配 device_id + token → 存入 devices 表

**通信通道：HTTPS**（设备尚未有MQTT凭证）

### 8.4 阶段2：MQTT连接与订阅

设备用激活返回的 `activation_token` 作为 MQTT 密码，通过 mTLS 连接 Broker：

```
连接参数：
  Broker:   ssl://mqtt.example.com:8883
  ClientID: dev_a1b2c3d4e5f6
  Username: dev_a1b2c3d4e5f6
  Password: a1b2c3d4...（activation_token）
  CA证书:   出厂预置 ca.pem
  客户端证书: 出厂预置 device_cert.pem
  客户端密钥: 出厂预置 device_key.pem
  KeepAlive: 60秒
  CleanStart: false
  Will Topic: company_zhangsan/iot/coffee_v1/dev_a1b2c3d4e5f6/status
  Will Payload: {"status":"offline"}
```

连接成功后订阅下行 Topic：

| 订阅 Topic | QoS | 用途 |
|-----------|-----|------|
| `{device}/v1/property/set` | 1 | 接收云端配置下发 |
| `{device}/v1/ota/notify` | 1 | 接收OTA升级通知 |
| `{device}/v1/command/+` | 1 | 接收远程指令（重启/维护/锁定） |

**通信通道：MQTT (mTLS)**

### 8.5 阶段3：正常运行

设备进入正常运行循环，以下操作并行执行：

#### 8.5.1 心跳上报（MQTT，60秒周期）

```
Publish: {device}/v1/heartbeat  QoS 1
```

```json
{
  "device_id":        "dev_a1b2c3d4e5f6",
  "timestamp":        "2026-07-10T08:30:00Z",
  "status":           0,
  "firmware_version": "v1.0.0",
  "signal_strength":  85,
  "alarm_count":      0
}
```

#### 8.5.2 属性上报（MQTT，按需/周期）

```
Publish: {device}/v1/property/post  QoS 1
```

```json
{
  "cpu_temp_c":     52.3,
  "water_temp_c":   88.5,
  "bean_remaining_g": 320,
  "uptime_seconds":  3600
}
```

#### 8.5.3 事件上报（MQTT，实时）

订单状态变更：
```
Publish: {device}/v1/event/post  QoS 1
```
```json
{
  "event_type": "order_status",
  "order_id":   "ORD-20260710-001",
  "status":     3,
  "timestamp":  "2026-07-10T08:35:00Z"
}
```

故障告警：
```json
{
  "event_type":    "fault_alert",
  "fault_code":    3,
  "fault_level":   3,
  "description":   "水泵流量异常",
  "timestamp":     "2026-07-10T08:40:00Z",
  "sensor_snapshot": {"flow_rate_ml_s": 2.5}
}
```

#### 8.5.4 配置同步（HTTP，启动时+按需）

```
GET /api/v1/device/config
Authorization: Bearer {activation_token}
```

设备启动时主动拉取配置，收到 MQTT `property/set` 时热更新。

#### 8.5.5 配方同步（HTTP，启动时+按需）

```
GET /api/v1/recipes/sync
Authorization: Bearer {activation_token}
```

设备缓存全部在线配方，支持增量同步。

**通信通道：MQTT（实时数据） + HTTP（配置/配方拉取）**

### 8.6 阶段4：OTA固件升级

```
云端上传新固件 → 通知设备(MQTT) → 设备下载(HTTP) → 校验安装 → 上报结果(MQTT)
```

#### Step 1: 云端推送升级通知（MQTT）

设备收到 Broker 推送：
```
Receive: {device}/v1/ota/notify  QoS 1
```
```json
{
  "version":     "v2.0.0",
  "download_url":"http://cloud:9080/api/v1/ota/firmwares/download/coffee_v1/v2.0.0.ipk",
  "checksum":    "a1b2c3d4...（SHA256）",
  "force_upgrade": false
}
```

#### Step 2: 设备下载固件（HTTP，支持断点续传）

```
GET /api/v1/ota/firmwares/download/coffee_v1/v2.0.0.ipk
Range: bytes=5242880-    （续传时携带）
```

#### Step 3: 校验安装

下载完成后：
1. 校验 SHA256
2. 等待设备空闲（无订单制作中）
3. 备份当前固件 → 安装新固件
4. 重启设备 → 功能自检

#### Step 4: 进度上报（MQTT）

```
Publish: {device}/v1/ota/progress  QoS 1
```

```json
{"version":"v2.0.0","progress":45,"stage":"downloading"}
{"version":"v2.0.0","progress":100,"stage":"downloading"}
{"version":"v2.0.0","progress":0,"stage":"installing"}
{"version":"v2.0.0","progress":100,"stage":"done"}
```

安装失败时：
```json
{"version":"v2.0.0","stage":"failed","error":"checksum mismatch"}
```

**通信通道：MQTT（通知+进度） + HTTP（固件下载）**

### 8.7 已激活设备重新上线

设备已激活过（本地存有 `device_id` + `activation_token`），再次上电时**跳过 HTTP 激活**，直接进入 MQTT 连接：

```
设备上电 → 检测本地已存有凭证 → MQTT直连 → 订阅下行Topic → 心跳 → 云端识别在线
```

```
设备                            dev-sys-cloud
 │                                    │
 ├─ 读取本地 device_id + token        │
 ├─ MQTT CONNECT ────────────────────→│ 收到连接
 │   clientId: dev_a1b2c3d4e5f6       │
 │   password: a1b2c3d4...(token)     │ 验证token有效性
 │                                    │ 更新设备状态 → ONLINE
 │←── CONNACK ────────────────────────│
 │                                    │
 ├─ SUBSCRIBE property/set ──────────→│
 ├─ SUBSCRIBE ota/notify ────────────→│
 ├─ SUBSCRIBE command/+ ─────────────→│
 │                                    │
 ├─ PUBLISH heartbeat ───────────────→│ 更新 last_heartbeat_at
 │   {status:0, fw_ver:...}          │
 │                                    │
 ├─ HTTP GET /config ────────────────→│ 拉取最新配置
 ├─ HTTP GET /recipes ───────────────→│ 拉取配方
 │                                    │
 ├─ 正常运行（心跳+属性+事件）          │
 ╞════════════════════════════════════╡
 │ 异常掉线                            │
 │←── Will Message ───────────────────│ Broker发布遗嘱 → 云端收到offline通知
 │                                    │ 更新设备状态 → OFFLINE
 │                                    │ 启动离线计时器
 │                                    │
 │ 重连（指数退避1s→2s→4s→...→60s）    │
 ├─ MQTT CONNECT ────────────────────→│ 更新设备状态 → ONLINE
 │   (同上流程)                        │ 补传离线期间缓存的数据
```

**与首次激活的区别：**

| 场景 | 首次激活 | 已激活重新上线 |
|------|---------|---------------|
| HTTP `/activate` | 调用 | **跳过** |
| MQTT凭证 | 激活返回的 token | 本地存储的 token |
| 设备状态 | INSERT devices 表 | UPDATE last_heartbeat_at |
| 凭证过期 | 不适用 | token 有效期1年，过期需重新激活 |
| 云端校验 | `uid` 查是否已注册 | `device_id` + `token` 验证 |
| 配置/配方 | 启动拉取 | 启动拉取，有变更时增量 |

**Token 过期处理：**

```
设备 MQTT CONNECT → Broker ACL 校验 token → 过期拒绝 → 
设备收到 CONNACK(5=Not Authorized) → 回退到阶段1: HTTP /activate 重新激活
```

重新激活时，云端检测 `uid` 已存在 → 返回已有的 `device_id` + 新 `token`，不创建新设备记录。

### 8.8 通道职责总结

| 阶段 | 操作 | 通道 | 原因 |
|------|------|------|------|
| 激活 | 设备注册 | HTTP | 一次性请求，需获取凭证 |
| 连接 | MQTT Connect + 订阅 | MQTT (mTLS) | 长连接双向通信 |
| 心跳 | 周期上报 | MQTT | 高频小数据，需要服务端实时感知 |
| 属性 | 传感器数据 | MQTT | 高频，允许丢失偶发数据(QoS 1) |
| 事件 | 订单/故障 | MQTT | 需要服务端即时响应 |
| 配置 | 拉取+下发 | HTTP拉 + MQTT推 | 启动拉取，运行中推送 |
| 配方 | 同步 | HTTP | 数据量大，按需拉取 |
| OTA通知 | 云端→设备 | MQTT | 需要即时推送 |
| OTA下载 | 固件包 | HTTP (Range) | 大文件(可达数百MB)，需断点续传 |
| OTA进度 | 设备→云端 | MQTT | 实时进度，小数据 |
| 远程指令 | 云端→设备 | MQTT | 需要即时送达 |

> **文档状态：修订中（V2.2）**
>
> 本文档定义了IoT云平台服务程序的核心功能需求，覆盖：多层级组织管理、RBAC账号权限体系、设备类型与型号管理、订单处理、OTA升级编排、故障检测与告警。
> 后续可根据实际业务需要扩展支付集成、库存管理、清洗维护、数据分析等高级功能模块。
