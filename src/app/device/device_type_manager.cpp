#include "device_type_manager.h"
#include "middleware/storage/database.h"
#include <iostream>

namespace dev_sys {

DeviceTypeManager::DeviceTypeManager(Database& db) : db_(db) {}
DeviceTypeManager::~DeviceTypeManager() = default;

// ============================================================
// Device Types
// ============================================================
std::vector<DeviceTypeInfo> DeviceTypeManager::list_types(bool active_only) {
    return db_.list_device_types(active_only);
}

std::optional<DeviceTypeInfo> DeviceTypeManager::get_type(const std::string& type_code) {
    return db_.get_device_type(type_code);
}

StatusCode DeviceTypeManager::create_type(const DeviceTypeInfo& info) {
    if (info.type_code.empty() || info.display_name.empty()) {
        return StatusCode::ERROR;
    }
    if (type_exists(info.type_code)) {
        std::cerr << "[TypeMgr] Type code already exists: " << info.type_code << std::endl;
        return StatusCode::DEV_ALREADY_ACTIVE; // reuse "already exists" code
    }
    return db_.insert_device_type(info);
}

StatusCode DeviceTypeManager::update_type(const std::string& type_code,
                                           const DeviceTypeInfo& info) {
    if (!type_exists(type_code)) {
        return StatusCode::RECIPE_NOT_FOUND; // reuse "not found"
    }
    return db_.update_device_type(info);
}

StatusCode DeviceTypeManager::delete_type(const std::string& type_code) {
    // Don't allow deleting "other" default
    if (type_code == "other") {
        return StatusCode::ERROR;
    }
    // Check if any models reference this type
    auto models = db_.list_device_models(type_code, true);
    if (!models.empty()) {
        std::cerr << "[TypeMgr] Cannot delete type with active models: " << type_code
                  << " (" << models.size() << " model(s) associated)" << std::endl;
        return StatusCode::ORG_HAS_DEVICES; // reuse: "Organization has associated devices"
    }
    return db_.delete_device_type(type_code);
}

// ============================================================
// Device Models
// ============================================================
std::vector<DeviceModelInfo> DeviceTypeManager::list_models(const std::string& type_code,
                                                              bool active_only) {
    return db_.list_device_models(type_code, active_only);
}

std::optional<DeviceModelInfo> DeviceTypeManager::get_model(const std::string& model_code) {
    return db_.get_device_model(model_code);
}

StatusCode DeviceTypeManager::create_model(const DeviceModelInfo& info) {
    if (info.model_code.empty() || info.display_name.empty() || info.type_code.empty()) {
        return StatusCode::ERROR;
    }
    if (!type_exists(info.type_code)) {
        std::cerr << "[TypeMgr] Parent type not found: " << info.type_code << std::endl;
        return StatusCode::RECIPE_NOT_FOUND;
    }
    if (model_exists(info.model_code)) {
        return StatusCode::DEV_ALREADY_ACTIVE;
    }
    return db_.insert_device_model(info);
}

StatusCode DeviceTypeManager::update_model(const std::string& model_code,
                                            const DeviceModelInfo& info) {
    if (!model_exists(model_code)) {
        return StatusCode::RECIPE_NOT_FOUND;
    }
    if (!type_exists(info.type_code)) {
        return StatusCode::RECIPE_NOT_FOUND;
    }
    return db_.update_device_model(info);
}

StatusCode DeviceTypeManager::delete_model(const std::string& model_code) {
    return db_.delete_device_model(model_code);
}

// ============================================================
// Validation
// ============================================================
bool DeviceTypeManager::type_exists(const std::string& type_code) const {
    return db_.get_device_type(type_code).has_value();
}

bool DeviceTypeManager::model_exists(const std::string& model_code) const {
    return db_.get_device_model(model_code).has_value();
}

} // namespace dev_sys
