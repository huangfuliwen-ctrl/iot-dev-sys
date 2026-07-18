#pragma once

#include "dev_sys/common/types.h"
#include "dev_sys/common/status_codes.h"
#include "org_manager.h"
#include <memory>
#include <vector>
#include <string>
#include <mutex>

namespace dev_sys {

// ============================================================
// Account Manager — 账号管理与JWT认证 (REQ-ORG-002/004/005)
// ============================================================
class AccountManager {
public:
    AccountManager(OrgManager* org_mgr);
    ~AccountManager();

    void set_database(Database* db) { db_ = db; }
    StatusCode load_from_database();

    // CRUD
    StatusCode create_account(AccountInfo& info, const std::string& password);
    StatusCode update_account(int32_t account_id, const AccountInfo& info);
    StatusCode change_password(int32_t account_id,
                               const std::string& old_password,
                               const std::string& new_password);
    StatusCode delete_account(int32_t account_id);

    // Auth
    LoginResponse login(const LoginRequest& req);
    StatusCode logout(const std::string& token);
    std::optional<TokenPayload> verify_token(const std::string& token) const;

    // Query
    std::optional<AccountInfo> get_account(int32_t account_id) const;
    std::optional<AccountInfo> get_account_by_username(const std::string& username) const;
    std::vector<AccountInfo> list_accounts(int32_t org_id = -1,
                                            const std::string& role_code = "",
                                            bool active_only = true) const;
    int32_t account_count() const;

    // Permissions
    static std::vector<std::string> permissions_for_role(const std::string& role_code);

    // Mock data
    void seed_mock_data();

private:
    std::string generate_token(const AccountInfo& account, const std::vector<int32_t>& org_scope);
    std::string hash_password(const std::string& password) const;  // mock: simple hash

    OrgManager* org_mgr_;
    Database* db_ = nullptr;
    mutable std::recursive_mutex mutex_;
    std::vector<AccountInfo> accounts_;
    std::vector<std::string> invalidated_tokens_;  // logout blacklist
    int32_t next_id_ = 1;
    int32_t failed_attempts_ = 0;  // per-account tracking simplified
};

} // namespace dev_sys
