#pragma once

#include "dev_sys/common/types.h"
#include "dev_sys/common/status_codes.h"
#include <memory>
#include <string>
#include <vector>
#include <optional>

namespace dev_sys {

// ============================================================
// Database (SQLite3 based)
// ============================================================
class Database {
public:
    Database();
    ~Database();

    StatusCode open(const std::string& db_path);
    StatusCode close();
    bool is_open() const;

    // ======== Generic query ========
    StatusCode execute(const std::string& sql);
    StatusCode query(const std::string& sql,
                     std::vector<std::vector<std::string>>& rows);

    // ======== Device CRUD (REQ-DM-002) ========
    StatusCode insert_device(const Device& device);
    StatusCode update_device(const Device& device);
    StatusCode upsert_device(const Device& device);
    std::optional<Device> get_device(const std::string& device_id);
    std::optional<Device> get_device_by_hwuid(const std::string& hardware_uid);
    std::vector<Device> list_devices_by_tenant(const std::string& tenant_id);
    std::vector<Device> list_all_devices();
    bool device_exists(const std::string& hardware_uid) const;

    // ======== Activation token ========
    StatusCode store_activation_token(const std::string& device_id,
                                       const std::string& token,
                                       int64_t issued_at,
                                       int64_t expires_at);
    std::optional<std::string> get_activation_token(const std::string& device_id);

    // ======== Audit log ========
    StatusCode log_activation(const std::string& device_id,
                               const std::string& hardware_uid,
                               const std::string& remote_ip,
                               bool success,
                               const std::string& message);

    // ======== Device Types CRUD (dynamic, frontend-managed) ========
    StatusCode insert_device_type(const DeviceTypeInfo& info);
    StatusCode update_device_type(const DeviceTypeInfo& info);
    StatusCode delete_device_type(const std::string& type_code);
    std::optional<DeviceTypeInfo> get_device_type(const std::string& type_code);
    std::vector<DeviceTypeInfo> list_device_types(bool active_only = false);

    // ======== Device Models CRUD (dynamic, frontend-managed) ========
    StatusCode insert_device_model(const DeviceModelInfo& info);
    StatusCode update_device_model(const DeviceModelInfo& info);
    StatusCode delete_device_model(const std::string& model_code);
    std::optional<DeviceModelInfo> get_device_model(const std::string& model_code);
    std::vector<DeviceModelInfo> list_device_models(const std::string& type_code = "",
                                                     bool active_only = false);

    // ======== Order CRUD ========
    StatusCode insert_order(const Order& order);
    StatusCode update_order_status(const std::string& order_id, int status);
    std::optional<Order> get_order(const std::string& order_id);
    std::vector<Order> list_all_orders();

    // ======== Recipe CRUD ========
    StatusCode insert_recipe(const Recipe& recipe);
    StatusCode update_recipe(const Recipe& recipe);
    StatusCode delete_recipe(const std::string& recipe_id);
    std::optional<Recipe> get_recipe(const std::string& recipe_id);
    std::vector<Recipe> list_all_recipes();

    // ======== Fault CRUD ========
    StatusCode insert_fault(const FaultInfo& fault);
    StatusCode resolve_fault(const std::string& tenant_id, const std::string& device_id, int code);
    std::vector<FaultInfo> list_all_faults();
    std::vector<FaultInfo> list_active_faults();
    std::vector<FaultInfo> list_faults_by_device(const std::string& tenant_id, const std::string& device_id);

    // ======== Firmware CRUD ========
    StatusCode insert_firmware(const FirmwareVersion& fw);
    StatusCode delete_firmware(const std::string& version);
    std::optional<FirmwareVersion> get_firmware(const std::string& version);
    std::vector<FirmwareVersion> list_all_firmwares();

    // ======== Maintenance ========
    StatusCode vacuum();
    StatusCode clean_expired_data(int retention_days);
    StatusCode backup(const std::string& backup_path);

private:
    StatusCode init_schema();
    StatusCode migrate_if_needed();
    StatusCode create_devices_table();
    StatusCode create_activation_tokens_table();
    StatusCode create_audit_log_table();
    StatusCode create_tenants_table();
    StatusCode create_device_types_table();
    StatusCode create_device_models_table();
    StatusCode create_orders_table();
    StatusCode create_recipes_table();
    StatusCode create_faults_table();
    StatusCode create_firmwares_table();

    Device device_from_row(const std::vector<std::string>& row) const;
    DeviceTypeInfo  type_from_row(const std::vector<std::string>& row) const;
    DeviceModelInfo model_from_row(const std::vector<std::string>& row) const;
    Order  order_from_row(const std::vector<std::string>& row) const;
    Recipe recipe_from_row(const std::vector<std::string>& row) const;
    FaultInfo fault_from_row(const std::vector<std::string>& row) const;
    FirmwareVersion firmware_from_row(const std::vector<std::string>& row) const;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dev_sys
