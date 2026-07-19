#pragma once

#include "dev_sys/common/types.h"
#include "dev_sys/common/status_codes.h"
#include <memory>
#include <string>
#include <functional>

namespace dev_sys {

class Database;
class TlsManager;

// ============================================================
// Device Activation Service (REQ-DM-002)
//
// 处理设备首次注册激活流程:
//   1. 接收设备激活请求(HTTPS)
//   2. 校验硬件唯一ID, 检查是否已激活
//   3. 生成device_id, activation_token, 可选mTLS证书
//   4. 存储设备记录到DB
//   5. 返回激活凭证给设备
// ============================================================
class DeviceActivation {
public:
    explicit DeviceActivation(Database& db);
    ~DeviceActivation();

    // Set TLS manager for certificate generation (optional, for mTLS)
    void set_tls_manager(TlsManager* tls);
    // Set MQTT broker URI for activation responses
    void set_broker_uri(const std::string& uri) { broker_uri_ = uri; }
    void set_default_tenant(const std::string& t) { default_tenant_ = t; }

    // ======== Core: process an activation request ========
    ActivationResponse process_activation(const ActivationRequest& request,
                                           const std::string& remote_ip = "");

    // ======== Token management ========
    StatusCode revoke_token(const std::string& device_id);
    StatusCode renew_token(const std::string& device_id, ActivationResponse& response);

    // ======== Query ========
    bool is_device_activated(const std::string& hardware_uid) const;
    std::optional<Device> get_device_by_hwuid(const std::string& hardware_uid) const;

    // ======== Re-activation (device reset / re-provision) ========
    ActivationResponse reactivate_device(const std::string& hardware_uid,
                                          const std::string& remote_ip = "");

private:
    std::string generate_device_id(const ActivationRequest& request);
    std::string generate_activation_token(const std::string& device_id);
    std::string generate_product_id(DeviceType type);
    std::string generate_certificate(const std::string& device_id);     // optional mTLS
    std::string generate_private_key(const std::string& device_id);     // optional mTLS

    StatusCode validate_request(const ActivationRequest& request) const;

    Database&    db_;
    TlsManager*  tls_mgr_ = nullptr;
    std::string  broker_uri_ = "tcp://127.0.0.1:1883";
    std::string  default_tenant_ = "default";
};

} // namespace dev_sys
