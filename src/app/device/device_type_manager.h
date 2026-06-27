#pragma once

#include "dev_sys/common/types.h"
#include "dev_sys/common/status_codes.h"
#include <memory>
#include <vector>
#include <string>

namespace dev_sys {

class Database;

// ============================================================
// DeviceTypeManager — 设备类型/型号动态管理 (CRUD)
//
// 后端提供给前端的管理接口：
//   类型:  GET    /api/v1/device-types         列表
//          POST   /api/v1/device-types         新增
//          PUT    /api/v1/device-types/{code}   更新
//          DELETE /api/v1/device-types/{code}   删除(软删除)
//
//   型号:  GET    /api/v1/device-models         列表 (?type_code=xxx)
//          POST   /api/v1/device-models         新增
//          PUT    /api/v1/device-models/{code}   更新
//          DELETE /api/v1/device-models/{code}   删除(软删除)
// ============================================================
class DeviceTypeManager {
public:
    explicit DeviceTypeManager(Database& db);
    ~DeviceTypeManager();

    // ======== Device Types ========
    std::vector<DeviceTypeInfo> list_types(bool active_only = false);
    std::optional<DeviceTypeInfo> get_type(const std::string& type_code);
    StatusCode create_type(const DeviceTypeInfo& info);
    StatusCode update_type(const std::string& type_code, const DeviceTypeInfo& info);
    StatusCode delete_type(const std::string& type_code);

    // ======== Device Models ========
    std::vector<DeviceModelInfo> list_models(const std::string& type_code = "",
                                              bool active_only = false);
    std::optional<DeviceModelInfo> get_model(const std::string& model_code);
    StatusCode create_model(const DeviceModelInfo& info);
    StatusCode update_model(const std::string& model_code, const DeviceModelInfo& info);
    StatusCode delete_model(const std::string& model_code);

    // ======== Validation ========
    bool type_exists(const std::string& type_code) const;
    bool model_exists(const std::string& model_code) const;

private:
    Database& db_;
};

} // namespace dev_sys
